#include "AgentLoop.h"

#include "QOpenAi.h"
#include "SubAgent.h" // startSubAgentTask/cancelSubAgent/resolvePermission 需要完整类型

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

#include <memory>

namespace {

// 工具调用轮次上限（防止模型反复请求工具形成死循环）
constexpr int kMaxToolIterations = 30;

// system prompt（lcc s07 loop.py build_system_prompt 原文逐字移植）：告知模型工作目录 +
// 技能目录清单，并指引按需用 load_skill 读取全文。与 s06 三段无空格拼接不同，s07 起句间
// 为正常空格（lcc 首段以 "tasks. " 结尾的空格真实存在）；\n\n 空行结构逐字保留。
// s06 的 todo_write/task 指引句被 lcc 官方移除，此处不保留。
// %2 = 技能目录文本（skillsCatalog）；多参 arg() 单次替换，替换值中的 % 字符不会被二次展开
QString makeSystemPrompt(const QString &workDir, const QString &skillCatalog)
{
    return QStringLiteral(
               "You are a coding agent at %1. Use tools to solve tasks. "
               "Act, don't explain.\n\n"
               "Skills available:\n%2\n\n"
               "Use load_skill to read the full instructions when a skill applies.")
        .arg(workDir, skillCatalog);
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

// 工具调用的关键参数摘要（lcc s02 tool_use info：bash→command、glob→pattern、文件工具→path；
// s05 起 todo_write 为固定文案；s06 起 task→prompt；s07 起 load_skill→name，对齐 lcc 日志语义）
QString toolSummary(const QString &toolName, const QJsonObject &args)
{
    if (toolName == QStringLiteral("bash"))
        return args.value(QStringLiteral("command")).toString();
    if (toolName == QStringLiteral("glob"))
        return args.value(QStringLiteral("pattern")).toString();
    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
        || toolName == QStringLiteral("edit_file"))
        return args.value(QStringLiteral("path")).toString();
    if (toolName == QStringLiteral("todo_write"))
        return QStringLiteral("update task list"); // lcc s06 原文（hooks.py）已去掉末尾冒号
    if (toolName == QStringLiteral("task"))
        return args.value(QStringLiteral("prompt")).toString();
    if (toolName == QStringLiteral("load_skill"))
        return args.value(QStringLiteral("name")).toString(); // lcc s07：摘要取技能名
    return QString();
}

// PreToolUse 钩子的“需询问”返回协议前缀（C++ 移植约定，有意偏差）：
// lcc 的 permission 钩子内部同步 input() 询问后直接返回拦截文本或 None；
// GUI 无阻塞 stdin，钩子改为携带 "ASK:<reason>" 返回，由 executeTool 识别后
// 发 permissionRequired 异步挂起（s03 机制，UI 契约零改动）。
// 不带该前缀的非空返回值一律视为硬拦截文本（回填为 tool_result）。
// 现有拦截文案（"Blocked: ..."）不以 "ASK:" 开头，两路径无冲突。
const QString &askPrefix()
{
    static const QString prefix = QStringLiteral("ASK:");
    return prefix;
}

// 从工具调用中提取工具名与解析后的 arguments（钩子与 executeTool 共用；
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
    if (toolName == QStringLiteral("todo_write"))
        return QStringLiteral("update task list"); // lcc s06 原文（hooks.py）已去掉末尾冒号
    if (toolName == QStringLiteral("task"))
        // lcc 原文为 f"task: {block.input.get('prompt','')}" 不截断；task prompt 可能很长，
        // 为避免日志刷屏截 60 字符（裁决项，有意偏离 lcc）
        return QStringLiteral("task: ")
            + args.value(QStringLiteral("prompt")).toString().left(60);
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

    // 技能扫描（lcc s07）：构造时扫描一次 <m_workDir>/skills/*/SKILL.md，目录注入 system prompt
    scanSkills();

    // 初始 system prompt（包含工作目录与技能目录，lcc s07）
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = makeSystemPrompt(m_workDir, skillsCatalog());
    m_messages.append(systemMessage);
}

void AgentLoop::setWorkDir(const QString &dir)
{
    // 空串忽略；归一化为绝对路径
    if (dir.isEmpty())
        return;
    m_workDir = QDir(dir).absolutePath();

    // lcc s07 仅在启动时扫描一次；lite 有意超集：换工作目录时重扫技能并同步重建
    // system prompt（技能目录随工作目录走，避免陈旧清单误导模型）
    scanSkills();

    // system 消息始终位于历史首位（构造时写入），就地刷新使后续请求反映当前目录与技能
    if (!m_messages.isEmpty())
        m_messages[0][QStringLiteral("content")] = makeSystemPrompt(m_workDir, skillsCatalog());
}

