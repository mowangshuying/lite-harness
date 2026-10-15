#include "QOpenAi.h"
#include "LiteLog.h"

#include "AgentConstants.h" // 流式总时长哨兵上限单源（kStreamTotalTimeoutMs）+ s15 恢复常量（kMaxConsecutive529）
#include "AppSettings.h"    // settings.ini 单源（apiBaseUrl / apiToken 两个键）
#include "AgentRecovery.h"  // s15 过载文本判据单源（RecoveryRules::isOverloadedText，lcc s15 af26862）

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QByteArray>
#include <QTimer>
#include <QUrl>
#include <QDebug>

namespace {

class OpenAiClient
{
public:
    OpenAiClient() = default;

    QNetworkAccessManager manager;
    QString url;
    QString token;
    int maxRetries = AgentConst::kMaxRetriesDefault; // 最大重试次数（仅 5xx / 429）；启动由 initFromSettings 读 settings.ini 覆盖
    int streamIdleTimeout = 60000;  // 流式静默超时（毫秒）：每收到数据即重置，服务器持续有输出则总时长不限；<=0 不限时
    // 流式总时长哨兵（毫秒）：idle 每收字节即重置，杀不死"慢而不断"的流（持续有字节、
    // 永不发 [DONE]），需总量防线——与 AsyncRequest 的总超时同机制；取值进 AgentConstants.h
    int streamTotalTimeout = AgentConst::kStreamTotalTimeoutMs;
    QString fallbackModelId;     // s15 过载换模后备模型（空=禁用）；启动由 initFromSettings 读 settings.ini 键 fallbackModelId 注入
    bool verbose = false;        // 调试日志开关
};

OpenAiClient &client()
{
    static OpenAiClient c;
    return c;
}

// 仅 5xx 与 429 可重试
bool isRetryableStatus(int httpStatus)
{
    return httpStatus == 429 || httpStatus >= 500;
}

// 指数退避：第 n 次重试（从 1 起）等待 1000 * 2^(n-1) 毫秒（1s, 2s, 4s...）。
// 指数钳制到 [0,6]（最大 64s）：n 过大时 1<<(n-1) 位移 ≥32 为 UB，负数位移同为 UB
int retryDelayMs(int retryNumber)
{
    const int exponent = qBound(0, retryNumber - 1, 6);
    return 1000 * (1 << exponent);
}

// 服务端错误体解析（4xx 硬错误与 5xx/429 重试耗尽两条路径共用）：合并全部残体来源
// ——pendingFrame 存已成行但未遇帧边界派发的行（错误体单行带尾部 '\n' 时全文在此）、
// buffer 存未成行残段、readAll 取尚未交付的尾巴（故须在 cleanupReply 前调用）。
// 修3 结论沿用：errorString() 对 4xx 常是 "Unknown error"，可判定信息只在响应体里。
// JSON 体取 error.message（有 code 则附 [code]）；非 JSON 体截 400 字节直呈（utf8 断字符
// 由替换符兜底，诊断用途可接受）；体为空回退 fallback。
QString parseErrorDetail(const QByteArray &pendingFrame, const QByteArray &buffer,
                         QNetworkReply *reply, const QString &fallback)
{
    QByteArray body = pendingFrame;
    if (!body.isEmpty() && !buffer.isEmpty())
        body += '\n';
    body.append(buffer);
    if (reply)
        body.append(reply->readAll());
    const QByteArray errBody = body.trimmed();
    const QJsonDocument errDoc = QJsonDocument::fromJson(errBody);
    if (errDoc.isObject())
    {
        const QJsonObject errObj = errDoc.object().value(QStringLiteral("error")).toObject();
        const QString msg = errObj.value(QStringLiteral("message")).toString();
        const QString code = errObj.value(QStringLiteral("code")).toString();
        if (!msg.isEmpty())
            return code.isEmpty() ? msg : msg + QStringLiteral(" [") + code + QStringLiteral("]");
    }
    else if (!errBody.isEmpty())
    {
        return QString::fromUtf8(errBody.left(400));
    }
    return fallback;
}

// base URL 归一：去掉全部尾部斜杠（endpointFor 与 setUrl 共用，消除两份重复实现）
QString trimTrailingSlashes(const QString &url)
{
    QString base = url;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base;
}

// 由基础 URL（不含端点路径）拼出具体端点
QString endpointFor(OpenAiClient &c, QOpenAi::ChatStream::Mode mode)
{
    const char *suffix = mode == QOpenAi::ChatStream::Mode::LegacyCompletion
        ? "/completions"
        : "/chat/completions";
    return trimTrailingSlashes(c.url) + QString::fromLatin1(suffix);
}

} // namespace

