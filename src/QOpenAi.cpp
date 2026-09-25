#include "QOpenAi.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QByteArray>
#include <QTimer>
#include <QEventLoop>
#include <QUrl>
#include <QDebug>
#include <QGuiApplication>  // 等待期应用级 override cursor（BlockingSession 用）
#include <QCursor>          // Qt::CursorShape→QCursor 隐式转换需完整类型（setOverrideCursor 重载决议）

namespace {

class OpenAiClient
{
public:
    OpenAiClient() = default;

    QNetworkAccessManager manager;
    QString url;
    QString token;
    int maxRetries = 0;          // 最大重试次数（仅 5xx / 429），默认 0 不重试
    int streamIdleTimeout = 60000;  // 流式静默超时（毫秒）：每收到数据即重置，服务器持续有输出则总时长不限；<=0 不限时
    int blockingTimeout = 120000;   // 阻塞式非流式请求总超时（毫秒）：无中间字节可依据，按总量计；<=0 不限时
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

// base URL 归一：去掉全部尾部斜杠（endpointFor 与 setUrl 共用，消除两份重复实现）
QString trimTrailingSlashes(const QString &url)
{
    QString base = url;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base;
}

QJsonObject errorJson(const QString &message)
{
    QJsonObject obj;
    obj[QStringLiteral("error")] = message;
    return obj;
}

// 由基础 URL（不含端点路径）拼出具体端点
QString endpointFor(OpenAiClient &c, QOpenAi::ChatStream::Mode mode)
{
    const char *suffix = mode == QOpenAi::ChatStream::Mode::LegacyCompletion
        ? "/completions"
        : "/chat/completions";
    return trimTrailingSlashes(c.url) + QString::fromLatin1(suffix);
}

// 阻塞式非流式请求（含退避重试与超时）：成功返回响应 JSON，失败返回含 error 字段的对象
QJsonObject blockingRequest(OpenAiClient &c, const QString &endpoint, const QJsonObject &input)
{
    if (c.url.isEmpty() || c.token.isEmpty())
        return errorJson(QObject::tr("未配置 QOpenAiBaseUrl/QOpenAiToken 环境变量。"));

    // 等待期显式化：此后所有路径都在嵌套事件循环里同步等 LLM（含重试退避的 wait.exec），
    // 用 RAII 守卫统一进出，覆盖本函数全部 return / continue / 异常退出，无需逐点手工恢复。
    // 特意放在配置缺失早退之后——那条路径根本不进嵌套循环，不应闪现等待态。
    // 超时/重试/返回语义零变化：守卫只旁路发布状态，不干预请求本身。
    QOpenAi::BlockingSession waitSession;

    int retriesLeft = c.maxRetries;

    for (int attempt = 1;; ++attempt)
    {
        QNetworkRequest req{QUrl(endpoint)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Authorization", QByteArray("Bearer ") + c.token.toUtf8());

        const QByteArray body = QJsonDocument(input).toJson(QJsonDocument::Compact);
        if (c.verbose)
            qDebug() << "QOpenAi [request]" << "attempt=" << attempt
                     << "url=" << endpoint << "body=" << QString::fromUtf8(body);

        QNetworkReply *reply = c.manager.post(req, body);

        // 用事件循环阻塞等待响应，QTimer 实现超时
        bool timedOut = false;
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        if (c.blockingTimeout > 0)
        {
            timeoutTimer.start(c.blockingTimeout);
            QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&loop, &timedOut] {
                timedOut = true;
                loop.quit();
            });
        }
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorMsg = reply->errorString();

        if (c.verbose)
            qDebug() << "QOpenAi [response]" << "attempt=" << attempt
                     << "status=" << httpStatus
                     << (timedOut ? QStringLiteral("timeout") : errorMsg);

        if (timedOut)
        {
            reply->abort();
            reply->deleteLater();
            return errorJson(QObject::tr("请求超时（%1 ms）。").arg(c.blockingTimeout));
        }

        if (reply->error() != QNetworkReply::NoError)
        {
            // 5xx 服务端错误 / 429 限流：指数退避重试
            if (isRetryableStatus(httpStatus) && retriesLeft > 0)
            {
                --retriesLeft;
                const int retryNumber = c.maxRetries - retriesLeft;
                const int delayMs = retryDelayMs(retryNumber);
                if (c.verbose)
                    qDebug() << "QOpenAi [retry]" << retryNumber << "/" << c.maxRetries
                             << "after" << delayMs << "ms";
                reply->deleteLater();
                QEventLoop wait;
                QTimer::singleShot(delayMs, &wait, &QEventLoop::quit);
                wait.exec();
                continue;
            }

            reply->deleteLater();

            if (httpStatus >= 400 && httpStatus < 500)
                return errorJson(QObject::tr("HTTP %1 错误: %2").arg(httpStatus).arg(errorMsg));
            if (isRetryableStatus(httpStatus))
                return errorJson(QObject::tr("HTTP %1 错误（重试 %2 次后仍失败）: %3")
                                     .arg(httpStatus).arg(c.maxRetries).arg(errorMsg));
            return errorJson(errorMsg);
        }

        // 成功：解析完整 JSON 并返回
        const QByteArray data = reply->readAll();
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject())
            return errorJson(QObject::tr("响应 JSON 解析失败: %1").arg(QString::fromUtf8(data.left(200))));
        return doc.object();
    }
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
    bool timedOut = false;     // 是否超时中止
    Mode mode = Mode::Chat;    // 流处理模式（决定端点与 SSE 解析分支）
    QJsonObject input;         // 请求体，供重试复用
    int retriesLeft = 0;       // 剩余可重试次数
    QTimer *timeoutTimer = nullptr;
};