QString AgentLoop::workDir() const
{
    return m_workDir;
}

AgentLoop::~AgentLoop()
{
    // 与 stop() 相同但静默（不发信号）
    // lcc s06 R1 收口三路之一（析构）：先级联取消子代理——cancel 抑制其完成回调，
    // "(cancelled)" 回填进即将清空的历史属良性无害
    cancelSubAgent();

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
    // lcc s05：rounds_since_todo 为 loop() 的局部变量——每轮用户提问（run）从零起步
    m_roundsSinceTodo = 0;

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
    // 输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致）
    request[QStringLiteral("max_tokens")] = 8000;
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

            // Stop 钩子（lcc s04 引入，s06 起为"续跑"语义）：返回非空则作为一条 user
            // 消息注入历史并发起新一轮请求（消耗 m_toolIterations，kMaxToolIterations=30
            // 兜底，不加额外计数上限）。内置 summary 钩子恒返回空串，故默认行为与 s04 一致
            // 直接收尾。lcc 原文误拼 "conent"，此处按正确键名 "content" 写入
            const QString force = triggerStopHooks();
            if (!force.isEmpty())
            {
                QJsonObject injected;
                injected[QStringLiteral("role")] = QStringLiteral("user");
                injected[QStringLiteral("content")] = force;
                m_messages.append(injected);

                if (++m_toolIterations > kMaxToolIterations)
                {
                    m_running = false;
                    emit error(tr("工具调用次数超过上限（%1 次），终止循环。").arg(kMaxToolIterations));
                    return;
                }
                QJsonArray messagesJson;
                for (const auto &msg : m_messages)
                    messagesJson.append(msg);
                startChatRequest(messagesJson);
                return;
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
        // lcc s06 R1 收口三路之一：错误链同样级联取消子代理并合成 "(cancelled)" 回填
        // （串行队列下宿主流错误与子代理运行实际互斥，此处为防御性接线）
        cancelSubAgent();
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
    // lcc s05：used_todo 为每批 tool_calls 的轮内标志，新批次开始归零
    m_usedTodoThisRound = false;

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

        // 待办提醒（lcc s05 引入，s06 改并入形态）：本批 tool_calls 未"执行"todo_write 则
        // 计数 +1，执行过则归零。与 lcc 一致：权限门拦截/用户拒绝的 todo_write 不算执行
        // （handler 未跑）；走到 handler 的即使返回校验错误也算执行过。
        // lcc s06 把提醒文本并入最后一条 tool_result 的 content 尾部（OpenAI 协议 tool 消息
        // content 为扁平字符串，直接字符串拼接；提醒内文逐字一致，不再发独立 user 消息）
        if (m_usedTodoThisRound)
            m_roundsSinceTodo = 0;
        else
            ++m_roundsSinceTodo;
        if (m_roundsSinceTodo >= 3)
        {
            const QString reminder =
                QStringLiteral("\n\n<reminder>Update your todos.</reminder>");
            for (int i = m_messages.size() - 1; i >= 0; --i)
            {
                if (m_messages.at(i).value(QStringLiteral("role")).toString()
                    == QStringLiteral("tool"))
                {
                    QJsonObject &tail = m_messages[i];
                    tail[QStringLiteral("content")] =
                        tail.value(QStringLiteral("content")).toString() + reminder;
                    break;
                }
            }
            m_roundsSinceTodo = 0;
        }

        QJsonArray messagesJson;
        for (const auto &msg : m_messages)
            messagesJson.append(msg);
        startChatRequest(messagesJson);
        return;
    }

    const QJsonObject toolCall = m_pendingToolCalls.takeAt(0).toObject();
    executeTool(toolCall, mainToolHandlers(), /*permissionGranted=*/false);
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

void AgentLoop::executeTool(const QJsonObject &toolCall,
                            const QHash<QString, ToolHandler> &handlers,
                            bool permissionGranted)
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

    // bash 走异步进程链（不进 handler 表：跨事件循环回填，表内只放同步工具）
    if (toolName == QStringLiteral("bash"))
    {
        executeBashAsync(toolCall, args);
        return;
    }

    // task 走子代理异步链（lcc s06）：独立上下文黑盒，完成后经回调走 onToolFinished 收口
    if (toolName == QStringLiteral("task"))
    {
        startSubAgentTask(toolCall, args);
        return;
    }

    // 其余工具经 handler 表同步路由；未注册名称不中断循环，错误内容作为结果回填
    // （对齐 lcc Unknown 分支；lite-harness 文案 "Unknown tool: %1" 保持不变）
    const auto it = handlers.constFind(toolName);
    const QString output = it == handlers.constEnd()
        ? QStringLiteral("Unknown tool: %1").arg(toolName)
        : it.value()(args);

    // lcc s05 语义保留：只要 todo_write 的 handler 跑过即视为本轮已用 todo（校验错误输出
    // 同样算）；被权限门拦截的不会走到这里，不置位（lcc s06 中拦截也置位，此为 s05 行为红线，
    // 作为已知偏差记录）
    if (toolName == QStringLiteral("todo_write"))
        m_usedTodoThisRound = true;

    // PostToolUse 钩子（lcc s04）：handler 产出结果后、回填前触发。
    // 表内工具与未知工具共用此出口（lcc 中 Unknown 分支同样触发 PostToolUse；
    // 被 PreToolUse 拦截的调用不会走到这里，与 lcc 拒绝即 continue 的语义一致）
    triggerPostToolUseHooks(toolCall, output);

    onToolFinished(toolCall, toolName, summary, output);
}

