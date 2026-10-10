#include "SubAgent.h"

#include "QOpenAi.h"
#include "ToolNames.h" // 工具名集中常量（lcc a6d29b9 tool_names.py 移植）
#include "AgentLoopInternal.h"
#include "AgentConstants.h" // 模型清单/max_tokens 单源（bash 超时与截断已随执行链收敛到 BashRunner）
#include "BashRunner.h"     // bash 执行链（建进程/超时/截断/黑名单文案）与主循环单源共用
#include "BackgroundTasksManager.h" // formatBashResult：非零退出码前缀，与主循环前台/后台分支同口径

#include <QDebug>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QSignalBlocker>

#include <memory>
#include <utility>

namespace {

// 子代理轮次预算：每次发起请求消耗一轮（lcc 9165f8f 后子代理不再触发 Stop，见最终回答分支）。
// 用户裁决改与主循环一致：上限取 maxToolIterationsValue()（settings.ini 可设置，默认 500），
// 于 start() 入口快照进 m_maxTurns，单次运行中不随设置变动（同主循环回合入口快照纪律）。

// 子代理工具白名单（lcc s06 subTools）：主循环 7 工具定义中的前 5 个，
// 定义逐字共享（todo_write/task 不进子表，模型幻觉调用也只落 Unknown 回填）。
// 注：全量定义由成员函数取得后传入（createToolsDefinition 为 AgentLoop 私有静态，
// friend 权限仅覆盖 SubAgent 成员，不覆盖本自由函数）
QJsonArray filterSubTools(const QJsonArray &all)
{
    static const QSet<QString> allowed = {
        ToolNames::BASH, ToolNames::READ_FILE, ToolNames::WRITE_FILE,
        ToolNames::EDIT_FILE, ToolNames::GLOB,
    };

    QJsonArray sub;
    for (const QJsonValue &value : all)
    {
        const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!allowed.contains(name))
            continue;

        if (name == ToolNames::BASH)
        {
            // 子代理 bash 专用（lcc s11 95242de sub_bash_info 等价）：从 schema  properties
            // 剔除 run_in_background——schema 层禁止后台（执行层双保险见 SubAgent::executeTool）。
            // QJsonObject 隐式共享，逐层拷贝修改即深拷贝语义（lcc deepcopy 的 lite 等价）
            QJsonObject tool = value.toObject();
            QJsonObject fn = tool.value(QStringLiteral("function")).toObject();
            QJsonObject params = fn.value(QStringLiteral("parameters")).toObject();
            QJsonObject props = params.value(QStringLiteral("properties")).toObject();
            props.remove(QStringLiteral("run_in_background"));
            params[QStringLiteral("properties")] = props;
            fn[QStringLiteral("parameters")] = params;
            tool[QStringLiteral("function")] = fn;
            sub.append(tool);
            continue;
        }

        sub.append(value);
    }
    return sub;
}

} // namespace

SubAgent::SubAgent(AgentLoop *host, QObject *parent, const QString &workDir,
                   const QString &model, const QString &prompt)
    : QObject(parent)
    , m_host(host)
    , m_workDir(workDir)
    , m_model(model)
    , m_prompt(prompt)
    , m_handlers(AgentLoop::baseFileToolHandlers(workDir))
{
}

QString SubAgent::subSystemPrompt(const QString &workDir)
{
    // lcc s06 原文为两段相邻字面量拼接（句间无空格），按规格裁决规整为正常空格——
    // 此处为与 lcc 文本的有意偏差之一（主循环 prompt 保留无空格quirk，见 AgentLoop.cpp）
    return QStringLiteral("You are a coding agent at %1. Complete the given task, then return a concise final answer.")
        .arg(workDir);
}

void SubAgent::start(CompleteHandler onComplete)
{
    m_onComplete = std::move(onComplete);
    // 轮次预算入口快照：与主循环 maxToolIterations 同源（用户裁决一致化）
    m_maxTurns = AgentConst::maxToolIterationsValue();

    // 独立上下文：仅 system（子代理 prompt）+ user（任务描述），不携带主循环任何历史
    //（lcc run_subagent 的 messages=[{role:user,...}] + system= 参数，OpenAI 协议下
    //  system 作为首条消息发送，与主循环同一形态）
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = subSystemPrompt(m_workDir);
    m_messages.append(systemMessage);

    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    userMessage[QStringLiteral("content")] = m_prompt;
    m_messages.append(userMessage);

    startChatRequest();
}

