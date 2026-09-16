#include "AgentLoop.h"

#include "QOpenAi.h"

#include <QJsonDocument>
#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QSet>
#include <QPair>
#include <QTimer>
#include <QDebug>

namespace {

// 工具调用轮次上限（防止模型反复请求工具形成死循环）
constexpr int kMaxToolIterations = 30;

// system prompt：告知模型当前工作目录（与 lcc s02 语义一致，多工具版为 "Use tools"）
QString makeSystemPrompt(const QString &workDir)
{
    return QStringLiteral("You are a coding agent at %1. Use tools to solve tasks. Act, don't explain.")
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

// glob 模式转正则（供 runGlob 使用）：
// "**" 匹配任意层级路径；"**/" 允许零层或多层目录；"*" 匹配单层内任意字符（不跨 '/'）；
// "?" 匹配单个非 '/' 字符；其余字符按字面量转义
QRegularExpression globToRegex(const QString &pattern)
{
    QString rx;
    const int size = pattern.size();
    for (int i = 0; i < size; ++i)
    {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('*'))
        {
            if (i + 1 < size && pattern.at(i + 1) == QLatin1Char('*'))
            {
                if (i + 2 < size && pattern.at(i + 2) == QLatin1Char('/'))
                {
                    rx += QStringLiteral("(?:.*/)?"); // "**/" → 零或多层目录
                    i += 2;
                }
                else
                {
                    rx += QStringLiteral(".*"); // 末尾 "**" → 任意剩余（含 '/'）
                    ++i;
                }
            }
            else
            {
                rx += QStringLiteral("[^/]*"); // "*" → 单层通配
            }
        }
        else if (c == QLatin1Char('?'))
        {
            rx += QStringLiteral("[^/]");
        }
        else
        {
            rx += QRegularExpression::escape(QString(c));
        }
    }

    QRegularExpression re(QRegularExpression::anchoredPattern(rx));
    re.setPatternOptions(QRegularExpression::CaseInsensitiveOption); // Windows 文件系统大小写不敏感
    return re;
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
    dispatchToolCall(toolCall);
}

void AgentLoop::onToolFinished(const QJsonObject &toolCall, const QString &toolName,
                               const QString &summary, const QString &output)
{
    // 已被 stop()/析构中断则终止工具链
    if (!m_running)
        return;

    emit toolOutputReady(toolName, summary, output);

    // 构建 tool 结果消息回填上下文
    QJsonObject toolResult;
    toolResult[QStringLiteral("role")] = QStringLiteral("tool");
    toolResult[QStringLiteral("tool_call_id")] = toolCall.value(QStringLiteral("id")).toString();
    toolResult[QStringLiteral("content")] = output;
    m_toolResultsReady.append(toolResult);

    runNextTool();
}

void AgentLoop::dispatchToolCall(const QJsonObject &toolCall)
{
    // 解析工具名与参数（arguments 为流式拼装出的 JSON 字符串）
    const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
    const QString toolName = function.value(QStringLiteral("name")).toString();
    const QJsonObject args =
        QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8()).object();

    // 人类可读摘要（对齐 lcc s02 的 tool_use info：关键参数行）
    QString summary;
    if (toolName == QStringLiteral("bash"))
        summary = args.value(QStringLiteral("command")).toString();
    else if (toolName == QStringLiteral("glob"))
        summary = args.value(QStringLiteral("pattern")).toString();
    else if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
             || toolName == QStringLiteral("edit_file"))
        summary = args.value(QStringLiteral("path")).toString();

    // bash 走异步进程链
    if (toolName == QStringLiteral("bash"))
    {
        executeBashAsync(toolCall, args);
        return;
    }

    // 文件类工具为本地 IO，同步执行；未知工具不中断循环，错误内容作为结果回填（对齐 lcc Unknown 分支）
    QString output;
    if (toolName == QStringLiteral("read_file"))
        output = runReadFile(args);
    else if (toolName == QStringLiteral("write_file"))
        output = runWriteFile(args);
    else if (toolName == QStringLiteral("edit_file"))
        output = runEditFile(args);
    else if (toolName == QStringLiteral("glob"))
        output = runGlob(args);
    else
        output = QStringLiteral("Unknown tool: %1").arg(toolName);

    onToolFinished(toolCall, toolName, summary, output);
}