namespace QOpenAi {

class ChatStream::Private
{
public:
    QNetworkReply *reply = nullptr;
    QByteArray buffer;         // SSE 累积缓冲（仅存未成行的残段）
    QByteArray pendingFrame;   // 当前帧已收集的行（遇空行=帧边界时整帧派发）
    QString thinking;          // 累积推理原文
    QString content;           // 累积正文原文
    QJsonArray toolCalls;      // 累积的 tool_calls（按 index 对齐，含占位）
    bool done = false;         // 是否已收尾（防重复 emit）
    bool timedOut = false;     // 是否超时中止（idle 静默防线）
    bool totalTimedOut = false; // 是否总时长哨兵中止（"慢而不断"防线，收字节不重置）
    Mode mode = Mode::Chat;    // 流处理模式（决定端点与 SSE 解析分支）
    QJsonObject input;         // 请求体，供重试复用
    int retriesLeft = 0;       // 剩余可重试次数
    QTimer *timeoutTimer = nullptr;
    QTimer *totalTimer = nullptr;  // 总时长哨兵（对照 AsyncRequest::m_totalTimer 同款机制）
    // 修3（usage 回读）：stream_options.include_usage 的 usage 尾 chunk 在 finish_reason
    // 之后到达，宽限表只在「请求了 stream_options 且 usage 未达」时武装，单次触发
    QTimer *usageGraceTimer = nullptr;
    QJsonObject usage;            // 捕获的 usage 对象（服务端原样透出，不进 assistantMsg）
    bool usageRequested = false;  // 本次请求体是否带 stream_options（仅 Chat 模式武装）
    bool sawFinishReason = false; // 已见 finish_reason 且进入宽限（usage 尾 chunk 到达即提前收口）

    // ---- s15 错误恢复传输层状态（lcc s15 af26862 recovery.py withRetry 语义下沉）----
    QString finishReason;         // 最近一次 finish_reason 值（随 messageFinished 上抛；重试清零重积）
    int consecutive529 = 0;       // 连续过载错误计数（任一有效帧到达 / 换模 / 非过载可重试错误即清零）
    bool usedFallback = false;    // 是否已消耗 fallback 换模机会（每流至多一次，lcc switched_model 口径）
};

ChatStream::ChatStream(QObject *parent) : QObject(parent), d(new Private)
{
    d->timeoutTimer = new QTimer(this);
    d->timeoutTimer->setSingleShot(true);
    connect(d->timeoutTimer, &QTimer::timeout, this, [this] {
        d->timedOut = true;
        if (client().verbose)
            LOG_DEBUG << "QOpenAi [timeout] 服务器静默超过 " << client().streamIdleTimeout << " ms";
        if (d->reply)
            d->reply->abort();
    });

    // 总时长哨兵：与 idle 防线的分工——idle 杀"静默卡死"，本表杀"慢而不断"
    // （readIncoming 收字节只重置 idle、绝不重置本表）。超时动作与 idle 同口径：
    // 置终态标志 → abort reply → finished 落 finishStream 的 error 分支，
    // 「done 恒一次」「cancel 后永久静默」语义不受影响
    d->totalTimer = new QTimer(this);
    d->totalTimer->setSingleShot(true);
    connect(d->totalTimer, &QTimer::timeout, this, [this] {
        d->totalTimedOut = true;
        if (client().verbose)
            LOG_DEBUG << "QOpenAi [timeout] 流式响应总时长超过 " << client().streamTotalTimeout << " ms";
        if (d->reply)
            d->reply->abort();
    });

    // usage 尾 chunk 宽限（修3 / 决策 D7）：finish_reason 已至而 usage 未至时给它最后一拍；
    // 超时直接走 finishStream 正常收尾（usage 为空则不 emit usageReceived），
    // done 门闩保证终局恰一次，最坏代价仅 +kUsageGraceMs 尾延迟
    d->usageGraceTimer = new QTimer(this);
    d->usageGraceTimer->setSingleShot(true);
    connect(d->usageGraceTimer, &QTimer::timeout, this, &ChatStream::finishStream);
}

ChatStream::~ChatStream()
{
    cleanupReply();
    delete d;
}

void ChatStream::cancel()
{
    if (d->done)
        return;
    d->done = true;
    d->timeoutTimer->stop();
    d->totalTimer->stop();
    d->usageGraceTimer->stop();

    if (d->reply)
    {
        d->reply->abort();
        cleanupReply();
    }
}

bool ChatStream::switchedToFallback() const
{
    return d->usedFallback;
}

void ChatStream::startRequest(const QJsonObject &input, Mode mode)
{
    d->input = input;
    d->input[QStringLiteral("stream")] = true; // 流式请求必须参数
    d->mode = mode;
    // 修3：stream_options 由调用方塞进 input 原样透传（仅主循环携带）；此处只判定
    // 本流是否需要 finish_reason 后的 usage 宽限。AsyncRequest/SubAgent 不带该字段
    // → usageRequested 恒 false，LegacyCompletion 亦恒 false，行为逐字节不变。
    d->usageRequested = (d->mode == Mode::Chat) && d->input.contains(QStringLiteral("stream_options"));
    d->retriesLeft = client().maxRetries;
    sendRequest();
}

void ChatStream::sendRequest()
{
    OpenAiClient &c = client();
    if (c.url.isEmpty() || c.token.isEmpty())
    {
        // 不在此处同步 emit（调用方此刻尚未连接信号），延迟一拍让 connect 先完成
        QTimer::singleShot(0, this, [this] {
            emit error(tr("未配置模型服务地址或 API Key，请在设置页的「模型服务」中填写。"));
        });
        return;
    }

    const QString endpoint = endpointFor(c, d->mode);
    QNetworkRequest req{QUrl(endpoint)}; // 基础 URL + 端点路径（/chat/completions 或 /completions）
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + c.token.toUtf8());

