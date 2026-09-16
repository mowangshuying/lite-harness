#include "AgentLoop.h"

#include "QOpenAi.h"

#include <QJsonDocument>
#include <QProcess>
#include <QDir>
#include <QTimer>
#include <QDebug>

namespace {

// 工具调用轮次上限（防止模型反复请求工具形成死循环）
constexpr int kMaxToolIterations = 30;

// system prompt：告知模型当前工作目录（与 lcc s01 语义一致）
QString makeSystemPrompt(const QString &workDir)
{
    return QStringLiteral("You are a coding agent at %1. Use bash to solve tasks. Act, don't explain.")
        .arg(workDir);
}

// 危险命令黑名单
const QStringList &dangerousCommands()
{
    static const QStringList list = {
        QStringLiteral("rm -rf /"),
        QStringLiteral("sudo"),
        QStringLiteral("shutdown"),
        QStringLiteral("reboot"),
        QStringLiteral("> /dev/"),
    };
    return list;
}

} // namespace

AgentLoop::AgentLoop(QObject *parent) : QObject(parent)
{
    // 模型 ID：优先环境变量 MODEL_ID，缺省 qwen3.8-max
    m_model = QString::fromUtf8(qgetenv("MODEL_ID"));
    if (m_model.isEmpty())
        m_model = QStringLiteral("qwen3.8-flash");

    // 工作目录默认取进程当前目录
    m_workDir = QDir::currentPath();

    // 初始 system prompt（包含工作目录）
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = makeSystemPrompt(m_workDir);
    m_messages.append(systemMessage);
}

void AgentLoop::setWorkDir(const QString &dir)
{
    // 空串忽略；归一化为绝对路径
    if (dir.isEmpty())
        return;
    m_workDir = QDir(dir).absolutePath();

    // system 消息始终位于历史首位（构造时写入），就地刷新使后续请求反映当前目录
    if (!m_messages.isEmpty())
        m_messages[0][QStringLiteral("content")] = makeSystemPrompt(m_workDir);
}

QString AgentLoop::workDir() const
{
    return m_workDir;
}

AgentLoop::~AgentLoop()
{
    // 与 stop() 相同但静默（不发信号）
    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill();
    }
    m_activeProcesses.clear();

    if (m_currentStream)
    {
        m_currentStream->disconnect(this);
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }
    m_messages.clear();
}

void AgentLoop::run(const QString &userMessage)
{
    // 已有一个循环周期在运行则拒绝新消息
    if (m_running)
    {
        emit error(tr("Agent 仍在运行中，请等待完成后再发送。"));
        return;
    }

    m_running = true;
    m_toolIterations = 0;

    // 追加用户消息到会话历史
    QJsonObject userMessageObj;
    userMessageObj[QStringLiteral("role")] = QStringLiteral("user");
    userMessageObj[QStringLiteral("content")] = userMessage;
    m_messages.append(userMessageObj);

    // 快照历史并发起流式请求（事件驱动，不创建工作线程）
    QJsonArray messagesJson;
    for (const auto &msg : m_messages)
        messagesJson.append(msg);
    startChatRequest(messagesJson);
}

void AgentLoop::startChatRequest(const QJsonArray &messages)
{
    QJsonObject request;
    request[QStringLiteral("model")] = m_model;
    request[QStringLiteral("messages")] = messages;
    request[QStringLiteral("tools")] = createToolsDefinition();
    // 默认开启思考
    request[QStringLiteral("enable_thinking")] = true;
    // stream 由 QOpenAi 内部按流式发送，无需在此显式指定

    QOpenAi::ChatStream *s = QOpenAi::chat().createStream(request, this);
    m_currentStream = s;

    // 增量转发（this 上下文：AgentLoop 销毁自动断连，s 为 this 子对象自动释放）
    connect(s, &QOpenAi::ChatStream::thinkingDelta, this, &AgentLoop::thinkingDelta);
    connect(s, &QOpenAi::ChatStream::textDelta, this, &AgentLoop::textDelta);

    connect(s, &QOpenAi::ChatStream::messageFinished, this, [this, s](const QJsonObject &fullMsg) {
        m_currentStream = nullptr;
        s->deleteLater();

        // 无工具调用 -> 最终回复，循环结束
        const QJsonArray toolCalls = fullMsg.value(QStringLiteral("tool_calls")).toArray();
        if (toolCalls.isEmpty())
        {
            m_messages.append(fullMsg);
            m_running = false;
            emit finished(fullMsg.value(QStringLiteral("content")).toString());
            return;
        }

        // 有工具调用 -> 防死循环计数
        if (++m_toolIterations > kMaxToolIterations)
        {
            m_running = false;
            emit error(tr("工具调用次数超过上限（%1 次），终止循环。").arg(kMaxToolIterations));
            return;
        }

        continueWithToolResults(fullMsg);
    });

    connect(s, &QOpenAi::ChatStream::error, this, [this, s](const QString &msg) {
        m_currentStream = nullptr;
        s->deleteLater();
        m_running = false;
        emit error(msg);
    });
}