void AgentLoop::executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args)
{
    const QString command = args.value(QStringLiteral("command")).toString();

    // 安全检查：危险命令黑名单（同步短路，不启动进程）
    for (const auto &danger : dangerousCommands())
    {
        if (command.contains(danger, Qt::CaseInsensitive))
        {
            onToolFinished(toolCall, QStringLiteral("bash"), command,
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

        onToolFinished(toolCall, QStringLiteral("bash"), command, output);
    });

    process->start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
}

QString AgentLoop::safePath(const QString &p, QString *error) const
{
    // 相对路径按工作区解析，绝对路径直接使用；cleanPath 归一化 "../" 与分隔符（Windows 反斜杠转正斜杠）
    const QString joined = QDir::isAbsolutePath(p)
        ? QDir::cleanPath(p)
        : QDir::cleanPath(m_workDir + QLatin1Char('/') + p);

    // 存在部分尽量取 canonical path（消解符号链接/大小写真实形态）；
    // 文件尚不存在时（write 场景）规范化已存在的父目录后接原文件名
    const QFileInfo info(joined);
    QString absPath;
    if (info.exists())
    {
        const QString canonical = info.canonicalFilePath();
        absPath = canonical.isEmpty() ? joined : canonical;
    }
    else
    {
        const QString canonicalParent = QFileInfo(info.dir().absolutePath()).canonicalFilePath();
        absPath = canonicalParent.isEmpty()
            ? joined
            : QDir::cleanPath(canonicalParent + QLatin1Char('/') + info.fileName());
    }

    // 前缀比较带目录分隔符边界（防 "D:/work" 误判 "D:/work-evil"），Windows 下大小写不敏感
    const QString root = QDir::cleanPath(m_workDir);
    if (absPath.compare(root, Qt::CaseInsensitive) != 0
        && !absPath.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
    {
        if (error)
            *error = QStringLiteral("Error: Path escapes workspace: %1").arg(p);
        return QString();
    }
    return absPath;
}

QString AgentLoop::runReadFile(const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    QString err;
    const QString abs = safePath(path, &err);
    if (abs.isEmpty())
        return err;

    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error:%1").arg(file.errorString());

    // UTF-8 按行读取；QTextStream 行为对齐 Python splitlines（末尾换行不产生空行）
    QString text = QString::fromUtf8(file.readAll()); // 非 const：QTextStream 需要 QString*
    QStringList lines;
    QTextStream ts(&text);
    for (QString line = ts.readLine(); !line.isNull(); line = ts.readLine())
        lines.append(line);

    const int limit = args.value(QStringLiteral("limit")).toInt();
    if (limit > 0 && limit < lines.size())
    {
        const int more = lines.size() - limit;
        lines = lines.mid(0, limit);
        lines.append(QStringLiteral("... (%1 more lines)").arg(more));
    }

    QString output = lines.join(QLatin1Char('\n'));
    if (output.length() > 50000)
        output = output.left(50000); // 截断
    if (output.isEmpty())
        output = QStringLiteral("(no output)");
    return output;
}

QString AgentLoop::runWriteFile(const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    const QString content = args.value(QStringLiteral("content")).toString();
    QString err;
    const QString abs = safePath(path, &err);
    if (abs.isEmpty())
        return err;

    // 自动创建父目录（对齐 mkdir(parents=True)）
    if (!QDir().mkpath(QFileInfo(abs).dir().absolutePath()))
        return QStringLiteral("Error:cannot create directory:%1").arg(QFileInfo(abs).dir().absolutePath());

    QFile file(abs);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("Error:%1").arg(file.errorString());
    const QByteArray bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size())
        return QStringLiteral("Error:%1").arg(file.errorString());
    return QStringLiteral("Wrote %1 bytes to %2").arg(bytes.size()).arg(path);
}

QString AgentLoop::runEditFile(const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    const QString oldText = args.value(QStringLiteral("old_text")).toString();
    const QString newText = args.value(QStringLiteral("new_text")).toString();
    QString err;
    const QString abs = safePath(path, &err);
    if (abs.isEmpty())
        return err;

    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error:%1").arg(file.errorString());
    const QString text = QString::fromUtf8(file.readAll());

    // 只替换第一处（对齐 str.replace(old, new, 1)）
    const int index = text.indexOf(oldText);
    if (index < 0)
        return QStringLiteral("Error: text not found in %1").arg(path);
    QString edited = text;
    edited.replace(index, oldText.size(), newText);

    file.close();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("Error:%1").arg(file.errorString());
    const QByteArray bytes = edited.toUtf8();
    if (file.write(bytes) != bytes.size())
        return QStringLiteral("Error:%1").arg(file.errorString());
    return QStringLiteral("Edited %1").arg(path);
}

QString AgentLoop::runGlob(const QJsonObject &args)
{
    // 统一分隔符风格（模型可能给出反斜杠模式）
    QString pattern = args.value(QStringLiteral("pattern")).toString();
    pattern.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QRegularExpression re = globToRegex(pattern);
    if (!re.isValid())
        return QStringLiteral("Error:%1").arg(re.errorString());

    // 以工作区为根递归遍历，按相对路径匹配；结果过滤 safePath 逃逸项（如符号链接指向外部）
    QStringList collected;
    QSet<QString> seen;
    QDirIterator it(m_workDir, QDir::AllEntries | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString rel = QDir(m_workDir).relativeFilePath(it.next());
        if (!re.match(rel).hasMatch())
            continue;
        QString err;
        if (safePath(rel, &err).isEmpty())
            continue;
        if (!seen.contains(rel))
        {
            seen.insert(rel);
            collected.append(rel);
        }
    }

    if (collected.isEmpty())
        return QStringLiteral("(no matches)");

    collected.sort();
    QStringList shown = collected.mid(0, 200); // 输出前 200 条
    if (collected.size() > 200)
        shown.append(QStringLiteral("...(more matches omitted; narrow the pattern)"));
    return shown.join(QLatin1Char('\n'));
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
    // 单个工具定义（OpenAI function schema 风格）；props 为 {参数名, 类型} 列表
    auto makeTool = [](const QString &name, const QString &description,
                       const QList<QPair<QString, QString>> &props, const QStringList &required) {
        QJsonObject properties;
        for (const auto &prop : props)
        {
            QJsonObject schema;
            schema[QStringLiteral("type")] = prop.second;
            properties[prop.first] = schema;
        }

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = properties;
        inputSchema[QStringLiteral("required")] = QJsonArray::fromStringList(required);

        QJsonObject function;
        function[QStringLiteral("name")] = name;
        function[QStringLiteral("description")] = description;
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        return tool;
    };

    QJsonArray tools;
    tools.append(makeTool(QStringLiteral("bash"), QStringLiteral("Run a shell command."),
                          { {QStringLiteral("command"), QStringLiteral("string")} },
                          {QStringLiteral("command")}));
    tools.append(makeTool(QStringLiteral("read_file"), QStringLiteral("Read file contents"),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("limit"), QStringLiteral("integer")} },
                          {QStringLiteral("path")}));
    tools.append(makeTool(QStringLiteral("write_file"), QStringLiteral("Write content to a file"),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("content"), QStringLiteral("string")} },
                          {QStringLiteral("path"), QStringLiteral("content")}));
    // lcc s02 原码 edit_file 漏了 required，此处修正
    tools.append(makeTool(QStringLiteral("edit_file"), QStringLiteral("Replace exact text in a file once."),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("old_text"), QStringLiteral("string")},
                            {QStringLiteral("new_text"), QStringLiteral("string")} },
                          {QStringLiteral("path"), QStringLiteral("old_text"), QStringLiteral("new_text")}));
    // lcc s02 原码 glob 的 required 误写为 "require"，此处修正
    tools.append(makeTool(QStringLiteral("glob"),
                          QStringLiteral("Find files matching a glob pattern; ** matches recursively."),
                          { {QStringLiteral("pattern"), QStringLiteral("string")} },
                          {QStringLiteral("pattern")}));
    return tools;
}