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

// 硬禁止列表（lcc s03 DENY_LIST）：命中即拒绝，不询问
const QStringList &denyPatterns()
{
    static const QStringList list = {
        QStringLiteral("rm -rf /"),
        QStringLiteral("sudo"),
        QStringLiteral("shutdown"),
        QStringLiteral("reboot"),
        QStringLiteral("mkfs"),
        QStringLiteral("dd if="),
        QStringLiteral("> /dev/sda"),
    };
    return list;
}

// 破坏性命令词正则（lcc s03 DESTRUCTIVE_COMMAND_WORD，防绕过升级）：
// (?i) 忽略大小写；(?:^|[;&|()\n{}"'`]) 匹配串首或分隔符/花括号/引号（覆盖 powershell -Command "..." 与 & {...} 嵌套写法）；
// (?:rm|del|erase|ri|rmdir|rd|Remove-Item) 各类删除命令及别名；(?=\s|$|[;&|(){}]) 词尾断言防 "rms" 类前缀误伤。
// Qt6 PCRE 原生支持 (?i) 与 lookahead；反引号在原始字符串中无需转义
bool containsDestructiveCommand(const QString &command)
{
    static const QRegularExpression re(QStringLiteral(
        R"RE((?i)(?:^|[;&|()\n{}"'`])\s*(?:rm|del|erase|ri|rmdir|rd|Remove-Item)(?=\s|$|[;&|(){}]))RE"));
    return re.match(command).hasMatch();
}

// 工具调用的关键参数摘要（lcc s02 tool_use info：bash→command、glob→pattern、文件工具→path）
QString toolSummary(const QString &toolName, const QJsonObject &args)
{
    if (toolName == QStringLiteral("bash"))
        return args.value(QStringLiteral("command")).toString();
    if (toolName == QStringLiteral("glob"))
        return args.value(QStringLiteral("pattern")).toString();
    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
        || toolName == QStringLiteral("edit_file"))
        return args.value(QStringLiteral("path")).toString();
    return QString();
}

// PreToolUse 钩子的“需询问”返回协议前缀（C++ 移植约定，有意偏差）：
// lcc 的 permission 钩子内部同步 input() 询问后直接返回拦截文本或 None；
// GUI 无阻塞 stdin，钩子改为携带 "ASK:<reason>" 返回，由 dispatchToolCall 识别后
// 发 permissionRequired 异步挂起（s03 机制，UI 契约零改动）。
// 不带该前缀的非空返回值一律视为硬拦截文本（回填为 tool_result）。
// 现有拦截文案（"Blocked: ..."）不以 "ASK:" 开头，两路径无冲突。
const QString &askPrefix()
{
    static const QString prefix = QStringLiteral("ASK:");
    return prefix;
}

// 从工具调用中提取工具名与解析后的 arguments（钩子与 dispatchToolCall 共用；
// arguments 为流式拼装出的 JSON 字符串）
QString callToolName(const QJsonObject &toolCall)
{
    return toolCall.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString();
}

QJsonObject callToolArgs(const QJsonObject &toolCall)
{
    const QString raw = toolCall.value(QStringLiteral("function")).toObject()
                            .value(QStringLiteral("arguments")).toString();
    return QJsonDocument::fromJson(raw.toUtf8()).object();
}

// log_before 钩子的参数预览（对应 lcc str(list(block.input.values())[:2])[:60]）：
// 取前两个参数值拼为 "[v1, v2]" 后截 60 字符。
// 偏差：QJsonObject 按键名字典序遍历（Python dict 为文档插入序）；非字符串值经 QVariant 转文本
QString argsPreview(const QJsonObject &args)
{
    QStringList head;
    for (auto it = args.constBegin(); it != args.constEnd() && head.size() < 2; ++it)
        head.append(it.value().toVariant().toString());
    return (QStringLiteral("[") + head.join(QStringLiteral(", ")) + QStringLiteral("]")).left(60);
}

// log_after 钩子的工具参数描述（逐字对应 lcc log_after_use_tool_hook 的 info 分支文案）。
// 偏差：lcc 用 block.input[key] 直接取值（缺 key 会 KeyError），此处 .toString() 缺省为空串
QString toolUseInfo(const QString &toolName, const QJsonObject &args)
{
    if (toolName == QStringLiteral("bash"))
        return QStringLiteral("command: ") + args.value(QStringLiteral("command")).toString();
    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
        || toolName == QStringLiteral("edit_file"))
        return QStringLiteral("path: ") + args.value(QStringLiteral("path")).toString();
    if (toolName == QStringLiteral("glob"))
        return QStringLiteral("pattern: ") + args.value(QStringLiteral("pattern")).toString();
    return QString();
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

    // 内置生命周期钩子（对齐 lcc s04 模块尾部的 register_hook 清单）
    registerBuiltinHooks();

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

    // UserPromptSubmit 钩子（lcc s04）：用户消息入历史前触发
    // （内置 context_inject 打印当前工作目录；返回值在 lcc 中亦被忽略）
    triggerUserPromptSubmitHooks(userMessage);

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

            // Stop 钩子（lcc s04）：循环即将结束（无 tool_calls）时触发。
            // lcc 语义：若返回非空则作为一条 user 消息注入历史，但无论如何都 return——
            // 并不存在“强制续跑”，注入内容只影响下一轮上下文（内置 summary_hook 恒返回
            // None，故非空分支在 lcc 中实为死代码，此处原样保留为扩展点；
            // lcc 原文误拼 "conent"，此处按正确键名 "content" 写入）
            const QString force = triggerStopHooks();
            if (!force.isEmpty())
            {
                QJsonObject injected;
                injected[QStringLiteral("role")] = QStringLiteral("user");
                injected[QStringLiteral("content")] = force;
                m_messages.append(injected);
            }

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

void AgentLoop::dispatchToolCall(const QJsonObject &toolCall, bool permissionGranted)
{
    // 解析工具名与参数（arguments 为流式拼装出的 JSON 字符串）
    const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
    const QString toolName = function.value(QStringLiteral("name")).toString();
    const QJsonObject args =
        QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
    const QString summary = toolSummary(toolName, args);

    // PreToolUse 钩子链（lcc s04）：s03 的权限门移入内置 permission 钩子之后，此处只处理
    // 通用返回协议。triggerPreToolUseHooks 首个非空返回即短路（逐字对齐 lcc trigger_hooks）：
    // 1) 以 "ASK:" 开头 → 需询问：发 permissionRequired 并暂停队列，等 resolvePermission() 裁决
    //    （询问文案与 s03 逐字一致；ASK 前缀为 C++ 异步移植协议，lcc 为控制台同步 input）
    // 2) 非空且无 ASK 前缀 → 硬拒绝（deny 列表命中）：不询问直接拒绝，原因写进 tool_result
    // 3) 空 → 放行，进入执行路径
    // permissionGranted=true 为批准后的续跑路径：permission 钩子内部短路，其余钩子（日志）照常执行
    const QString gate = triggerPreToolUseHooks(toolCall, permissionGranted);
    if (!gate.isEmpty())
    {
        if (gate.startsWith(askPrefix()))
        {
            m_awaitingPermission = true;
            m_pendingPermissionCall = toolCall;
            emit permissionRequired(toolName, summary, gate.mid(askPrefix().size()));
            return; // 队列暂停：不回填、不请求，等用户裁决
        }
        onToolFinished(toolCall, toolName, summary, gate);
        return;
    }

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

    // PostToolUse 钩子（lcc s04）：handler 产出结果后、回填前触发。
    // 文件工具与未知工具共用此出口（lcc 中 Unknown 分支同样触发 PostToolUse；
    // 被 PreToolUse 拦截的调用不会走到这里，与 lcc 拒绝即 continue 的语义一致）
    triggerPostToolUseHooks(toolCall, output);

    onToolFinished(toolCall, toolName, summary, output);
}

QString AgentLoop::checkDenyList(const QString &command) const
{
    // 子串匹配，按列表顺序取第一个命中项（对齐 lcc check_deny_list；
    // 大小写不敏感与仓库既有 bash 黑名单风格一致，较 lcc 的大小写敏感更严格——Windows 命令名本就不区分大小写）
    for (const QString &pattern : denyPatterns())
    {
        if (command.contains(pattern, Qt::CaseInsensitive))
            return QStringLiteral("Blocked: %1 is on the deny list").arg(pattern);
    }
    return QString();
}

QString AgentLoop::checkPermissionRules(const QString &toolName, const QJsonObject &args) const
{
    // 规则 1（lcc PERMISSION_RULES）：read/write/edit_file 的 path 逃逸工作区。
    // lcc 用未归一化的拼接判定，这里复用 safePath 的越界检测结果，语义一致
    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
        || toolName == QStringLiteral("edit_file"))
    {
        QString err;
        if (safePath(args.value(QStringLiteral("path")).toString(), &err).isEmpty())
            return QStringLiteral("Writing outside workspace");
        return QString();
    }

    // 规则 2：bash 命中破坏性命令词正则，或包含关键子串（子串部分区分大小写，逐字对齐 lcc）
    if (toolName == QStringLiteral("bash"))
    {
        const QString command = args.value(QStringLiteral("command")).toString();
        if (containsDestructiveCommand(command) || command.contains(QStringLiteral("rm "))
            || command.contains(QStringLiteral("> /etc/")) || command.contains(QStringLiteral("chmod 777")))
            return QStringLiteral("Potentially destructive command");
    }

    // 其余工具（glob 等）无询问规则
    return QString();
}

