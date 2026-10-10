// 工具分发：executeTool 前置钩子链 → bash/task 异步特判 → handler 表同步路由 → onToolFinished 统一收口。
// handler 表（主循环 / 基础文件工具）与供 SubAgent、ChatSessionPage 友元复用的静态转发同处本文件。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "SubAgent.h"
#include "ToolNames.h"
#include "BashRunner.h"

#include <QJsonDocument>

// --- 跨编译单元共享的内部工具（声明见 AgentLoopInternal.h，定义归属见该头注释）---
namespace AgentLoopDetail
{
// 工具调用的关键参数摘要（lcc s02 tool_use info：bash→command、glob→pattern、文件工具→path；
// s05 起 todo_write 为固定文案；s06 起 task→prompt；s07 起 load_skill→name，对齐 lcc 日志语义）
QString toolSummary(const QString &toolName, const QJsonObject &args)
{
    if (toolName == ToolNames::BASH)
        return args.value(QStringLiteral("command")).toString();
    if (toolName == ToolNames::GLOB)
        return args.value(QStringLiteral("pattern")).toString();
    if (toolName == ToolNames::READ_FILE || toolName == ToolNames::WRITE_FILE
        || toolName == ToolNames::EDIT_FILE)
        return args.value(QStringLiteral("path")).toString();
    if (toolName == ToolNames::TODO_WRITE)
        return QStringLiteral("update task list"); // lcc s06 原文（hooks.py）已去掉末尾冒号
    if (toolName == ToolNames::TASK)
        return args.value(QStringLiteral("prompt")).toString();
    if (toolName == ToolNames::LOAD_SKILL)
        return args.value(QStringLiteral("name")).toString(); // lcc s07：摘要取技能名
    // lcc s10 任务图：create_task→subject；list_tasks→自拟固定短文本（仿 todo_write 风格，登记偏差）；
    // update/get/claim/complete_task→task_id（toolUseInfo 不加对应分支——lcc s10 hooks.py 字节不变）
    if (toolName == ToolNames::CREATE_TASK)
        return args.value(QStringLiteral("subject")).toString();
    if (toolName == ToolNames::LIST_TASKS)
        return QStringLiteral("task list");
    if (toolName == ToolNames::UPDATE_TASK || toolName == ToolNames::GET_TASK
        || toolName == ToolNames::CLAIM_TASK || toolName == ToolNames::COMPLETE_TASK)
        return args.value(QStringLiteral("task_id")).toString();
    // lcc s12 定时任务：摘要取 cron 表达式/固定短文本/job_id（仿 s10 风格，lcc 无对应钩子文案）
    if (toolName == ToolNames::SCHEDULE_CRON)
        return QStringLiteral("schedule cron ") + args.value(QStringLiteral("cron")).toString();
    if (toolName == ToolNames::LIST_CRONS)
        return QStringLiteral("cron list");
    if (toolName == ToolNames::CANCEL_CRON)
        return QStringLiteral("cancel cron ") + args.value(QStringLiteral("job_id")).toString();
    // lcc s13 Agent Teams：摘要取队友名/收件人/固定短文本（仿 s10/s12 风格，lcc 无对应钩子文案）
    if (toolName == ToolNames::SPAWN_TEAMMATE)
        return args.value(QStringLiteral("name")).toString();
    if (toolName == ToolNames::LIST_TEAMMATES)
        return QStringLiteral("teammate list");
    if (toolName == ToolNames::SEND_MESSAGE)
        return QStringLiteral("to ") + args.value(QStringLiteral("to")).toString();
    if (toolName == ToolNames::REQUEST_SHUTDOWN)
        return args.value(QStringLiteral("teammate")).toString();
    if (toolName == ToolNames::REQUEST_PLAN)
        return args.value(QStringLiteral("teammate")).toString();
    if (toolName == ToolNames::REVIEW_PLAN)
        return args.value(QStringLiteral("request_id")).toString();
    if (toolName == ToolNames::CREATE_WORKTREE)
        return args.value(QStringLiteral("name")).toString();
    return QString();
}

// 工具调用的读取口径（声明见 AgentLoopInternal.h）：arguments 为流式拼装出的 JSON 字符串
QString callToolName(const QJsonObject &toolCall)
{
    return toolCall.value(QStringLiteral("function")).toObject()
               .value(QStringLiteral("name")).toString();
}

QString callToolArgsText(const QJsonObject &toolCall)
{
    return toolCall.value(QStringLiteral("function")).toObject()
               .value(QStringLiteral("arguments")).toString();
}

// 严格解析：空/纯空白 arguments 视作空对象（无参工具的合法路径，COMPACT/list_tasks 不受影响）；
// 非法 JSON 不再静默变空对象（此前空跑会带着缺字段的 args 误入 handler），改为在 errorText 里
// 给出回填文案并携带原始参数前 100 字符，便于模型自纠。
ToolCallView parseToolCall(const QJsonObject &toolCall)
{
    ToolCallView view;
    view.name = callToolName(toolCall);
    view.argsText = callToolArgsText(toolCall);
    if (view.argsText.trimmed().isEmpty())
        return view;

    QJsonParseError parseErr{};
    const QJsonDocument argsDoc = QJsonDocument::fromJson(view.argsText.toUtf8(), &parseErr);
    if (argsDoc.isObject())
    {
        view.args = argsDoc.object();
        return view;
    }
    view.errorText = QStringLiteral("Error: invalid tool arguments JSON (%1): %2")
                         .arg(parseErr.errorString(), view.argsText.left(100));
    return view;
}

// 宽容解析：非法/空一律得空对象，供钩子链与历史回放这类只读预览路径使用
QJsonObject parseToolArgsText(const QString &argsText)
{
    if (argsText.trimmed().isEmpty())
        return QJsonObject();
    return QJsonDocument::fromJson(argsText.toUtf8()).object();
}

QJsonObject callToolArgs(const QJsonObject &toolCall)
{
    return parseToolCall(toolCall).args;
}

QJsonObject makeToolResult(const QString &callId, const QString &content)
{
    QJsonObject toolResult;
    toolResult[QStringLiteral("role")] = QStringLiteral("tool");
    toolResult[QStringLiteral("tool_call_id")] = callId;
    toolResult[QStringLiteral("content")] = content;
    return toolResult;
}

QJsonArray snapshotMessages(const QVector<QJsonObject> &messages)
{
    QJsonArray snapshot;
    for (const QJsonObject &msg : messages)
        snapshot.append(msg);
    return snapshot;
}
} // namespace AgentLoopDetail