void SubAgent::startChatRequest()
{
    if (m_cancelled || m_settled)
        return;

    // 轮次预算：入口快照 m_maxTurns（与主循环同源）内未产出最终答案则以停跑文案收尾
    if (m_turns >= m_maxTurns)
    {
        finish(QStringLiteral("Subagent stopped after %1 turns without a final answer.")
                   .arg(m_maxTurns));
        return;
    }
    ++m_turns;

    QJsonObject request;
    request[QStringLiteral("model")] = m_model;
    request[QStringLiteral("messages")] = AgentLoopDetail::snapshotMessages(m_messages);
    // 成员函数内调用私有静态（friend 生效），过滤交给自由函数
    request[QStringLiteral("tools")] = filterSubTools(AgentLoop::createToolsDefinition());
    // lcc s06：显式输出上限（与主循环同取 AgentConst::kMaxTokens）；子代理不开 enable_thinking（黑盒无思考展示）
    request[QStringLiteral("max_tokens")] = AgentConst::kMaxTokens;

    QOpenAi::ChatStream *s = QOpenAi::chat().createStream(request, this);
    m_currentStream = s;

    // 黑盒：有意不连接 thinkingDelta/textDelta——子代理过程不进入 UI

    connect(s, &QOpenAi::ChatStream::messageFinished, this, [this, s](const QJsonObject &fullMsg) {
        m_currentStream = nullptr;
        s->deleteLater();
        if (m_cancelled)
            return;

        const QJsonArray toolCalls = fullMsg.value(QStringLiteral("tool_calls")).toArray();
        if (toolCalls.isEmpty())
        {
            m_messages.append(fullMsg);

            // Stop 钩子不在子代理触发（lcc 9165f8f）：Stop 只在主循环最终回答处触发一次，
            // 子代理黑盒终点即汇总；轮次上限收尾保留（startChatRequest 顶部检查）

            // extract_text 等价：OpenAI 形态下 content 即纯文本；空内容按 lcc 兜底文案
            const QString text = fullMsg.value(QStringLiteral("content")).toString();
            finish(text.isEmpty() ? QStringLiteral("(no summary)") : text);
            return;
        }

        continueWithToolResults(fullMsg);
    });

    connect(s, &QOpenAi::ChatStream::error, this, [this, s](const QString &msg) {
        m_currentStream = nullptr;
        s->deleteLater();
        if (m_cancelled)
            return;
        // API 失败以字符串结果回给主模型（lcc except → return "Error: ..."），
        // 不触发主循环 error 信号、不中断主循环
        finish(QStringLiteral("Error: subagent API call failed: %1").arg(msg));
    });
}

void SubAgent::continueWithToolResults(const QJsonObject &assistantMessage)
{
    m_messages.append(assistantMessage);
    m_pendingToolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    m_toolResultsReady = QJsonArray();

    runNextTool();
}

void SubAgent::runNextTool()
{
    if (m_cancelled || m_settled)
        return;

    if (m_pendingToolCalls.isEmpty())
    {
        // 本批工具全部完成：回填结果并再次请求（无 todo 提醒逻辑——lcc 子代理循环没有）
        for (const QJsonValue &value : m_toolResultsReady)
            m_messages.append(value.toObject());
        m_toolResultsReady = QJsonArray();

        startChatRequest();
        return;
    }

    const QJsonObject toolCall = m_pendingToolCalls.takeAt(0).toObject();
    executeTool(toolCall, /*permissionGranted = */ false);
}

