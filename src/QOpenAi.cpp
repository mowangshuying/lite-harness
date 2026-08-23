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

namespace {

class OpenAiClient
{
public:
    OpenAiClient() = default;

    QNetworkAccessManager manager;
    QString url;
    QString token;
    int maxRetries = 0;          // 最大重试次数（仅 5xx / 429），默认 0 不重试
    int networkTimeout = 30000;  // 请求超时（毫秒），默认 30s，<=0 不限时
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

// 指数退避：第 n 次重试（从 1 起）等待 1000 * 2^(n-1) 毫秒（1s, 2s, 4s...）
int retryDelayMs(int retryNumber)
{
    return 1000 * (1 << (retryNumber - 1));
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
    QString base = c.url;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + QString::fromLatin1(suffix);
}

// 阻塞式非流式请求（含退避重试与超时）：成功返回响应 JSON，失败返回含 error 字段的对象
QJsonObject blockingRequest(OpenAiClient &c, const QString &endpoint, const QJsonObject &input)
{
    if (c.url.isEmpty() || c.token.isEmpty())
        return errorJson(QObject::tr("未配置 QOpenAiBaseUrl/QOpenAiToken 环境变量。"));

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
        if (c.networkTimeout > 0)
        {
            timeoutTimer.start(c.networkTimeout);
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
            return errorJson(QObject::tr("请求超时（%1 ms）。").arg(c.networkTimeout));
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
    QByteArray buffer;         // SSE 累积缓冲
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
            qDebug() << "QOpenAi [timeout] 超过" << client().networkTimeout << "ms";
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

    // 每次请求独立计时；networkTimeout <= 0 时禁用超时
    d->timeoutTimer->stop();
    if (c.networkTimeout > 0)
        d->timeoutTimer->start(c.networkTimeout);
}

void ChatStream::readIncoming()
{
    if (!d->reply || d->done)
        return;

    d->buffer.append(d->reply->readAll());

    // 归一化 CRLF 为 LF，兼容 \r\n\r\n 和 \n\n 两种帧分隔符
    d->buffer.replace("\r\n", "\n");

    int idx = 0;
    while ((idx = d->buffer.indexOf("\n\n")) != -1)
    {
        const QByteArray frame = d->buffer.left(idx);
        d->buffer.remove(0, idx + 2);
        if (!frame.trimmed().isEmpty())
            processFrame(frame);
    }
}

void ChatStream::processFrame(const QByteArray &frame)
{
    if (client().verbose)
        qDebug() << "QOpenAi [sse]" << frame;

    QByteArray line = frame.trimmed();
    if (!line.startsWith("data:"))
        return;

    const QString payload = QString::fromUtf8(line.mid(5).trimmed());
    if (payload == "[DONE]")
    {
        finishStream();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
    if (!doc.isObject())
    {
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

        // 超时中止（区别于主动取消）
        if (d->timedOut)
        {
            d->done = true;
            cleanupReply();
            emit error(tr("请求超时（%1 ms）。").arg(c.networkTimeout));
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

// ---- 运行时配置 ----

void setUrl(const QString &url)
{
    // base URL（不含端点路径），统一去掉尾部斜杠
    QString base = url;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    client().url = base;
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
    client().networkTimeout = qMax(0, milliseconds);
}

int timeout()
{
    return client().networkTimeout;
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