void AgentLoop::onToolFinished(const QJsonObject &toolCall, const QString &toolName,
                               const QString &summary, const QString &output)
{
    // 已被 stop()/析构中断则终止工具链
    if (!m_running)
        return;

    emit toolOutputReady(toolName, summary, output, !isToolFailure(output));

    // 构建 tool 结果消息回填上下文
    m_toolResultsReady.append(AgentLoopDetail::makeToolResult(
        toolCall.value(QStringLiteral("id")).toString(), output));

    runNextTool();
}

void AgentLoop::executeTool(const QJsonObject &toolCall,
                            const QHash<QString, ToolHandler> &handlers,
                            bool permissionGranted)
{
    // 解析工具名与参数（口径单源见 parseToolCall）：非法 arguments 直接以错误文本回填并结束本调用。
    const AgentLoopDetail::ToolCallView call = AgentLoopDetail::parseToolCall(toolCall);
    const QString toolName = call.name;
    const QJsonObject args = call.args;
    if (!call.ok())
    {
        // 摘要占位（第六轮审计 C8）：args 为空对象时 toolSummary 会渲染出空白卡片头，
        // 改取原始参数文本前 40 字符，让用户至少看得见模型提交了什么
        onToolFinished(toolCall, toolName,
                       QStringLiteral("%1: %2").arg(toolName, call.argsText.left(40)),
                       call.errorText);
        return;
    }
    const QString summary = AgentLoopDetail::toolSummary(toolName, args);

    // compact 工具（lcc s08）：loop.py 在 execute_tool 之前拦截（PreToolUse/PostToolUse 钩子
    // 均不运行、不追加 tool_result、不发卡片、不计入 used_todo）——此处同样在钩子链之前
    // 特判：仅置位并手动推进队列，压缩替换在批尾执行（见 runNextTool 的 flush 分支）
    if (toolName == ToolNames::COMPACT)
    {
        m_compactRequested = true;
        runNextTool();
        return;
    }

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
        if (gate.startsWith(AgentLoopDetail::askPrefix()))
        {
            m_awaitingPermission = true;
            m_pendingPermissionCall = toolCall;
            emit permissionRequired(toolName, summary, gate.mid(AgentLoopDetail::askPrefix().size()));
            return; // 队列暂停：不回填、不请求，等用户裁决
        }
        onToolFinished(toolCall, toolName, summary, gate);
        return;
    }

    // 事前进行中信号（B3）：已通过权限门、即将真正执行——UI 据此先行落 live 工具卡。
    // compact 已在函数前部特判离开，硬拒绝/询问暂停路径不会到达此处；询问批准的
    // 续跑路径（permissionGranted=true）重走全链会再次到达，UI 端同名 live 卡幂等去重
    emit toolStarted(toolName, summary);

    // bash 走异步进程链（不进 handler 表：跨事件循环回填，表内只放同步工具）
    if (toolName == ToolNames::BASH)
    {
        // 后台任务分支（lcc s11 execute_tool 后台门）：run_in_background 严格 true 时转异步，
        // 占位文本立即闭合 tool_use↔tool_result 配对，真实结果由后续回合收割注入
        if (BackgroundTasksManager::shouldRunBackground(toolName, args))
        {
            const QString command = args.value(QStringLiteral("command")).toString();
            QString error;
            const QString taskId = m_backgroundTasks.start(
                command, toolCall.value(QStringLiteral("id")).toString(), &error);

            QString output;
            if (taskId.isEmpty())
            {
                // lcc: Popen 前校验失败（ValueError）折叠为 start error 文案，不起进程
                output = QStringLiteral("[Background task start error] %1").arg(error);
            }
            else
            {
                output = QStringLiteral(
                    "[Background task %1 started] The result will be collected on a later turn.")
                    .arg(taskId);
                executeBashAsync(toolCall, args, /*background=*/true, taskId);
            }

            // lcc execute_tool 统一尾部：后台分支的占位输出同样触发 PostToolUse 后收口
            triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, ToolNames::BASH, command, output);
            return;
        }

        executeBashAsync(toolCall, args);
        return;
    }

    // task 走子代理异步链（lcc s06）：独立上下文黑盒，完成后经回调走 onToolFinished 收口
    if (toolName == ToolNames::TASK)
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
    if (toolName == ToolNames::TODO_WRITE)
        m_usedTodoThisRound = true;

    // PostToolUse 钩子（lcc s04）：handler 产出结果后、回填前触发。
    // 表内工具与未知工具共用此出口（lcc 中 Unknown 分支同样触发 PostToolUse；
    // 被 PreToolUse 拦截的调用不会走到这里，与 lcc 拒绝即 continue 的语义一致）
    triggerPostToolUseHooks(toolCall, output);

    onToolFinished(toolCall, toolName, summary, output);
}