void SubAgent::executeTool(const QJsonObject &toolCall, bool permissionGranted)
{
    // 解析工具名与参数：与主循环共用 AgentLoopDetail::parseToolCall（错误文案同源），非法
    // arguments 直接以错误文本回填模型；空/纯空白参数视作空对象。
    const AgentLoopDetail::ToolCallView call = AgentLoopDetail::parseToolCall(toolCall);
    const QString toolName = call.name;
    const QJsonObject args = call.args;
    if (!call.ok())
    {
        onToolFinished(toolCall, call.errorText);
        return;
    }
    const QString summary = AgentLoopDetail::toolSummary(toolName, args);

    // PreToolUse 钩子链（共用宿主注册表，含 s03 权限门与日志钩子）。返回协议与主循环一致：
    // 1) "ASK:" 前缀 → 需询问：暂停子队列（宿主仍在等 task 回调，整条链冻结），
    //    经自身 permissionRequired → 宿主转发为 UI 既有 3 参信号
    // 2) 其余非空 → 硬拒绝：直接回填为 tool_result（不触发 PostToolUse，对齐 lcc）
    const QString gate = m_host->triggerPreToolUseHooks(toolCall, permissionGranted);
    if (!gate.isEmpty())
    {
        if (gate.startsWith(AgentLoopDetail::askPrefix()))
        {
            m_awaitingPermission = true;
            m_pendingPermissionCall = toolCall;
            emit permissionRequired(toolName, summary,
                                    gate.mid(AgentLoopDetail::askPrefix().size()));
            return; // 队列暂停：不回填、不请求，等宿主把裁决路由进 resolvePermission()
        }
        onToolFinished(toolCall, gate);
        return;
    }

    // bash 走子代理自身的异步进程链（收口点在进程 finished 回调，簿记独立于宿主）
    if (toolName == ToolNames::BASH)
    {
        // 子代理禁后台·执行层双保险（lcc s11 95242de allow_background=False 等价）：
        // schema 已删 run_in_background（见 filterSubTools），模型若仍幻觉带参，丢弃后
        // 永远前台执行；降级提示仅打控制台、不进模型可见输出（lcc log_warn 语义）
        QJsonObject bashArgs = args;
        if (bashArgs.take(QStringLiteral("run_in_background")).toBool())
                qWarning() << "[bg] not allowed in this context, running in foreground";
        executeBashAsync(toolCall, bashArgs);
        return;
    }

    // 其余工具经 handler 表同步路由；未知名称不中断循环，错误文本作为结果回填
    //（文案沿用主循环 "Unknown tool: %1"，lcc 原文 "Unknown:" 的有意偏差在 s02 已确立）
    auto it = m_handlers.constFind(toolName);
    const QString output = it == m_handlers.constEnd()
        ? QStringLiteral("Unknown tool: %1").arg(toolName)
        : it.value()(args);

    // PostToolUse 钩子（共用宿主注册表）：子代理内部工具同样进入日志钩子
    m_host->triggerPostToolUseHooks(toolCall, output);

    onToolFinished(toolCall, output);
}

void SubAgent::onToolFinished(const QJsonObject &toolCall, const QString &output)
{
    if (m_cancelled || m_settled)
        return;

    // 实时进度透传（六路收口的唯一汇流点，取消卫兵之后）：宿主直连转发 subagentProgress
    // 驱动 UI task 卡进度行。名称/摘要从 toolCall 就地重算（arguments 是小 JSON 串，重算
    // 开销可忽略，免为进度信号扩六处调用签名）；参数非法路径摘要留空。
    // turnNo=m_turns：工具由第 N 次请求产出，下一次 ++ 在全队收口后，恒为所属轮
    {
        const AgentLoopDetail::ToolCallView call = AgentLoopDetail::parseToolCall(toolCall);
        const QString summary = call.ok()
            ? AgentLoopDetail::toolSummary(call.name, call.args)
            : QString();
        emit progressEmitted(m_turns, call.name, summary);
    }

    // 黑盒收口：只回填 tool 结果，不发 toolOutputReady——task 对外可见性由宿主收口一次
    QJsonObject toolResult = AgentLoopDetail::makeToolResult(
        toolCall.value(QStringLiteral("id")).toString(), output);
    m_toolResultsReady.append(toolResult);

    runNextTool();
}

void SubAgent::resolvePermission(bool allow)
{
    // 宿主 resolvePermission 在无自身待决时路由至此；无待决时忽略
    if (!m_awaitingPermission || m_cancelled)
        return;

    const QJsonObject toolCall = m_pendingPermissionCall;
    m_pendingPermissionCall = QJsonObject();
    m_awaitingPermission = false;
    if (m_settled)
        return;

    if (!allow)
    {
        // 拒绝：回填 "Permission denied"（与主循环 s03 文案逐字一致）并续跑子队列；
        // 不触发 PostToolUse（handler 未运行，对齐 lcc 拒绝即 continue）
        onToolFinished(toolCall, QStringLiteral("Permission denied"));
        return;
    }

    // 允许：重走执行链（permissionGranted=true 使权限钩子仅跳过询问规则，
    // deny 列表与 bash 内部黑名单仍照常检查——MINOR-3 双层防御）
    executeTool(toolCall, /*permissionGranted = */ true);
}