ChatStream::ChatStream(QObject *parent) : QObject(parent), d(new Private)
{
    d->timeoutTimer = new QTimer(this);
    d->timeoutTimer->setSingleShot(true);
    connect(d->timeoutTimer, &QTimer::timeout, this, [this] {
        d->timedOut = true;
        if (client().verbose)
            qDebug() << "QOpenAi [timeout] 服务器静默超过" << client().streamIdleTimeout << "ms";
        if (d->reply)
            d->reply->abort();
    });
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

    if (d->reply)
    {
        d->reply->abort();
        cleanupReply();
    }
}

void ChatStream::startRequest(const QJsonObject &input, Mode mode)
{
    d->input = input;
    d->input[QStringLiteral("stream")] = true; // 流式请求必须参数
    d->mode = mode;
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
            emit error(tr("未配置 QOpenAiBaseUrl/QOpenAiToken 环境变量。"));
        });
        return;
    }

    const QString endpoint = endpointFor(c, d->mode);
    QNetworkRequest req{QUrl(endpoint)}; // 基础 URL + 端点路径（/chat/completions 或 /completions）
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + c.token.toUtf8());

    const QByteArray body = QJsonDocument(d->input).toJson(QJsonDocument::Compact);
    if (c.verbose)
        qDebug() << "QOpenAi [request]" << "url=" << endpoint << "body=" << QString::fromUtf8(body);

    d->timedOut = false;
    d->reply = c.manager.post(req, body);
    connect(d->reply, &QNetworkReply::readyRead, this, &ChatStream::readIncoming);
    connect(d->reply, &QNetworkReply::finished, this, &ChatStream::finishStream);

    // 活动超时：首字节前窗口自此计时，readIncoming 每收到数据重置；streamIdleTimeout <= 0 时禁用
    d->timeoutTimer->stop();
    if (c.streamIdleTimeout > 0)
        d->timeoutTimer->start(c.streamIdleTimeout);
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
        qDebug() << "QOpenAi [sse]" << frame;

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
        // 维持 emit error 终局语义——调用方（AgentLoop/SubAgent）错误分支会落盘历史，不做降级
        emit error(tr("SSE 帧 JSON 解析失败: %1").arg(payload.left(200)));
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonValue choicesVal = root.value(QStringLiteral("choices"));
    if (!choicesVal.isArray())
        return;
    const QJsonArray choices = choicesVal.toArray();
    if (choices.isEmpty())
        return;
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
        if (!choice.value(QStringLiteral("finish_reason")).toString().isEmpty())
            finishStream();
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

    // 本轮结束（finish_reason 出现）
    const QString finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    if (!finishReason.isEmpty())
        finishStream();
}