QString AgentLoop::checkDenyList(const QString &command)
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

QString AgentLoop::checkPermissionRules(const QString &workDir, const QString &toolName,
                                        const QJsonObject &args)
{
    // 规则 1（lcc PERMISSION_RULES）：read/write/edit_file 的 path 逃逸工作区。
    // lcc 用未归一化的拼接判定，这里复用 safePathIn 的越界检测结果，语义一致
    // （s06 起以显式 workDir 参数为准：子代理共用同一逻辑、各查各的沙箱根）
    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("write_file")
        || toolName == QStringLiteral("edit_file"))
    {
        QString err;
        if (safePathIn(workDir, args.value(QStringLiteral("path")).toString(), &err).isEmpty())
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
    // UI 收到 permissionRequired 后回传裁决（宿主询问与子代理转发的询问共用本入口，
    // 串行队列保证同一时刻至多一方等待）。lcc s06：路由到当前挂起方；无待决询问时忽略
    if (m_awaitingPermission)
    {
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

        // 允许：该 toolCall 重新走完整执行链（permission 钩子在 permissionGranted=true 时内部短路，
        // 不再二次询问；日志等其他 PreToolUse 钩子照常执行，对齐 lcc 批准后继续走链的行为。
        // executeBashAsync 内置黑名单仍生效——双层防御）
        executeTool(toolCall, mainToolHandlers(), /*permissionGranted = */ true);
        return;
    }

    // 宿主未在询问：若子代理正在等待裁决，把决定转发给它
    if (m_activeSub && m_activeSub->isAwaitingPermission())
        m_activeSub->resolvePermission(allow);
}

// ---------------------------------------------------------------------------
// 工具 handler 表与 task 子代理（lcc s06）
// ---------------------------------------------------------------------------

QHash<QString, AgentLoop::ToolHandler> AgentLoop::baseFileToolHandlers(const QString &workDir)
{
    // 宿主与子代理共用的同步文件工具集（lcc s06 toolsHandlers/subToolsHandlers 的交集部分）：
    // 各自以传入的 workDir 为沙箱根构建，互不串扰
    QHash<QString, ToolHandler> handlers;
    handlers.insert(QStringLiteral("read_file"), [workDir](const QJsonObject &args) {
        return runReadFileIn(workDir, args);
    });
    handlers.insert(QStringLiteral("write_file"), [workDir](const QJsonObject &args) {
        return runWriteFileIn(workDir, args);
    });
    handlers.insert(QStringLiteral("edit_file"), [workDir](const QJsonObject &args) {
        return runEditFileIn(workDir, args);
    });
    handlers.insert(QStringLiteral("glob"), [workDir](const QJsonObject &args) {
        return runGlobIn(workDir, args);
    });
    return handlers;
}

QHash<QString, AgentLoop::ToolHandler> AgentLoop::mainToolHandlers()
{
    // 主循环同步工具集：文件四件套 + todo_write + load_skill（bash/task 为 executeTool 异步特判）；
    // lcc s07：load_skill 仅主循环注册，子代理工具白名单不含它（见 SubAgent filterSubTools）
    QHash<QString, ToolHandler> handlers = baseFileToolHandlers(m_workDir);
    handlers.insert(QStringLiteral("todo_write"), [this](const QJsonObject &args) {
        return runTodoWrite(args);
    });
    handlers.insert(QStringLiteral("load_skill"), [this](const QJsonObject &args) {
        return runLoadSkill(args);
    });
    return handlers;
}