void SubAgent::executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args)
{
    const QString command = args.value(QStringLiteral("command")).toString();

    // bash 内部危险黑名单（lcc fddb23e G4 单源：与权限门 DENY_LIST 共用 AgentLoopDetail::bashDenyList；
    // 清单与文案与主循环逐字一致，判定/文案实现亦单源于 BashRunner::dangerWarning）
    const QString danger = BashRunner::dangerWarning(command, AgentLoopDetail::bashDenyList());
    if (!danger.isEmpty())
    {
        m_host->triggerPostToolUseHooks(toolCall, danger);
        onToolFinished(toolCall, danger);
        return;
    }

    // 超时标志（shared_ptr 随回调捕获，无裸 new/delete——MINOR-2）；进程创建/登记/
    // 挂超时/PowerShell 启动与主循环共用 BashRunner::start（原整函数级复制收敛），
    // "先 connect 后 start"时序由 arm 回调保证。失败可判定性与主循环前台 bash 同款两道防线：
    // errorOccurred 显式收口启动失败、非零退出码经 formatBashResult 前缀化——缺任一道都会把
    // 失败洗白成"成功"回喂模型（裸输出或 (no output)），违背 B1「工具侧一切失败折叠为可判定
    // 错误文本」约定。差异仅两处留在下方回调：m_cancelled 短路（取消后静默丢弃输出）与
    // 黑盒收口（onToolFinished 不带工具名参数）
    auto timedOut = std::make_shared<bool>(false);
    auto handled = std::make_shared<bool>(false);
    BashRunner::start(command, m_workDir, this, &m_activeProcesses, timedOut,
                      [this, toolCall, timedOut, handled](QProcess *process) {
        connect(process, &QProcess::errorOccurred, this,
                [this, process, toolCall, handled](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || *handled)
                return;
            *handled = true;
            const QString output = QStringLiteral("Error: bash 启动失败：powershell.exe 无法启动（%1）")
                                       .arg(process->errorString());
            // PostToolUse 钩子与正常分支同时序（handler 产出后、回填前）
            m_host->triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, output);
        });

        connect(process, &QProcess::finished, this,
                [this, process, toolCall, timedOut, handled](int exitCode, QProcess::ExitStatus) {
            m_activeProcesses.removeAll(process);

            // FailedToStart 已由 errorOccurred 显式收口：不再二次 onToolFinished，只销毁进程
            if (*handled)
            {
                process->deleteLater();
                return;
            }
            *handled = true;

            if (m_cancelled)
            {
                process->deleteLater();
                return;
            }

            // finalizeOutput：超时→Timeout 文案（不读缓冲）；否则截断+空兜底（与主循环同口径）
            const QString base = BashRunner::finalizeOutput(process, *timedOut);
            // 非零退出码前缀 "Error: command exited with status N:"（与主循环前台/后台分支单源）
            const QString output =
                *timedOut ? base
                          : BackgroundTasksManager::formatBashResult(base, exitCode, false);
            process->deleteLater();

            m_host->triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, output);
        });
    });
}

void SubAgent::finish(const QString &result)
{
    // 完结仅一次；cancel() 之后不再触发（收口配对由宿主 cancelSubAgent() 负责）
    if (m_settled || m_cancelled)
        return;
    m_settled = true;

    const CompleteHandler handler = std::exchange(m_onComplete, nullptr);
    if (handler)
        handler(result);
}

void SubAgent::cancel()
{
    // 幂等：断流、kill 子进程、清队列、抑制回调（宿主随后为 task 合成 "(cancelled)"）
    if (m_cancelled)
        return;
    m_cancelled = true;
    m_settled = true;
    m_onComplete = nullptr;

    QSignalBlocker blocker(this); // 阻断尚未处理的转发链

    if (m_currentStream)
    {
        m_currentStream->cancel();
        m_currentStream->disconnect();
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }

    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill(); // kill 触发的 finished 回调经 m_cancelled 早退
    }
    m_activeProcesses.clear();

    m_pendingToolCalls = QJsonArray();
    m_toolResultsReady = QJsonArray();
    m_pendingPermissionCall = QJsonObject();
    m_awaitingPermission = false;
}