void ChatStream::finishStream()
{
    if (d->done)
        return;
    d->timeoutTimer->stop();

    OpenAiClient &c = client();
    const bool hasError = d->reply && d->reply->error() != QNetworkReply::NoError;
    const int httpStatus = d->reply
        ? d->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        : 0;

    if (c.verbose)
    {
        const QString note = hasError
            ? tr("error=%1").arg(d->reply->errorString())
            : QStringLiteral("ok");
        qDebug() << "QOpenAi [response]" << "status=" << httpStatus << note;
    }

    // 检查网络错误并分层处理
    if (hasError)
    {
        const QString errorMsg = d->reply->errorString();

        // 静默超时中止（区别于主动取消）：服务器连续无字节输出
        if (d->timedOut)
        {
            d->done = true;
            cleanupReply();
            emit error(tr("服务器无响应（连续 %1 ms 未收到数据）。").arg(c.streamIdleTimeout));
            return;
        }

        // 主动取消不报错
        if (d->reply->error() == QNetworkReply::OperationCanceledError)
        {
            cleanupReply();
            return;
        }

        // HTTP 状态码分层
        if (httpStatus >= 400 && httpStatus < 500)
        {
            // 4xx 客户端错误：硬错误，不重试
            d->done = true;
            cleanupReply();
            emit error(tr("HTTP %1 错误: %2").arg(httpStatus).arg(errorMsg));
            return;
        }
        else if (isRetryableStatus(httpStatus))
        {
            // 5xx 服务端错误 / 429 限流：指数退避重试
            if (d->retriesLeft > 0)
            {
                --d->retriesLeft;
                scheduleRetry();
                return;
            }
            d->done = true;
            cleanupReply();
            emit error(tr("HTTP %1 错误（重试 %2 次后仍失败）: %3")
                           .arg(httpStatus)
                           .arg(c.maxRetries)
                           .arg(errorMsg));
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
    emit messageFinished(assistantMsg);
}

void ChatStream::scheduleRetry()
{
    OpenAiClient &c = client();
    const int retryNumber = c.maxRetries - d->retriesLeft; // 本次是第几次重试（从 1 起）
    const int delayMs = retryDelayMs(retryNumber);

    if (c.verbose)
        qDebug() << "QOpenAi [retry]" << retryNumber << "/" << c.maxRetries
                 << "after" << delayMs << "ms";

    cleanupReply();

    // 重试从头开始，清空累积状态
    d->buffer.clear();
    d->thinking.clear();
    d->content.clear();
    d->toolCalls = QJsonArray();

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

QJsonObject CategoryChat::create(const QJsonObject &input)
{
    OpenAiClient &c = client();
    return blockingRequest(c, endpointFor(c, ChatStream::Mode::Chat), input);
}

ChatStream *CategoryCompletion::createStream(const QJsonObject &input, QObject *parent)
{
    auto *stream = new ChatStream(parent);
    stream->startRequest(input, ChatStream::Mode::LegacyCompletion);
    return stream;
}

QJsonObject CategoryCompletion::create(const QJsonObject &input)
{
    OpenAiClient &c = client();
    return blockingRequest(c, endpointFor(c, ChatStream::Mode::LegacyCompletion), input);
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
    // 配置缺失的 error 经 singleShot(0) 延迟一拍发射（QOpenAi.cpp :242-245），
    // 下方 connect 在事件循环返回前同步完成，该路径必被 error 分支接住、不崩不发。
    m_stream = chat().createStream(input, this);
    // context 一律取 this：终态后对象 deleteLater，连接随析构自动断开，无悬挂回调
    connect(m_stream, &ChatStream::messageFinished, this, [this](const QJsonObject &msg) {
        // 只提取正文：ChatStream 收尾时已将累积 delta 组装进 message.content（:505）；
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
            // 总超时哨兵：idle 计时每收字节即重置（:273-276），"慢而不断"的流永不触发
            // idle 防线，需按总量兜底——语义对齐原阻塞链 blockingTimeout（:107-116）
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

// ---- 等待期状态（BlockingGate / BlockingSession，设计缘由见 QOpenAi.h 注释）----

BlockingGate::BlockingGate(QObject *parent)
    : QObject(parent)
{
}

bool BlockingGate::busy() const
{
    return m_depth > 0;
}

BlockingGate *blockingGate()
{
    // 函数局部 static：首次使用即构造、进程退出前析构。全仓零线程（有意约定），
    // 不存在初始化竞争；UI 组件以 this 为 context 连接，随页面析构自动断连防悬窗回调
    static BlockingGate gate;
    return &gate;
}

BlockingSession::BlockingSession()
{
    BlockingGate *gate = blockingGate();
    if (++gate->m_depth == 1)
    {
        // 仅最外层（0→1 边界）动全局：先推入应用级 WaitCursor 再广播 busy，
        // UI 禁用控件的槽执行时视觉"系统正忙"信号已就位。
        // 重入中间层（1→2…）只加计数——嵌套循环事件派发中触发的同步路径
        // 属于同一段等待期，不重复推光标/发信号
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        emit gate->busyChanged(true);
    }
}

BlockingSession::~BlockingSession()
{
    BlockingGate *gate = blockingGate();
    if (--gate->m_depth == 0)
    {
        // 与构造反序对称：先广播解禁（UI 恢复控件），再弹回光标。
        // 重入中间层退出（n→n-1，n-1≥1）两者都不动——绝不第一层退出就提前解禁。
        // override 光标栈的 push/pop 由同一 0↔1 计数边界配对，平衡有保证
        //（已 grep 确认仓内无其他 setOverrideCursor 调用点干扰该栈）
        emit gate->busyChanged(false);
        QGuiApplication::restoreOverrideCursor();
    }
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

void setTimeout(int milliseconds)
{
    client().streamIdleTimeout = qMax(0, milliseconds);
}

int timeout()
{
    return client().streamIdleTimeout;
}

void setBlockingTimeout(int milliseconds)
{
    client().blockingTimeout = qMax(0, milliseconds);
}

int blockingTimeout()
{
    return client().blockingTimeout;
}

void setVerbose(bool enabled)
{
    client().verbose = enabled;
}

bool verbose()
{
    return client().verbose;
}

void initByEnv()
{
    OpenAiClient &c = client();
    setUrl(QString::fromUtf8(qgetenv("QOpenAiBaseUrl")));
    c.token = QString::fromUtf8(qgetenv("QOpenAiToken"));
    if (c.url.isEmpty() || c.token.isEmpty())
        qWarning() << "QOpenAi: 环境变量 QOpenAiBaseUrl / QOpenAiToken 未配置或为空。";
}

} // namespace QOpenAi