void AgentLoop::resolvePermission(bool allow)
{
    // UI 收到 permissionRequired 后回传裁决；无待决询问时忽略
    if (!m_awaitingPermission)
        return;

    const QJsonObject toolCall = m_pendingPermissionCall;
    m_pendingPermissionCall = QJsonObject();
    m_awaitingPermission = false;

    // stop() 已清理状态的话此处兜底：不再续跑工具链
    if (!m_running)
        return;

    if (!allow)
    {
        // 拒绝：照常回填 "Permission denied" 并发 toolOutputReady（UI 可展示被拒），随后继续队列。
        // 不触发 PostToolUse——lcc 中用户拒绝即 continue，handler 未运行（s04 语义）
        const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
        const QString toolName = function.value(QStringLiteral("name")).toString();
        const QJsonObject args = QJsonDocument::fromJson(
            function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
        onToolFinished(toolCall, toolName, toolSummary(toolName, args), QStringLiteral("Permission denied"));
        return;
    }

    // 允许：该 toolCall 重新走完整分发链（permission 钩子在 permissionGranted=true 时内部短路，
    // 不再二次询问；日志等其他 PreToolUse 钩子照常执行，对齐 lcc 批准后继续走链的行为。
    // executeBashAsync 内置黑名单仍生效——双层防御）
    dispatchToolCall(toolCall, /*permissionGranted = */ true);
}

// ---------------------------------------------------------------------------
// 生命周期钩子（lcc s04）：注册顺序即执行顺序，与 lcc 尾部 register_hook 清单逐一对应。
// 返回值约定：空串 ≡ lcc 的 None（放行/继续链）；非空短路（见各 trigger 函数与 askPrefix 注释）
// ---------------------------------------------------------------------------

void AgentLoop::registerBuiltinHooks()
{
    // UserPromptSubmit: context_inject —— 打印会话工作目录（lcc 用 Path.cwd()，此处对应 m_workDir）。
    // "UserPromtSubmit" 为 lcc 原文拼写，按文案对齐原则逐字保留
    m_userPromptSubmitHooks.append([this](const QString &) -> QString {
        qDebug().noquote() << QStringLiteral("[HOOK] UserPromtSubmit: working in %1").arg(m_workDir);
        return QString();
    });

    // PreToolUse #1: permission —— s03 的 checkDenyList / checkPermissionRules 检查逻辑原样移入
    // 钩子（文案逐字不变）。permissionGranted=true 为批准后续跑：短路返回空，避免二次询问
    m_preToolUseHooks.append([this](const QJsonObject &toolCall, bool permissionGranted) -> QString {
        if (permissionGranted)
            return QString();

        const QString toolName = callToolName(toolCall);
        const QJsonObject args = callToolArgs(toolCall);

        if (toolName == QStringLiteral("bash"))
        {
            const QString blocked = checkDenyList(args.value(QStringLiteral("command")).toString());
            if (!blocked.isEmpty())
                return blocked; // 硬拒绝：直接作为拦截文本回填
        }

        const QString reason = checkPermissionRules(toolName, args);
        if (!reason.isEmpty())
            return askPrefix() + reason; // 需询问：异步协议，由 dispatchToolCall 挂起队列

        return QString();
    });

    // PreToolUse #2: log_before —— 打印工具名与参数预览（lcc: [HOOK] name(args_preview)）
    m_preToolUseHooks.append([](const QJsonObject &toolCall, bool) -> QString {
        qDebug().noquote() << QStringLiteral("[HOOK] %1(%2)")
                                  .arg(callToolName(toolCall), argsPreview(callToolArgs(toolCall)));
        return QString();
    });

    // PostToolUse #1: log_after —— 打印工具调用信息与输出（lcc 原文案；
    // lcc 在此再次打印 tool_use 行，与 log_before 有意重复，原样保留）
    m_postToolUseHooks.append([](const QJsonObject &toolCall, const QString &output) -> QString {
        const QString toolName = callToolName(toolCall);
        qDebug().noquote() << QStringLiteral("[HOOK] tool_use: %1 - %2")
                                  .arg(toolName, toolUseInfo(toolName, callToolArgs(toolCall)));
        qDebug().noquote() << QStringLiteral("[HOOK] tool_result:%1").arg(output);
        return QString();
    });

    // PostToolUse #2: large_output —— 超长输出提醒（lcc 阈值 100000 字符）。
    // 注：本实现中 bash/read_file 输出在 handler 内已先行截断到 50000，钩子实际难以触发，
    // 与 lcc 现状一致（lcc 的 run_bash 同样先 [:50000]），保留以对齐结构
    m_postToolUseHooks.append([](const QJsonObject &toolCall, const QString &output) -> QString {
        if (output.size() > 100000)
            qDebug().noquote() << QStringLiteral("[HOOK] Large output from %1: %2 chars")
                                      .arg(callToolName(toolCall)).arg(output.size());
        return QString();
    });

    // Stop: summary —— 统计整场会话的工具调用次数并打印。
    // lcc 扫描全量消息中的 tool_result 块；本实现为 OpenAI 格式，等价于统计 role=="tool"
    // 的历史消息条数（含被拒/被拦截的回填项，lcc 同样计入），故直接扫 m_messages 而非成员计数
    m_stopHooks.append([this]() -> QString {
        int toolCount = 0;
        for (const QJsonObject &msg : m_messages)
        {
            if (msg.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
                ++toolCount;
        }
        qDebug().noquote() << QStringLiteral("[HOOK] Stop: session used %1 tool calls").arg(toolCount);
        return QString();
    });
}

QString AgentLoop::triggerUserPromptSubmitHooks(const QString &prompt)
{
    for (const auto &hook : m_userPromptSubmitHooks)
    {
        const QString result = hook(prompt);
        if (!result.isEmpty())
            return result; // 首个非空即短路（lcc: if result is not None: return result）
    }
    return QString();
}

QString AgentLoop::triggerPreToolUseHooks(const QJsonObject &toolCall, bool permissionGranted)
{
    for (const auto &hook : m_preToolUseHooks)
    {
        const QString result = hook(toolCall, permissionGranted);
        if (!result.isEmpty())
            return result;
    }
    return QString();
}

QString AgentLoop::triggerPostToolUseHooks(const QJsonObject &toolCall, const QString &output)
{
    for (const auto &hook : m_postToolUseHooks)
    {
        const QString result = hook(toolCall, output);
        if (!result.isEmpty())
            return result;
    }
    return QString();
}

QString AgentLoop::triggerStopHooks()
{
    for (const auto &hook : m_stopHooks)
    {
        const QString result = hook();
        if (!result.isEmpty())
            return result;
    }
    return QString();
}

void AgentLoop::executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args)
{
    const QString command = args.value(QStringLiteral("command")).toString();

    // 安全检查：危险命令黑名单（同步短路，不启动进程）。
    // s01 内部黑名单与 s03 deny 列表双层防御保留；该输出按 handler 产出对待
    // （对齐 lcc run_bash 的返回文案），同样触发 PostToolUse
    for (const auto &danger : dangerousCommands())
    {
        if (command.contains(danger, Qt::CaseInsensitive))
        {
            const QString output = QStringLiteral("Error: Dangerous command blocked: %1").arg(command);
            triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, QStringLiteral("bash"), command, output);
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

        // PostToolUse 钩子（lcc s04）：bash handler 产出后、回填前触发
        triggerPostToolUseHooks(toolCall, output);

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

    // 待决权限询问：视为 deny，直接回填历史保持 tool_use/tool_result 配对完整
    //（OpenAI 协议要求每个 tool_call 必有对应 tool 消息；不经 onToolFinished 以免续跑队列或发展示信号）
    if (m_awaitingPermission)
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            m_pendingPermissionCall.value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("Permission denied");
        m_messages.append(toolResult);
        m_pendingPermissionCall = QJsonObject();
        m_awaitingPermission = false;
    }

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