void AgentLoop::continueWithToolResults(const QJsonObject &assistantMessage)
{
    // 追加带 tool_calls 的完整 assistant 消息
    m_messages.append(assistantMessage);
    m_pendingToolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    m_toolResultsReady = QJsonArray();

    runNextTool();
}

void AgentLoop::runNextTool()
{
    if (!m_running)
        return;

    if (m_pendingToolCalls.isEmpty())
    {
        // 全部工具执行完成：回填结果到历史并再次请求
        for (const auto &value : m_toolResultsReady)
            m_messages.append(value.toObject());
        m_toolResultsReady = QJsonArray();

        QJsonArray messagesJson;
        for (const auto &msg : m_messages)
            messagesJson.append(msg);
        startChatRequest(messagesJson);
        return;
    }

    const QJsonObject toolCall = m_pendingToolCalls.takeAt(0).toObject();
    executeBashAsync(toolCall);
}

void AgentLoop::onToolFinished(const QJsonObject &toolCall, const QString &command, const QString &output)
{
    // 已被 stop()/析构中断则终止工具链
    if (!m_running)
        return;

    emit toolOutputReady(command, output);

    // 构建 tool 结果消息回填上下文
    QJsonObject toolResult;
    toolResult[QStringLiteral("role")] = QStringLiteral("tool");
    toolResult[QStringLiteral("tool_call_id")] = toolCall.value(QStringLiteral("id")).toString();
    toolResult[QStringLiteral("content")] = output;
    m_toolResultsReady.append(toolResult);

    runNextTool();
}

void AgentLoop::executeBashAsync(const QJsonObject &toolCall)
{
    // 解析参数（id / function.arguments 中的 command）
    const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
    const QJsonObject args =
        QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
    const QString command = args.value(QStringLiteral("command")).toString();

    // 安全检查：危险命令黑名单（同步短路，不启动进程）
    for (const auto &danger : dangerousCommands())
    {
        if (command.contains(danger, Qt::CaseInsensitive))
        {
            onToolFinished(toolCall, command,
                           QStringLiteral("Error: Dangerous command blocked: %1").arg(command));
            return;
        }
    }

    // 异步执行（QProcess 为 this 子对象，析构自动清理）
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(m_workDir);
    m_activeProcesses.append(process);

    // 120 秒超时：kill 后 finished 信号触发，靠标志区分“超时被杀” vs “正常结束”
    auto *timedOut = new bool(false);
    QTimer::singleShot(120000, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });

    connect(process, &QProcess::finished, this,
            [this, process, toolCall, command, timedOut](int, QProcess::ExitStatus) {
        m_activeProcesses.removeAll(process);

        QString output;
        if (*timedOut)
        {
            output = QStringLiteral("Error: Timeout (120s)");
        }
        else
        {
            output = QString::fromLocal8Bit(process->readAllStandardOutput());
            if (output.length() > 50000)
                output = output.left(50000); // 截断
            if (output.isEmpty())
                output = QStringLiteral("(no output)");
        }
        delete timedOut;
        process->deleteLater();

        onToolFinished(toolCall, command, output);
    });

    process->start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
}

void AgentLoop::stop()
{
    if (!m_running)
        return;

    // 中断正在执行的 QProcess（其 finished 后 onToolFinished 因 m_running=false 不再继续）
    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill();
    }
    m_activeProcesses.clear();

    // 主动取消当前流（cancel 内部设 done=true，后续信号不再处理）
    if (m_currentStream)
    {
        static_cast<QOpenAi::ChatStream*>(m_currentStream.data())->cancel();
        m_currentStream->disconnect(this);
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }

    m_running = false;
    emit error(tr("已停止。"));
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