bool AgentLoop::isToolFailure(const QString &output)
{
    // 成败判定单源（见头文件注释）：文案族为全仓既成约定——各 handler/钩子/BashRunner/
    // TaskStore（本任务起统一补 "Error: " 前缀）的失败输出必居其一；stop 链的
    // "(stopped)"/"(cancelled)" 直写历史不经本判定（不发 toolOutputReady）。
    return output.startsWith(QStringLiteral("Error:"))
        || output == QLatin1String("Permission denied")
        || output.startsWith(QStringLiteral("Blocked:"))
        || output.startsWith(QStringLiteral("[Background task start error]"))
        || output.startsWith(QStringLiteral("Unknown tool:"));
}

QHash<QString, AgentLoop::ToolHandler> AgentLoop::baseFileToolHandlers(const QString &workDir)
{
    // 宿主与子代理共用的同步文件工具集（lcc s06 toolsHandlers/subToolsHandlers 的交集部分）：
    // 各自以传入的 workDir 为沙箱根构建，互不串扰
    QHash<QString, ToolHandler> handlers;
    handlers.insert(ToolNames::READ_FILE, [workDir](const QJsonObject &args) {
        return runReadFileIn(workDir, args);
    });
    handlers.insert(ToolNames::WRITE_FILE, [workDir](const QJsonObject &args) {
        return runWriteFileIn(workDir, args);
    });
    handlers.insert(ToolNames::EDIT_FILE, [workDir](const QJsonObject &args) {
        return runEditFileIn(workDir, args);
    });
    handlers.insert(ToolNames::GLOB, [workDir](const QJsonObject &args) {
        return runGlobIn(workDir, args);
    });
    return handlers;
}

