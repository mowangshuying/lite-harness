#include "QOpenAi.h"

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
};

OpenAiClient &client()
{
    static OpenAiClient c;
    return c;
}

} // namespace

namespace QOpenAi {

class ChatStream::Private
{
public:
    QNetworkReply *reply = nullptr;
    QByteArray buffer;       // SSE 累积缓冲
    QString thinking;        // 累积推理原文
    QString content;         // 累积正文原文
    QJsonArray toolCalls;    // 累积的 tool_calls（按 index 对齐，含占位）
    bool done = false;       // 是否已收尾（防重复 emit）
};

ChatStream::ChatStream(QObject *parent) : QObject(parent), d(new Private)
{
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
    
    if (d->reply)
    {
        d->reply->abort();
        cleanupReply();
    }
}

void ChatStream::startRequest(const QJsonObject &input)
{
    OpenAiClient &c = client();
    if (c.url.isEmpty() || c.token.isEmpty())
    {
        // 不在此处同步 emit（调用方此刻尚未连接信号），延迟一拍让 connect 先完成
        QTimer::singleShot(0, this, [this] {
            emit error(tr("未配置 QOpenAiUrl/QOpenAiToken 环境变量。"));
        });
        return;
    }

    QNetworkRequest req(QUrl(c.url)); // url 应为完整 /chat/completions 端点
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + c.token.toUtf8());

    QNetworkReply *reply = c.manager.post(req, QJsonDocument(input).toJson(QJsonDocument::Compact));
    d->reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, &ChatStream::readIncoming);
    connect(reply, &QNetworkReply::finished, this, &ChatStream::finishStream);
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
    d->done = true;

    // 检查网络错误并分层处理
    if (d->reply && d->reply->error() != QNetworkReply::NoError)
    {
        const int httpStatus = d->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorMsg = d->reply->errorString();
        
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
            cleanupReply();
            emit error(tr("HTTP %1 错误: %2").arg(httpStatus).arg(errorMsg));
            return;
        }
        else if (httpStatus >= 500 || httpStatus == 429)
        {
            // 5xx 服务端错误 / 429 限流：可重试（当前先报错，后续可加重试机制）
            cleanupReply();
            emit error(tr("HTTP %1 错误（可重试）: %2").arg(httpStatus).arg(errorMsg));
            return;
        }
        else
        {
            // 其他网络错误（无 HTTP 状态码或未知）
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

void ChatStream::cleanupReply()
{
    if (d->reply)
    {
        d->reply->disconnect(this);
        d->reply->deleteLater();
        d->reply = nullptr;
    }
}

ChatStream *CategoryCompletion::createStream(const QJsonObject &input, QObject *parent)
{
    auto *stream = new ChatStream(parent);
    stream->startRequest(input);
    return stream;
}

CategoryCompletion &completion()
{
    static CategoryCompletion c;
    return c;
}

void __initByEnv()
{
    OpenAiClient &c = client();
    c.url = QString::fromUtf8(qgetenv("QOpenAiUrl"));
    c.token = QString::fromUtf8(qgetenv("QOpenAiToken"));
    if (c.url.isEmpty() || c.token.isEmpty())
        qWarning() << "QOpenAi: 环境变量 QOpenAiUrl / QOpenAiToken 未配置或为空。";
}

} // namespace QOpenAi