void AgentLoop::startSubAgentTask(const QJsonObject &toolCall, const QJsonObject &args)
{
    // 串行队列下同一时刻至多一个子代理；异常残留时直接拒绝重复启动
    if (m_activeSub)
        return;

    SubAgent *sub = new SubAgent(this, this, m_workDir, m_model,
                                 args.value(QStringLiteral("prompt")).toString());
    // 子代理权限询问透明转发：复用宿主同一个 3 参 permissionRequired 信号，UI 零改动
    connect(sub, &SubAgent::permissionRequired, this, &AgentLoop::permissionRequired);

    m_activeSub = sub;
    m_pendingTaskCall = toolCall;

    // 完成回调：子代理黑盒收口，仅把最终汇总文本作为 tool_result 交还父循环
    //（"task" 的 toolOutputReady 供 UI 展示；stop()/错误链已先行收口时 onToolFinished
    // 的 !m_running 兜底自然静默）
    sub->start([this, toolCall, args](const QString &result) {
        m_activeSub = nullptr;
        m_pendingTaskCall = QJsonObject();
        onToolFinished(toolCall, QStringLiteral("task"),
                       toolSummary(QStringLiteral("task"), args), result);
    });
}

void AgentLoop::cancelSubAgent()
{
    if (!m_activeSub)
        return;

    SubAgent *sub = m_activeSub.data();
    m_activeSub = nullptr;
    sub->cancel();      // 级联：kill 流与进程、丢弃其待裁决询问、抑制完成回调
    sub->deleteLater();

    // 为父级 task 调用合成 "(cancelled)" tool_result，直写历史保持 tool_use/tool_result
    // 配对（不经 onToolFinished：停发展示信号、不续跑队列——s03 stop() 待决权限的同款收口）
    if (!m_pendingTaskCall.isEmpty())
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            m_pendingTaskCall.value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("(cancelled)");
        m_messages.append(toolResult);
        m_pendingTaskCall = QJsonObject();
    }
    m_pendingToolCalls = QJsonArray();
    m_toolResultsReady = QJsonArray();
}

// SubAgent（友元）复用的内部工具函数静态转发
const QString &AgentLoop::askPrefixOf() { return askPrefix(); }
QString AgentLoop::toolSummaryOf(const QString &toolName, const QJsonObject &args)
{
    return toolSummary(toolName, args);
}
const QStringList &AgentLoop::dangerousCommandList() { return dangerousCommands(); }

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
    // 钩子（文案逐字不变）。permissionGranted=true（批准后续跑）只跳过询问规则；
    // deny 列表检查始终执行——拒绝先于用户意志，且防御队列重放/迟到的续跑路径
    m_preToolUseHooks.append([this](const QJsonObject &toolCall, bool permissionGranted) -> QString {
        const QString toolName = callToolName(toolCall);
        const QJsonObject args = callToolArgs(toolCall);

        if (toolName == QStringLiteral("bash"))
        {
            const QString blocked = checkDenyList(args.value(QStringLiteral("command")).toString());
            if (!blocked.isEmpty())
                return blocked; // 硬拒绝：直接作为拦截文本回填
        }

        if (permissionGranted)
            return QString(); // 已批准：跳过询问，日志等其余钩子照常执行

        const QString reason = checkPermissionRules(m_workDir, toolName, args);
        if (!reason.isEmpty())
            return askPrefix() + reason; // 需询问：异步协议，由 executeTool 挂起队列

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

    // 120 秒超时：kill 后 finished 信号触发，靠标志区分“超时被杀” vs “正常结束”。
    // shared_ptr 捕获（MINOR-2 修复）：若进程从未启动/不发 finished，超时闭包与
    // 标志随最后一个捕获者释放，不再裸 new/delete 泄漏
    auto timedOut = std::make_shared<bool>(false);
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
        process->deleteLater();

        // PostToolUse 钩子（lcc s04）：bash handler 产出后、回填前触发
        triggerPostToolUseHooks(toolCall, output);

        onToolFinished(toolCall, QStringLiteral("bash"), command, output);
    });

    process->start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
}