    const QByteArray body = QJsonDocument(d->input).toJson(QJsonDocument::Compact);
    if (c.verbose)
        LOG_DEBUG << "QOpenAi [request] " << "url=" << endpoint << " body=" << QString::fromUtf8(body);

    d->timedOut = false;
    d->totalTimedOut = false;
    d->reply = c.manager.post(req, body);
    connect(d->reply, &QNetworkReply::readyRead, this, &ChatStream::readIncoming);
    connect(d->reply, &QNetworkReply::finished, this, &ChatStream::finishStream);

    // 活动超时：首字节前窗口自此计时，readIncoming 每收到数据重置；streamIdleTimeout <= 0 时禁用
    d->timeoutTimer->stop();
    if (c.streamIdleTimeout > 0)
        d->timeoutTimer->start(c.streamIdleTimeout);

    // 总时长哨兵：自此计时且收字节不重置；重试（scheduleRetry→sendRequest）按单次
    // attempt 口径重新起表，与 idle 的每 attempt 重装保持同构；<=0 时禁用
    d->totalTimedOut = false;
    d->totalTimer->stop();
    if (c.streamTotalTimeout > 0)
        d->totalTimer->start(c.streamTotalTimeout);
}

void ChatStream::readIncoming()
{
    if (!d->reply || d->done)
        return;

    // 收到任何字节即证明连接存活：重置静默窗口（QTimer::start 对运行中的单次定时器即重启）
    const int idleMs = client().streamIdleTimeout;
    if (idleMs > 0)
        d->timeoutTimer->start(idleMs);

    d->buffer.append(d->reply->readAll());

    // 增量行提取（替代旧「整缓冲 replace("\r\n") + indexOf("\n\n")」——后者每次 readyRead
    // 对全量缓冲扫描/重写，是 O(n²)）。逐行取整行，行尾单个 '\r' 即 CRLF 归一（跨 chunk 的
    // "\r" + "\n" 天然由「无整行则留在 buffer」处理）；空行为帧边界：将已收集行以 '\n'
    // 拼回后整帧派发，与旧实现等价（processFrame 对帧仅做 trimmed()+data: 前缀判断，
    // 行内重组不影响语义；帧内含非 data: 行依旧静默忽略）。
    for (;;)
    {
        const int nl = d->buffer.indexOf('\n');
        if (nl < 0)
            break;
        QByteArray line = d->buffer.left(nl);
        d->buffer.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
        {
            if (!d->pendingFrame.trimmed().isEmpty())
                processFrame(d->pendingFrame);
            d->pendingFrame.clear();
            continue;
        }
        if (!d->pendingFrame.isEmpty())
            d->pendingFrame += '\n';
        d->pendingFrame += line;
    }
}

