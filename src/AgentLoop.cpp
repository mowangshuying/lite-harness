#include "AgentLoop.h"

#include <QJsonDocument>
#include <QProcess>
#include <QDir>
#include <QDebug>

#include <TongYiOpenAi/TongYiOpenAi.hpp>

namespace {

using Json = nlohmann::json;

// QJsonObject -> nlohmann::json（通过紧凑 JSON 字符串互转）
Json toNlohmann(const QJsonObject &obj)
{
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return Json::parse(bytes.toStdString());
}

// nlohmann::json -> QJsonObject
QJsonObject fromNlohmann(const Json &json)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(json.dump())).object();
}

} // namespace

AgentLoop::AgentLoop(QObject *parent) : QObject(parent)
{
    // 模型 ID：优先环境变量 MODEL_ID，缺省 qwen-max
    m_model = QString::fromUtf8(qgetenv("MODEL_ID"));
    if (m_model.isEmpty())
        m_model = QStringLiteral("qwen-max");

    // 初始 system prompt
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] =
        QStringLiteral("You are a coding agent. Use bash to solve tasks. Act, don't explain.");
    m_messages.append(systemMessage);
}

AgentLoop::~AgentLoop()
{
    if (m_thread && m_thread->isRunning())
    {
        m_thread->requestInterruption();
        m_thread->quit();
        if (!m_thread->wait(8000))
        {
            qWarning() << "AgentLoop: worker thread did not stop in time.";
            m_thread->terminate();
            m_thread->wait();
        }
    }
    m_messages.clear();
}

void AgentLoop::run(const QString &userMessage)
{
    // 已有一个循环周期在运行则拒绝新消息（快速路径，不加锁）
    if (m_running.loadAcquire())
    {
        emit error(tr("Agent 仍在运行中，请等待完成后再发送。"));
        return;
    }

    QMutexLocker locker(&m_mutex); // 仅保护消息历史的快速追加
    m_running.storeRelease(true);

    // 追加用户消息到会话历史
    QJsonObject userMessageObj;
    userMessageObj[QStringLiteral("role")] = QStringLiteral("user");
    userMessageObj[QStringLiteral("content")] = userMessage;
    m_messages.append(userMessageObj);
    locker.unlock();

    // 每个循环周期启动一个工作线程，结束后自动回收
    auto *thread = QThread::create([this] { processLoop(); });
    thread->setObjectName(QStringLiteral("AgentLoopThread"));
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this] { m_running.storeRelease(false); });
    m_thread = thread;
    thread->start();
}

void AgentLoop::processLoop()
{
    // 防止模型反复请求工具形成死循环
    constexpr int kMaxToolIterations = 30;
    int toolIterations = 0;

    while (true)
    {
        if (QThread::currentThread()->isInterruptionRequested())
            break;

        // 快照当前消息历史（快速，不加锁等待期间不阻塞 UI）
        QJsonArray messagesJson;
        {
            QMutexLocker locker(&m_mutex);
            for (const auto &msg : m_messages)
                messagesJson.append(msg);
        }

        QJsonObject request;
        request[QStringLiteral("model")] = m_model;
        request[QStringLiteral("messages")] = messagesJson;
        request[QStringLiteral("tools")] = createToolsDefinition();

        // 1. 调用 API（同步阻塞，必须在工作线程，期间不持有锁）
        const Json response = TongYiOpenAi::completion().create(toNlohmann(request));

        // 2. 容错：HTTP 错误时返回空 Json
        if (response.is_null() || !response.contains("choices") || response["choices"].empty())
        {
            emit error(tr("API 调用失败：未返回有效响应。请检查网络与 TongYiOpenAi 环境变量配置。"));
            break;
        }

        const QJsonObject respObj = fromNlohmann(response);
        const QJsonArray choices = respObj.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty())
        {
            emit error(tr("API 调用失败：choices 为空。"));
            break;
        }
        const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();

        // 3. 无 tool_calls -> 最终回复，循环结束
        if (!message.contains(QStringLiteral("tool_calls")))
        {
            {
                QMutexLocker locker(&m_mutex);
                m_messages.append(message);
            }
            const QString replyText = message.value(QStringLiteral("content")).toString();
            emit finished(replyText);
            break;
        }

        // 4. 有 tool_calls -> 追加 assistant 消息，逐个执行工具
        if (++toolIterations > kMaxToolIterations)
        {
            emit error(tr("工具调用次数超过上限（%1 次），终止循环。").arg(kMaxToolIterations));
            break;
        }

        {
            QMutexLocker locker(&m_mutex);
            m_messages.append(message);
        }

        const QJsonArray toolCalls = message.value(QStringLiteral("tool_calls")).toArray();
        for (const auto &toolCallValue : toolCalls)
        {
            const QJsonObject toolCall = toolCallValue.toObject();
            const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();

            // 解析 arguments JSON 中的 command 字段
            const QJsonObject args =
                QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
            const QString command = args.value(QStringLiteral("command")).toString();

            const QString output = executeBash(command);
            emit toolOutputReady(command, output);

            // 构建 tool 结果消息回填上下文
            QJsonObject toolResult;
            toolResult[QStringLiteral("role")] = QStringLiteral("tool");
            toolResult[QStringLiteral("tool_call_id")] = toolCall.value(QStringLiteral("id")).toString();
            toolResult[QStringLiteral("content")] = output;
            {
                QMutexLocker locker(&m_mutex);
                m_messages.append(toolResult);
            }
        }
    }
}

QString AgentLoop::executeBash(const QString &command)
{
    // 安全检查：危险命令黑名单
    static const QStringList kDangerous = {
        QStringLiteral("rm -rf /"),
        QStringLiteral("sudo"),
        QStringLiteral("shutdown"),
        QStringLiteral("reboot"),
        QStringLiteral("> /dev/"),
    };
    for (const auto &danger : kDangerous)
    {
        if (command.contains(danger, Qt::CaseInsensitive))
            return QStringLiteral("Error: Dangerous command blocked: %1").arg(command);
    }

    // 使用 QProcess 执行（120 秒超时）
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(QDir::currentPath());
    process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
    if (!process.waitForStarted(5000))
        return QStringLiteral("Error: Failed to start process: %1").arg(process.errorString());
    if (!process.waitForFinished(120000))
    {
        process.kill();
        return QStringLiteral("Error: Timeout (120s)");
    }

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (output.length() > 50000)
        output = output.left(50000); // 截断
    return output.isEmpty() ? QStringLiteral("(no output)") : output;
}

QJsonArray AgentLoop::createToolsDefinition()
{
    QJsonObject tool;
    tool[QStringLiteral("type")] = QStringLiteral("function");

    QJsonObject function;
    function[QStringLiteral("name")] = QStringLiteral("bash");
    function[QStringLiteral("description")] = QStringLiteral("Run a shell command.");

    QJsonObject inputSchema;
    inputSchema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    QJsonObject commandProp;
    commandProp[QStringLiteral("type")] = QStringLiteral("string");
    properties[QStringLiteral("command")] = commandProp;
    inputSchema[QStringLiteral("properties")] = properties;
    inputSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("command")};

    function[QStringLiteral("parameters")] = inputSchema;
    tool[QStringLiteral("function")] = function;

    return QJsonArray{tool};
}