QHash<QString, AgentLoop::ToolHandler> AgentLoop::mainToolHandlers()
{
    // 效率 P4：本函数只负责「构建一份新表」，结果交 ensureToolHandlers() 缓存复用；
    // 捕获方式逐 lambda 核实——首行 baseFileToolHandlers 将 workDir **按值**固化进四个
    // 文件工具 lambda（表构建时刻定死沙箱根，切目录后旧表即陈旧，须失效重建）；
    // 其余 todo_write/load_skill/任务图六件套/cron 三件套共十个 lambda 均 [this] 捕获、
    // 执行时才读成员，表内不固化任何可变状态，可全局复用
    // 主循环同步工具集：文件四件套 + todo_write + load_skill + 任务图六件套（bash/task 为 executeTool 异步特判）；
    // lcc s07：load_skill 仅主循环注册，子代理工具白名单不含它（见 SubAgent filterSubTools）；
    // lcc s10：任务图六件套同为仅主循环注册（lcc subTools/subToolsHandlers 仍为五工具，天然不进 sub）
    // lcc s12：cron 三件套同为仅主循环注册（子代理白名单不含，天然不进 sub）
    QHash<QString, ToolHandler> handlers = baseFileToolHandlers(m_workDir);
    // s13 租约感知围栏根（挂载十三）：主循环的文件四件套改为本表覆盖项——执行时现取
    // leadToolCwd()（Lead 持租约且绑定 worktree → worktree 路径；无租约 → assignmentCwd
    // ①号分支经 workDirSink 回落 m_workDir，与 s13 前逐字节一致）。
    // baseFileToolHandlers 保持构造期按值固化语义不动——SubAgent 仍复用它（P4 既定：
    // 子代理不做租约切换）。
    handlers.insert(ToolNames::READ_FILE, [this](const QJsonObject &args) {
        return runReadFileIn(leadToolCwd(), args);
    });
    handlers.insert(ToolNames::WRITE_FILE, [this](const QJsonObject &args) {
        return runWriteFileIn(leadToolCwd(), args);
    });
    handlers.insert(ToolNames::EDIT_FILE, [this](const QJsonObject &args) {
        return runEditFileIn(leadToolCwd(), args);
    });
    handlers.insert(ToolNames::GLOB, [this](const QJsonObject &args) {
        return runGlobIn(leadToolCwd(), args);
    });
    handlers.insert(ToolNames::TODO_WRITE, [this](const QJsonObject &args) {
        return runTodoWrite(args);
    });
    handlers.insert(ToolNames::LOAD_SKILL, [this](const QJsonObject &args) {
        return runLoadSkill(args);
    });
    // lcc s10 任务图六件套：重构第三轮拆出 TaskStore 后本表直通 m_taskStore（薄委托，零转发函数）
    handlers.insert(ToolNames::CREATE_TASK, [this](const QJsonObject &args) {
        return m_taskStore.runCreateTask(args);
    });
    handlers.insert(ToolNames::UPDATE_TASK, [this](const QJsonObject &args) {
        return m_taskStore.runUpdateTask(args);
    });
    handlers.insert(ToolNames::LIST_TASKS, [this](const QJsonObject &) {
        return m_taskStore.runListTasks();
    });
    handlers.insert(ToolNames::GET_TASK, [this](const QJsonObject &args) {
        return m_taskStore.runGetTask(args);
    });
    // s13 补充事实①：Lead 的 claim/complete 改走租约通道（owner="agent"=kLeadOwnerKey），
    // 领取落内存租约 + 版本递增，回合尾由 releaseCompletedAssignment 退租（lcc loop.py 同型）；
    // 遗留非租约版 runClaimTask/runCompleteTask 不再挂进主循环表。
    handlers.insert(ToolNames::CLAIM_TASK, [this](const QJsonObject &args) {
        return m_taskStore.runClaimTaskLeased(args,
                                              QString::fromLatin1(AgentTeamsManager::kLeadOwnerKey));
    });
    handlers.insert(ToolNames::COMPLETE_TASK, [this](const QJsonObject &args) {
        return m_taskStore.runCompleteTaskLeased(args,
                                                 QString::fromLatin1(AgentTeamsManager::kLeadOwnerKey));
    });
    handlers.insert(ToolNames::SCHEDULE_CRON, [this](const QJsonObject &args) {
        return runScheduleCron(args);
    });
    handlers.insert(ToolNames::CANCEL_CRON, [this](const QJsonObject &args) {
        return runCancelCron(args);
    });
    handlers.insert(ToolNames::LIST_CRONS, [this](const QJsonObject &) {
        return runListCrons();
    });
    // lcc s13 Agent Teams（Lead 侧 7 工具，lcc TEAM_TOOLS agent_teams_manager.py :217-295）：
    // 直通引擎同步壳（失败一律折叠为工具输出字符串交还模型，仓规约同任务图/定时族）。
    handlers.insert(ToolNames::SPAWN_TEAMMATE, [this](const QJsonObject &args) {
        return m_teams.runSpawnTeammate(
            args.value(QStringLiteral("name")).toString(),
            args.value(QStringLiteral("role")).toString(),
            args.value(QStringLiteral("prompt")).toString(),
            args.value(QStringLiteral("task_id")).toString(),
            args.value(QStringLiteral("require_plan")).toBool());
    });
    handlers.insert(ToolNames::LIST_TEAMMATES, [this](const QJsonObject &) {
        return m_teams.runListTeammates();
    });
    handlers.insert(ToolNames::SEND_MESSAGE, [this](const QJsonObject &args) {
        return m_teams.runSendMessage(
            args.value(QStringLiteral("to")).toString(),
            args.value(QStringLiteral("content")).toString());
    });
    handlers.insert(ToolNames::REQUEST_SHUTDOWN, [this](const QJsonObject &args) {
        return m_teams.runRequestShutdown(args.value(QStringLiteral("teammate")).toString());
    });
    handlers.insert(ToolNames::REQUEST_PLAN, [this](const QJsonObject &args) {
        return m_teams.runRequestPlan(
            args.value(QStringLiteral("teammate")).toString(),
            args.value(QStringLiteral("task")).toString());
    });
    handlers.insert(ToolNames::REVIEW_PLAN, [this](const QJsonObject &args) {
        return m_teams.runReviewPlan(
            args.value(QStringLiteral("request_id")).toString(),
            args.value(QStringLiteral("approve")).toBool(),
            args.value(QStringLiteral("feedback")).toString());
    });
    handlers.insert(ToolNames::CREATE_WORKTREE, [this](const QJsonObject &args) {
        return m_teams.runCreateWorktree(
            args.value(QStringLiteral("name")).toString(),
            args.value(QStringLiteral("task_id")).toString());
    });
    return handlers;
}

const QHash<QString, AgentLoop::ToolHandler> &AgentLoop::ensureToolHandlers()
{
    // 惰性首用构建（P4）：表构建仅依赖 m_workDir（构造 init-list 或 setWorkDir 定值）与 this，
    // 无构造时序约束，但取惰性可免从不跑工具的会话（如仅恢复历史浏览）空转建表；
    // 失效策略：setWorkDir 写目录后 clear（文件四件套按值捕获 workDir 的保险失效路径，
    // 见 mainToolHandlers 头注释的逐 lambda 核实结论），其余路径表一经构建即恒定
    if (m_toolHandlers.isEmpty())
        m_toolHandlers = mainToolHandlers();
    return m_toolHandlers;
}