void ChatStream::processFrame(const QByteArray &frame)
{
    if (d->done)
        return; // 已收尾，忽略残留帧（如 [DONE] 后额外数据）
    if (client().verbose)
        LOG_DEBUG << "QOpenAi [sse] " << frame;

    QByteArray line = frame.trimmed();
    if (!line.startsWith("data:"))
        return; // 非 data: 帧即噪声（心跳/注释行），静默跳过——噪声容忍已在此分支存在

    const QString payload = QString::fromUtf8(line.mid(5).trimmed());
    if (payload == "[DONE]")
    {
        finishStream();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
    if (!doc.isObject())
    {
        // 带 data: 前缀却是坏 JSON：属协议错误而非噪声（噪声已在上方非 data: 分支被吸收），
        // 维持 emit error 终局语义——调用方（AgentLoop/SubAgent）错误分支会落盘历史，不做降级。
        // 终局三件套与 finishStream 各错误分支对齐（done → cleanupReply → emit）：不置 done
        // 则同批后续帧继续处理、残留帧还能触发 finishStream 发出 messageFinished，
        // 与 error 后的回合终局竞态（第六轮审计 C1）
        d->done = true;
        // error 终局处停哨（idle 表未停为既有行为——reply 已清空，其触发无副作用）
        d->totalTimer->stop();
        cleanupReply();
        emit error(tr("SSE 帧 JSON 解析失败: %1").arg(payload.left(200)));
        return;
    }
    const QJsonObject root = doc.object();

    // s15（lcc s15 af26862 recovery.py）：收到有效帧即证明服务端存活——过载连续计数清零（成功即清零）
    d->consecutive529 = 0;

    // 修3：usage 捕获必须先于 choices 检查——include_usage 语义下末帧 choices=[] 且带
    // usage，若按原顺序会被 isEmpty 分支静默丢弃
    const QJsonValue usageVal = root.value(QStringLiteral("usage"));
    if (usageVal.isObject() && !usageVal.toObject().isEmpty())
        d->usage = usageVal.toObject();

    const QJsonValue choicesVal = root.value(QStringLiteral("choices"));
    if (!choicesVal.isArray())
        return;
    const QJsonArray choices = choicesVal.toArray();
    if (choices.isEmpty())
    {
        // include_usage 的尾 chunk：usage 已在上方捕获。finish_reason 已见过的话提前收口，
        // 不再空等 [DONE]/finished（done 门闩保证与后续终局路径互斥、恰一次）
        if (d->sawFinishReason && !d->usage.isEmpty())
            finishStream();
        return;
    }
    const QJsonObject choice = choices.first().toObject();

    // legacy /completions：text 直接位于 choice，无 delta/reasoning_content/tool_calls
    if (d->mode == Mode::LegacyCompletion)
    {
        const QString text = choice.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
        {
            d->content += text;
            emit textDelta(text);
        }
        const QString legacyReason = choice.value(QStringLiteral("finish_reason")).toString();
        if (!legacyReason.isEmpty())
        {
            d->finishReason = legacyReason; // s15：记录值随 messageFinished 上抛（仅开通道，不改行为）
            finishStream();
        }
        return;
    }

    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

    // 推理增量
    const QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
    if (!reasoning.isEmpty())
    {
        d->thinking += reasoning;
        emit thinkingDelta(reasoning);
    }

    // 正文增量
    const QString text = delta.value(QStringLiteral("content")).toString();
    if (!text.isEmpty())
    {
        d->content += text;
        emit textDelta(text);
    }

    // tool_calls 增量合并（按 index 拼装）
    const QJsonValue tcsVal = delta.value(QStringLiteral("tool_calls"));
    if (tcsVal.isArray())
    {
        const QJsonArray tcs = tcsVal.toArray();
        for (const QJsonValue &tcVal : tcs)
        {
            const QJsonObject tc = tcVal.toObject();
            const int index = tc.value(QStringLiteral("index")).toInt(-1);
            if (index < 0)
                continue;

            while (d->toolCalls.size() <= index)
                d->toolCalls.append(QJsonObject());

            QJsonObject merged = d->toolCalls.at(index).toObject();
            if (merged.isEmpty())
                merged = tc;

            if (!tc.value(QStringLiteral("id")).toString().isEmpty())
                merged[QStringLiteral("id")] = tc.value(QStringLiteral("id"));

            if (!tc.value(QStringLiteral("type")).toString().isEmpty())
                merged[QStringLiteral("type")] = tc.value(QStringLiteral("type"));

            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            if (!fn.isEmpty())
            {
                QJsonObject mergedFn = merged.value(QStringLiteral("function")).toObject();
                if (!fn.value(QStringLiteral("name")).toString().isEmpty())
                    mergedFn[QStringLiteral("name")] = fn.value(QStringLiteral("name"));
                if (fn.contains(QStringLiteral("arguments")))
                {
                    const QString args = fn.value(QStringLiteral("arguments")).toString();
                    mergedFn[QStringLiteral("arguments")] =
                        mergedFn.value(QStringLiteral("arguments")).toString() + args;
                }
                merged[QStringLiteral("function")] = mergedFn;
            }

            d->toolCalls[index] = merged;
        }
    }

    // 本轮结束（finish_reason 出现）。修3（决策 D7）：OpenAI 语义下 usage 尾 chunk 排在
    // finish_reason 帧之后，现状「finish_reason 即终局」会让 usage 永失——请求了
    // stream_options 且 usage 未到时改启单次宽限；usage 尾 chunk / [DONE] / reply
    // finished / 宽限超时任一先行收口，done 门闩保证恰一次。未请求方逐字节不变。
    const QString finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    if (!finishReason.isEmpty())
    {
        d->finishReason = finishReason; // s15（lcc s15 af26862）：记录值随 messageFinished 上抛（仅开通道，不改行为）
        if (d->usageRequested && d->usage.isEmpty() && !d->sawFinishReason)
        {
            d->sawFinishReason = true;
            d->usageGraceTimer->start(AgentConst::kUsageGraceMs);
        }
        else
        {
            finishStream();
        }
    }
}

void ChatStream::finishStream()
{
    if (d->done)
        return;
    d->timeoutTimer->stop();
    d->totalTimer->stop();
    d->usageGraceTimer->stop(); // 任何终局路径收口即撤宽限（[DONE]/提前收口/超时后残留表不再触发）

    OpenAiClient &c = client();
    const bool hasError = d->reply && d->reply->error() != QNetworkReply::NoError;
    const int httpStatus = d->reply
        ? d->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        : 0;

    if (c.verbose)
    {
        const QString note = hasError
            ? tr("错误：%1").arg(d->reply->errorString())
            : QStringLiteral("ok");
        LOG_DEBUG << "QOpenAi [response] " << "status=" << httpStatus << " " << note;
    }

    // 检查网络错误并分层处理
    if (hasError)
    {
        const QString errorMsg = d->reply->errorString();

        // 静默超时中止（区别于主动取消）：服务器连续无字节输出。
        // s15（lcc s15 af26862 recovery.py 把 timeout 归入可重试瞬态错误）：重试预算尚存则与
        // 429/5xx 同走 scheduleRetry 退避重发通道；预算耗尽维持原终态文案（与接入前逐字节一致）。
        // 偏差登记：lcc 判 timeout 用异常类型名鸭子匹配，lite 由超时标志位触发本分支、不经文本判据，语义等价。
        if (d->timedOut)
        {
            if (d->retriesLeft > 0)
            {
                --d->retriesLeft;
                LOG_WARN << "QOpenAi [recovery] idle timeout retry "
                         << (c.maxRetries - d->retriesLeft) << " / " << c.maxRetries;
                scheduleRetry();
                return;
            }
            d->done = true;
            cleanupReply();
            emit error(tr("服务器无响应（连续 %1 ms 未收到数据）。").arg(c.streamIdleTimeout));
            return;
        }

        // 总时长哨兵中止（同样区别于主动取消）："慢而不断"的流——idle 窗口内始终有字节，
        // 静默防线永不触发，由本分支收口。须排在 OperationCanceledError 吞掉分支之前，
        // 否则 abort 产生的取消错误会被当作主动取消静默丢弃。
        // s15 偏差登记（对照 lcc timeout 可重试）：lite 总超时是最终兜底防线，重发会让单回合
        // 总等待翻倍，故保持直接终态、不占重试预算
        if (d->totalTimedOut)
        {
            d->done = true;
            cleanupReply();
            emit error(tr("流式响应超时（总时长超过 %1 ms）。").arg(c.streamTotalTimeout));
            return;
        }

        // 主动取消不报错
        if (d->reply->error() == QNetworkReply::OperationCanceledError)
        {
            cleanupReply();
            return;
        }

        // HTTP 状态码分层。可重试判定必须先于 4xx 硬错误：429 落在 [400,500) 区间内，
        // 原顺序（4xx 分支先行 return）令下方 isRetryableStatus 对 429 永不可达，指数退避
        // 链路成死代码——而 429 恰是 LLM API 最常见的瞬态错误，必须重试而非直接终结回合。
        if (isRetryableStatus(httpStatus))
        {
            // 5xx 服务端错误 / 429 限流：指数退避重试
            if (d->retriesLeft > 0)
            {
                // s15 过载连续计数与 fallback 换模（lcc s15 af26862 recovery.py withRetry：连续 529
                // 达 MAX_CONSECUTIVE_529 且配置了 fallbackModel → 换模重试，每 loop 至多一次）。
                // 偏差：lcc 包在 loop 外层、lite 把等价语义下沉到传输层；overloaded 判定把 lcc 鸭子
                // 判据（类型名/消息含 overloaded、消息含 529）折叠为「HTTP 状态 529 或错误体文本
                // 命中 RecoveryRules::isOverloadedText」。非过载族可重试错误（429/500 等）清零计数。
                // 换模重发沿用 lite 既有退避 1s,2s,4s…封顶 64s 无抖动（lcc 为 500ms·2^n 封顶 32s
                // + 25% 抖动，语义等价，见类头偏差清单）。
                const QString detail = parseErrorDetail(d->pendingFrame, d->buffer, d->reply, errorMsg);
                const bool overloaded =
                    httpStatus == 529 || RecoveryRules::isOverloadedText(detail);
                if (overloaded)
                    ++d->consecutive529;
                else
                    d->consecutive529 = 0;

                if (d->consecutive529 >= AgentConst::kMaxConsecutive529
                    && !c.fallbackModelId.isEmpty() && !d->usedFallback)
                {
                    // 消耗换模机会：重发体 model 字段改为 fallback（每流至多一次）；计数清零，
                    // 本次重发照走下方既有退避通道（retriesLeft 仍由统一入口扣减）
                    d->usedFallback = true;
                    d->consecutive529 = 0;
                    d->input[QStringLiteral("model")] = c.fallbackModelId;
                    LOG_WARN << "QOpenAi [recovery] 529 consecutive, switch to fallback model: "
                             << c.fallbackModelId;
                }

                --d->retriesLeft;
                scheduleRetry();
                return;
            }
            // 重试耗尽：与 4xx 同源读体解析（原只给 errorString()，对 4xx/429 常是
            // "Unknown error"，限流原因/配额信息全丢），detail 供上层关键词判定与用户诊断
            d->done = true;
            const QString detail = parseErrorDetail(d->pendingFrame, d->buffer, d->reply, errorMsg);
            cleanupReply();
            emit error(tr("HTTP %1 错误（重试 %2 次后仍失败）: %3")
                           .arg(httpStatus)
                           .arg(c.maxRetries)
                           .arg(detail));
            return;
        }
        else if (httpStatus >= 400 && httpStatus < 500)
        {
            // 4xx 客户端错误：硬错误，不重试。修3：响应体是 reactive 判定的信息源
            // （context_length_exceeded 等只存在于服务端 JSON 错误体里），errorString()
            // 对 4xx 常是 "Unknown error"——必须读体并入 error 文本。
            // 残体合并与解析已收敛到 parseErrorDetail（与重试耗尽路径共用，须在 cleanupReply 前读）
            d->done = true;
            const QString detail = parseErrorDetail(d->pendingFrame, d->buffer, d->reply, errorMsg);
            cleanupReply();
            // tr 格式串沿用现状，不新增 i18n 条目；detail 为服务端英文原文，关键词匹配依赖它
            emit error(tr("HTTP %1 错误: %2").arg(httpStatus).arg(detail));
            return;
        }
        else
        {
            // 其他网络错误（无 HTTP 状态码或未知）
            d->done = true;
            cleanupReply();
            emit error(errorMsg);
            return;
        }
    }

    // 正常收尾：组装完整 assistant 消息
    d->done = true; // 防重入：后续 [DONE] 或残留帧再触发收尾时被守卫拦下
    QJsonObject assistantMsg;
    assistantMsg[QStringLiteral("role")] = QStringLiteral("assistant");
    assistantMsg[QStringLiteral("content")] = d->content;
    if (!d->thinking.isEmpty())
        assistantMsg[QStringLiteral("reasoning_content")] = d->thinking;

    // 去占位后输出 tool_calls
    QJsonArray realCalls;
    for (const QJsonValue &v : d->toolCalls)
    {
        if (v.isObject() && !v.toObject().isEmpty())
            realCalls.append(v);
    }
    if (!realCalls.isEmpty())
        assistantMsg[QStringLiteral("tool_calls")] = realCalls;

    cleanupReply();
    // 契约 C-2：usage 恒先于 messageFinished、仅成功路径、仅非空时发射；
    // 直连顺序有保证，不并入 assistantMsg（落盘历史会污染持久化与计量）
    if (!d->usage.isEmpty())
        emit usageReceived(d->usage);
    emit messageFinished(assistantMsg, d->finishReason);
}

void ChatStream::scheduleRetry()
{
    OpenAiClient &c = client();
    const int retryNumber = c.maxRetries - d->retriesLeft; // 本次是第几次重试（从 1 起）
    const int delayMs = retryDelayMs(retryNumber);

    if (c.verbose)
        LOG_DEBUG << "QOpenAi [retry] " << retryNumber << " / " << c.maxRetries
                  << "after " << delayMs << " ms";

    cleanupReply();

    // 重试从头开始，清空累积状态
    d->buffer.clear();
    // s15（lcc s15 af26862）：半行残留与已捕获的 finish_reason 一并复位，重试 attempt 从零重收
    // （finishReason 与 sawFinishReason 同口径；pendingFrame 清理是 idle 超时重试路径的必须配套——
    // 静默超时可能截在帧中间，残半帧会拼进新 attempt 首行毁协议；429/5xx 路径下错误体多已被
    // parseErrorDetail 读尽，该潜在缺陷此前被掩盖）
    d->pendingFrame.clear();
    d->finishReason.clear();
    d->thinking.clear();
    d->content.clear();
    d->toolCalls = QJsonArray();
    // 修3：usage 捕获态与宽限一并复位，重试 attempt 从零重收（input 保留 stream_options，
    // usageRequested 无需重置——同一请求体重发）
    d->usage = QJsonObject();
    d->sawFinishReason = false;
    d->usageGraceTimer->stop();

    QTimer::singleShot(delayMs, this, [this] {
        if (!d->done)
            sendRequest();
    });
}

void ChatStream::cleanupReply()
{
    if (d->reply)
    {
        d->reply->disconnect(this);
        d->reply->deleteLater();
        d->reply = nullptr;
    }
}

ChatStream *CategoryChat::createStream(const QJsonObject &input, QObject *parent)
{
    auto *stream = new ChatStream(parent);
    stream->startRequest(input, ChatStream::Mode::Chat);
    return stream;
}

ChatStream *CategoryCompletion::createStream(const QJsonObject &input, QObject *parent)
{
    auto *stream = new ChatStream(parent);
    stream->startRequest(input, ChatStream::Mode::LegacyCompletion);
    return stream;
}

// 聊天入口（/chat/completions）
CategoryChat &chat()
{
    static CategoryChat c;
    return c;
}

// legacy 补全入口（/completions）
CategoryCompletion &completion()
{
    static CategoryCompletion c;
    return c;
}

// ---- 一次性异步文本请求（契约与设计缘由见 QOpenAi.h 的 AsyncRequest 声明注释）----

AsyncRequest::AsyncRequest(QObject *parent)
    : QObject(parent)
{
    m_totalTimer = new QTimer(this);
    m_totalTimer->setSingleShot(true);
}

AsyncRequest *AsyncRequest::sendText(const QJsonObject &input, QObject *parent,
                                     std::function<void(const QString &, const QString &)> done,
                                     int totalTimeoutMs)
{
    auto *req = new AsyncRequest(parent);
    req->m_handler = std::move(done);
    req->start(input, totalTimeoutMs);
    return req;
}

void AsyncRequest::start(const QJsonObject &input, int totalTimeoutMs)
{
    // 复用 ChatStream 而非另起非流式分支（设计文档 §3.1 决策）：侧链只消费正文，
    // 重试 / idle 超时 / 配置缺失延迟 error / 析构 abort 全部现成。
    // 配置缺失的 error 经 ChatStream::startRequest 内 singleShot(0) 延迟一拍发射，
    // 下方 connect 在事件循环返回前同步完成，该路径必被 error 分支接住、不崩不发。
    m_stream = chat().createStream(input, this);
    // context 一律取 this：终态后对象 deleteLater，连接随析构自动断开，无悬挂回调
    connect(m_stream, &ChatStream::messageFinished, this,
            [this](const QJsonObject &msg, const QString & /*finishReason*/) {
        // 只提取正文：ChatStream::finishStream 收尾时已将累积 delta 组装进 message.content；
        // reasoning_content / tool_calls 丢弃——本类契约即纯文本
        fireDone(msg.value(QStringLiteral("content")).toString(), QString());
    });
    connect(m_stream, &ChatStream::error, this, [this](const QString &message) {
        // 网络错误 / idle 超时 / 4xx / 重试用尽 / 配置缺失统一折叠为 error 串
        fireDone(QString(), message);
    });
    if (totalTimeoutMs > 0)
    {
        connect(m_totalTimer, &QTimer::timeout, this, [this, totalTimeoutMs] {
            // 总超时哨兵：idle 计时每收字节即重置，"慢而不断"的流永不触发
            // idle 防线，需按总量兜底——语义对齐迁移前阻塞链的总时限
            fireDone(QString(), tr("请求超时（%1 ms）。").arg(totalTimeoutMs));
        });
        m_totalTimer->start(totalTimeoutMs);
    }
}

void AsyncRequest::fireDone(const QString &content, const QString &error)
{
    if (m_done)
        return; // 首到终态生效：error / messageFinished / 总超时竞态互斥（风险清单 §6-2）
    m_done = true;
    m_totalTimer->stop();
    if (m_stream)
        m_stream->cancel(); // ChatStream 置位 done、abort reply，此后不再有任何信号

    // 回调先移交为栈上局部量再交付：下方 handler 调用内若同步析构本对象
    // （如回调里销毁 parent），成员已全部脱手、仅局部量与事件队列安全存续
    std::function<void(const QString &, const QString &)> handler = std::move(m_handler);
    m_handler = nullptr;
    deleteLater(); // 终态自清理。Qt 保证对象析构时丢弃待决 deleteLater 事件，无二次删除
    if (handler)
        handler(content, error);
}

void AsyncRequest::cancel()
{
    if (m_done)
        return; // 已终态/已取消：deleteLater 已在队列中，无需重复调度
    m_done = true;
    m_totalTimer->stop();
    if (m_stream)
        m_stream->cancel();
    m_handler = nullptr; // 主动取消不通知调用方：done 永久静默（契约见声明注释）
    deleteLater();
}

// ---- 运行时配置 ----

void setUrl(const QString &url)
{
    // base URL（不含端点路径），统一去掉尾部斜杠（与 endpointFor 共用同一 trim 实现）
    client().url = trimTrailingSlashes(url);
}

QString url()
{
    return client().url;
}

void setToken(const QString &token)
{
    client().token = token;
}

QString token()
{
    return client().token;
}

void setMaxRetries(int retries)
{
    client().maxRetries = qMax(0, retries);
}

int maxRetries()
{
    return client().maxRetries;
}

void setFallbackModelId(const QString &modelId)
{
    // lcc env.py 归一口径：strip 后空串=禁用换模（fallbackModelIdValue 已 trim，此处再防外部调用方塞空白）
    client().fallbackModelId = modelId.trimmed();
}

QString fallbackModelId()
{
    return client().fallbackModelId;
}

void setTimeout(int milliseconds)
{
    client().streamIdleTimeout = qMax(0, milliseconds);
}

int timeout()
{
    return client().streamIdleTimeout;
}

void setVerbose(bool enabled)
{
    client().verbose = enabled;
}

bool verbose()
{
    return client().verbose;
}

void initFromSettings()
{
    OpenAiClient &c = client();
    // 读侧统一走 AppSettings::value()（缓存解析产物），不再为两个键现构造 QSettings
    setUrl(AppSettings::value(QStringLiteral("apiBaseUrl")).toString());
    c.token = AppSettings::value(QStringLiteral("apiToken")).toString();
    // 重试次数同源于 settings.ini（键 maxRetries，默认/校验界见 AgentConst::kMaxRetries*）：
    // 原默认 0 且全仓无调用方，429/5xx 退避链路运行时不可达
    setMaxRetries(AgentConst::maxRetriesValue());
    // s15 后备模型同源于 settings.ini（键 fallbackModelId，空=禁用换模；对照 lcc env.py
    // FALLBACK_MODEL_ID "or None" 口径——未配置则全程不换模，行为与 s15 前逐字节一致）
    setFallbackModelId(AgentConst::fallbackModelIdValue());
    if (c.url.isEmpty() || c.token.isEmpty())
        LOG_WARN << "QOpenAi: settings.ini 的 apiBaseUrl / apiToken 未配置或为空。";
}

} // namespace QOpenAi