QString AgentLoop::safePathIn(const QString &workDir, const QString &p, QString *error)
{
    // 相对路径按工作区解析，绝对路径直接使用；cleanPath 归一化 "../" 与分隔符（Windows 反斜杠转正斜杠）
    const QString joined = QDir::isAbsolutePath(p)
        ? QDir::cleanPath(p)
        : QDir::cleanPath(workDir + QLatin1Char('/') + p);

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
    const QString root = QDir::cleanPath(workDir);
    if (absPath.compare(root, Qt::CaseInsensitive) != 0
        && !absPath.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
    {
        if (error)
            *error = QStringLiteral("Error: Path escapes workspace: %1").arg(p);
        return QString();
    }
    return absPath;
}

QString AgentLoop::runReadFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
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

QString AgentLoop::runWriteFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    const QString content = args.value(QStringLiteral("content")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
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

QString AgentLoop::runEditFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    const QString oldText = args.value(QStringLiteral("old_text")).toString();
    const QString newText = args.value(QStringLiteral("new_text")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
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

QString AgentLoop::runGlobIn(const QString &workDir, const QJsonObject &args)
{
    // 统一分隔符风格（模型可能给出反斜杠模式）
    QString pattern = args.value(QStringLiteral("pattern")).toString();
    pattern.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QRegularExpression re = globToRegex(pattern);
    if (!re.isValid())
        return QStringLiteral("Error:%1").arg(re.errorString());

    // 以工作区为根递归遍历，按相对路径匹配；结果过滤 safePathIn 逃逸项（如符号链接指向外部）
    QStringList collected;
    QSet<QString> seen;
    QDirIterator it(workDir, QDir::AllEntries | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString rel = QDir(workDir).relativeFilePath(it.next());
        if (!re.match(rel).hasMatch())
            continue;
        QString err;
        if (safePathIn(workDir, rel, &err).isEmpty())
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

// ---- 技能（lcc s07 SkillManager 内联移植：scan / catalog / load 三语义）----

void AgentLoop::scanSkills()
{
    // 对应 lcc scan_skills()：整表重建；skills 目录缺失静默为空（对应 python return）
    m_skills.clear();

    const QString skillsDir = QDir(m_workDir).filePath(QStringLiteral("skills")); // lcc env.py: workDir/"skills"
    if (!QFileInfo(skillsDir).isDir())
        return;

    // 越界防护根：解析后的清单必须仍位于 skills 目录内（lcc resolve().is_relative_to(root) 等价；
    // canonicalFilePath 已归一化，root 为空表示目录不可解析）
    const QString root = QFileInfo(skillsDir).canonicalFilePath();
    if (root.isEmpty())
        return;

    // 对应 sorted(glob("*/SKILL.md"))：按目录名升序遍历。微小偏差：python glob 跳过 "." 开头的
    // 隐藏目录，QDir::entryList 会包含——lite 有意超集，不作特判
    QStringList dirs = QDir(skillsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    dirs.sort(); // 码点升序 ≈ python sorted()（Windows 下 nt 文件系统 python 以 casefold 为键，差异极微）

    // 空白切分正则（对应 python str.split() 的连续空白切分）
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));

    for (const QString &dirName : dirs)
    {
        const QString manifestPath = QDir(skillsDir).filePath(dirName + QStringLiteral("/SKILL.md"));
        const QFileInfo manifestInfo(manifestPath);
        if (!manifestInfo.isFile()) // 对应 not is_file() → continue（QFileInfo::isFile 跟随符号链接，语义一致）
            continue;
        const QString resolved = manifestInfo.canonicalFilePath(); // 对应 resolve()
        if (resolved.isEmpty() || (resolved != root && !resolved.startsWith(root + QLatin1Char('/'))))
            continue; // 逃逸出 skills 根（符号链接指向外部等）→ 跳过

        QFile file(resolved);
        if (!file.open(QIODevice::ReadOnly))
            continue; // 偏差：lcc 单文件读失败会抛异常中止整次扫描；lite 跳过该条继续（更稳健的有意超集）
        const QString content = QString::fromUtf8(file.readAll()); // 对应 read_text(encoding="utf-8")

        // ---- frontmatter 定位（lcc parse_frontmatter 的 splitlines(keepends) 偏移量移植）----
        // 行终止符为 \n 或 \r，\r\n 视作一个；"行内容"截到首个终止符，即等价 rstrip("\r\n") 后的比较
        auto lineEnd = [&content](int from) {
            int i = from;
            while (i < content.size() && content.at(i) != QLatin1Char('\n') && content.at(i) != QLatin1Char('\r'))
                ++i;
            return i;
        };
        auto skipEol = [&content](int end) {
            if (end < content.size() && content.at(end) == QLatin1Char('\r')
                && end + 1 < content.size() && content.at(end + 1) == QLatin1Char('\n'))
                return 2; // \r\n
            return end < content.size() ? 1 : 0; // 单个 \n / \r / 文件尾
        };

        QString metaName;
        QString metaDesc;
        QString body;
        const int firstEnd = lineEnd(0);
        const bool hasFrontmatter =
            !content.isEmpty() && content.left(firstEnd) == QLatin1String("---"); // 首行 rstrip 后恰为 "---"
        int closingStart = -1; // 第二条 "---" 行起点（-1 = 不存在闭合行 → 整体视为正文）
        int closingEnd = -1;   // 其行尾（不含行终止符）
        if (hasFrontmatter)
        {
            int pos = firstEnd + skipEol(firstEnd);
            while (pos < content.size())
            {
                const int end = lineEnd(pos);
                if (content.mid(pos, end - pos) == QLatin1String("---"))
                {
                    closingStart = pos;
                    closingEnd = end;
                    break;
                }
                pos = end + skipEol(end);
            }
        }

        if (hasFrontmatter && closingStart >= 0)
        {
            const int metaStart = firstEnd + skipEol(firstEnd);
            const QString metaRegion = content.mid(metaStart, closingStart - metaStart);
            body = content.mid(closingEnd + skipEol(closingEnd)).trimmed(); // 对应 remainder.strip()

            // ---- 极简 YAML 解析（登记偏差：lite 无 PyYAML 且禁止引入第三方库）----
            // 仅识别顶格单行 `name:` / `description:` 平面标量（冒号后须有空格/制表或行尾，
            // 值可选去除外层成对引号后 trim）；其余键、缩进、多行结构一律忽略，
            // 效果 ≈ lcc yaml.safe_load 失败/非 dict 时回落 {} 走默认值的分支
            auto stripQuotes = [](QString v) {
                v = v.trimmed(); // 对应 str(...).strip()
                if (v.size() >= 2
                    && ((v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                        || (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))))
                    v = v.mid(1, v.size() - 2).trimmed(); // 偏差：不处理 YAML 转义，仅去外层成对引号
                return v;
            };
            const QStringList metaLines = metaRegion.split(QLatin1Char('\n'));
            for (QString rawLine : metaLines)
            {
                while (rawLine.endsWith(QLatin1Char('\r')))
                    rawLine.chop(1); // splitlines 语义：剥去 \r\n 残留
                QString *target = nullptr;
                int valueStart = 0;
                if (rawLine.startsWith(QLatin1String("name:")))
                {
                    target = &metaName;
                    valueStart = 5; // strlen("name:")
                }
                else if (rawLine.startsWith(QLatin1String("description:")))
                {
                    target = &metaDesc;
                    valueStart = 12; // strlen("description:")
                }
                else
                {
                    continue;
                }
                if (rawLine.size() > valueStart && rawLine.at(valueStart) != QLatin1Char(' ')
                    && rawLine.at(valueStart) != QLatin1Char('\t'))
                    continue; // "name:x" 非 YAML 平面标量映射键 → 整行忽略
                *target = stripQuotes(rawLine.mid(valueStart)); // 重复键后者覆盖前者（≈ PyYAML last-wins）
            }
        }
        else
        {
            // 首行/闭合行任一缺失 → 无 frontmatter：meta 为空、正文取原文
            // （对应 lcc return {}, text —— 此路径 lcc 不 strip，逐字保留）
            body = content;
        }

        // name 缺省 = 技能目录名（对应 manifest.parent.name）
        const QString name = metaName.isEmpty() ? dirName : metaName;
        // description 缺省 = 正文首行（对应 body.split("\n", 1)[0]；空正文 → 空串行，语义一致）
        const QString rawDesc =
            metaDesc.isEmpty() ? body.split(QLatin1Char('\n'), Qt::KeepEmptyParts).first() : metaDesc;
        // 清洗（对应 " ".join(str(desc).lstrip("# ").split())）：剥离开头的 '#'/' ' 字符，
        // 再按连续空白切分、以单个空格重连
        int lead = 0;
        while (lead < rawDesc.size() && (rawDesc.at(lead) == QLatin1Char('#') || rawDesc.at(lead) == QLatin1Char(' ')))
            ++lead;
        const QString description =
            rawDesc.mid(lead).split(wsRe, Qt::SkipEmptyParts).join(QLatin1Char(' '));

        // content 保存整份文件原文（含 frontmatter，对应 "content": text）
        // 对应 python dict 赋值语义：同名后扫覆盖值但保留原插入位置（catalog 依首次出现顺序）
        bool replaced = false;
        for (Skill &existing : m_skills)
        {
            if (existing.name == name)
            {
                existing = Skill{name, description, content};
                replaced = true;
                break;
            }
        }
        if (!replaced)
            m_skills.append(Skill{name, description, content});
    }
}

QString AgentLoop::skillsCatalog() const
{
    // 对应 lcc catalog()：空 → "(no skills found)"；否则逐行 "- {name}: {description}" 以 \n 连接
    if (m_skills.isEmpty())
        return QStringLiteral("(no skills found)");
    QStringList lines;
    lines.reserve(m_skills.size());
    for (const Skill &skill : m_skills)
        lines.append(QStringLiteral("- %1: %2").arg(skill.name, skill.description));
    return lines.join(QLatin1Char('\n'));
}

QString AgentLoop::runLoadSkill(const QJsonObject &args) const
{
    // 对应 lcc run_load_skill → skill_manager.load(name)：命中返回整份原文，未命中返回错误文本
    // 偏差：lcc 缺 name 参数直接 TypeError 崩溃；此处回落空串返回 Unknown 错误
    // （与 todo_write 缺参的 Graceful 处理同风格）
    const QString name = args.value(QStringLiteral("name")).toString();
    for (const Skill &skill : m_skills)
    {
        if (skill.name == name)
            return skill.content;
    }
    return QStringLiteral("Error: Unknown skill '%1'").arg(name);
}

QString AgentLoop::renderTodos(const QVector<TodoItem> &items)
{
    // lcc TodoManager.render 等价：空清单 "No todos"；标记 [ ]/[>]；[x] 对应
    // pending/in_progress/completed；lcc 原文完成度统计元素以换行开头，
    // '\n' join 后在清单与统计之间形成一空行
    if (items.isEmpty())
        return QStringLiteral("No todos");

    QStringList lines;
    for (const TodoItem &item : items)
    {
        QString marker = QStringLiteral("[ ]");
        if (item.status == QStringLiteral("in_progress"))
            marker = QStringLiteral("[>]");
        else if (item.status == QStringLiteral("completed"))
            marker = QStringLiteral("[x]");
        lines.append(QStringLiteral("%1 %2").arg(marker, item.content));
    }

    int done = 0;
    for (const TodoItem &item : items)
    {
        if (item.status == QStringLiteral("completed"))
            ++done;
    }
    lines.append(QStringLiteral("\n(%1/%2 completed)").arg(done).arg(items.size()));

    return lines.join(QLatin1Char('\n'));
}

QString AgentLoop::runTodoWrite(const QJsonObject &args)
{
    // lcc s06 update_todos 等价：无状态——只校验并渲染本次入参，不落任何持久清单
    // （lcc s05 的 TodoManager.items 持久化与 'Current Tasks' 控制台面板在 s06 移除）；
    // 任一校验失败返回 "Error:..."（lcc 抛 ValueError、run_todo_write 捕获转 "Error:{e}"）
    QJsonValue todosValue = args.value(QStringLiteral("todos"));

    // 模型偶发把 todos 传成 JSON 字符串（对应 lcc json.loads/ast.literal_eval 兼容
    // 路径；lcc 的 literal_eval 无 Qt 等价，单引号 Python 字面量解析不了）。
    // todos 缺失时 lcc 直接 TypeError 崩溃，此处按"非列表"优雅拒绝（有意偏差）
    if (todosValue.isString())
    {
        QJsonParseError parseError;
        const QJsonDocument parsed =
            QJsonDocument::fromJson(todosValue.toString().toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
            return QStringLiteral("Error:todos must be a list or JSON array string");
        if (!parsed.isArray())
            return QStringLiteral("Error:todos must be a list");
        todosValue = QJsonValue(parsed.array());
    }

    if (!todosValue.isArray())
        return QStringLiteral("Error:todos must be a list");

    const QJsonArray todos = todosValue.toArray();
    if (todos.size() > 20)
        return QStringLiteral("Error:Max 20 todos allowed");

    QVector<TodoItem> validated;
    int inProgressCount = 0;
    for (int index = 0; index < todos.size(); ++index)
    {
        if (!todos.at(index).isObject())
            return QStringLiteral("Error:todos[%1] must be an object").arg(index);

        const QJsonObject todo = todos.at(index).toObject();
        // lcc str(todo.get("content","")).strip() / str(todo.get("status","pending")).lower()：
        // 非字符串值经 QVariant 转文本；status 仅在键缺失时取默认 pending（空串键值照旧报错）
        const QString content =
            todo.value(QStringLiteral("content")).toVariant().toString().trimmed();
        QString status = QStringLiteral("pending");
        if (todo.contains(QStringLiteral("status")))
            status = todo.value(QStringLiteral("status")).toVariant().toString().toLower();

        if (content.isEmpty())
            return QStringLiteral("Error:todos[%1] requires content").arg(index);
        if (status != QStringLiteral("pending") && status != QStringLiteral("in_progress")
            && status != QStringLiteral("completed"))
        {
            return QStringLiteral("Error:todos[%1] has invalid status '%2'")
                .arg(index)
                .arg(status);
        }
        if (status == QStringLiteral("in_progress"))
            ++inProgressCount;

        validated.append(TodoItem{ content, status });
    }

    if (inProgressCount > 1)
        return QStringLiteral("Error:Only one todo can be in_progress at a time");

    // lcc s06：渲染本次入参并直接返回（无持久化、无 s05 的 'Current Tasks' 控制台面板）
    const QString output = renderTodos(validated);

    // 跨车道契约：每次校验通过（含清为空清单）后广播本次输入清单快照 [{content, status}, ...]，
    // 供 TodoCard 渲染（无状态下由模型逐轮重发全量清单维持面板内容）
    QJsonArray snapshot;
    for (const TodoItem &item : validated)
    {
        QJsonObject itemObj;
        itemObj[QStringLiteral("content")] = item.content;
        itemObj[QStringLiteral("status")] = item.status;
        snapshot.append(itemObj);
    }
    emit todoUpdated(snapshot);

    return output;
}

void AgentLoop::stop()
{
    if (!m_running)
        return;

    // lcc s06 R1：task 子代理在跑则先级联取消并合成 "(cancelled)" 配对回填。
    // 串行队列下"宿主待裁决"与"子代理运行中"互斥，随后的待决权限分支自然空转
    cancelSubAgent();

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

    // todo_write（lcc s05）：todos 为嵌套 array<object{content,status}> schema，
    // makeTool lambda 只支持平铺 string/integer 参数，按 lcc 原文手工构造；
    // 外层包装与 makeTool 产物一致（type:function + function.parameters）
    {
        QJsonObject contentSchema;
        contentSchema[QStringLiteral("type")] = QStringLiteral("string");
        contentSchema[QStringLiteral("minLength")] = 1;

        QJsonObject statusSchema;
        statusSchema[QStringLiteral("type")] = QStringLiteral("string");
        statusSchema[QStringLiteral("enum")] = QJsonArray::fromStringList(
            { QStringLiteral("pending"), QStringLiteral("in_progress"),
              QStringLiteral("completed") });

        QJsonObject itemProperties;
        itemProperties[QStringLiteral("content")] = contentSchema;
        itemProperties[QStringLiteral("status")] = statusSchema;

        QJsonObject items;
        items[QStringLiteral("type")] = QStringLiteral("object");
        items[QStringLiteral("properties")] = itemProperties;

        QJsonObject todosSchema;
        todosSchema[QStringLiteral("type")] = QStringLiteral("array");
        todosSchema[QStringLiteral("maxItems")] = 20;
        todosSchema[QStringLiteral("items")] = items;

        QJsonObject properties;
        properties[QStringLiteral("todos")] = todosSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = properties;
        inputSchema[QStringLiteral("required")] =
            QJsonArray::fromStringList({ QStringLiteral("todos") });

        QJsonObject function;
        function[QStringLiteral("name")] = QStringLiteral("todo_write");
        function[QStringLiteral("description")] =
            QStringLiteral("Create and manage a task list for your current coding session.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // task（lcc s06）：prompt 带 minLength 约束，makeTool lambda 不支持该字段，
    // 与 todo_write 同款手工构造；外层包装一致
    {
        QJsonObject promptSchema;
        promptSchema[QStringLiteral("type")] = QStringLiteral("string");
        promptSchema[QStringLiteral("minLength")] = 1;

        QJsonObject properties;
        properties[QStringLiteral("prompt")] = promptSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = properties;
        inputSchema[QStringLiteral("required")] =
            QJsonArray::fromStringList({ QStringLiteral("prompt") });

        QJsonObject function;
        function[QStringLiteral("name")] = QStringLiteral("task");
        function[QStringLiteral("description")] =
            QStringLiteral("Run a subagent with fresh conversation context and return its final text.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // load_skill（lcc s07 第 8 个工具）：schema 无 minLength 等附加约束，makeTool lambda 即可表达；
    // 描述与 required 逐字对齐 lcc LOAD_SKILL 定义
    tools.append(makeTool(QStringLiteral("load_skill"),
                          QStringLiteral("Load the full SKILL.md content by skill name."),
                          { { QStringLiteral("name"), QStringLiteral("string") } },
                          { QStringLiteral("name") }));

    return tools;
}