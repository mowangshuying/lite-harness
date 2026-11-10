#pragma once

// 工具 schema 单源（header-only、纯 QtCore，纪律同 CompactTokens.h / MemoryRules.h / HeatmapGrid.h）：
// `createToolsDefinition()` 逐工具构造发给服务端的 OpenAI function 定义，27 个工具。
//
// 为什么单独成头：本体此前长在 `AgentLoopPrompt.cpp` 里，而该 TU 经 `AgentLoop.h` 传递 include
// `QOpenAi.h`（链 Qt6::Network），按「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest，
// 于是**整套 schema 零直测**。它是「错了不报错、只在运行时炸」的高危面：
//   · `required` 里写了 `properties` 没有的键、或 `parameters.type` 不是 "object" → 服务端 400，
//     整个会话一发请求就失败，而本地编译与既有测试全绿；
//   · `function.name` 与执行器读键、与 `SubAgent` 白名单不同源 → 工具能被调用但恒 no-op，
//     用户看到的是「模型说做了但什么都没发生」；
//   · `todos.status` 的 enum 与执行侧三值白名单漂一格 → 模型合法提交的值被自己拒掉。
// 这些只能靠断言钉，肉眼扫 490 行字面量扫不出来。
//
// `AgentLoop::createToolsDefinition()` 退化为**一行转发**，调用点与 `AgentLoop.h` 签名均不变。
// 硬约束：本头**绝不 include `AgentLoop.h`**——那会把 moc + QOpenAi（Qt6::Network）依赖拖进测试
// 目标（同 `AgentPathGuard.h:67` 登记的坑）。函数内 `static const QJsonArray cached` 在 inline
// 函数里仍是跨 TU 单实例，缓存语义不变。

#include "AgentConstants.h"   // AgentConst::kTodoMaxItems（与执行器同源，勿在此另写字面量）
#include "ToolNames.h"        // 工具名单源，勿另立硬编码清单
#include "WorkflowManager.h"  // workflow 信封单源 WorkflowManager::toolInfo()

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace ToolSchema {

QJsonArray createToolsDefinition()
{
    // 缓存理由：27 个工具 schema 全为静态字面量、无运行期可变态、无 tr()（C 类禁翻区 QStringLiteral），
    // 每个 LLM 请求（含 SubAgent 与重试路径）从零重建属重复分配；改为函数局部 static 惰性一次性初始化。
    // 零线程原则（全仓主线程事件驱动）下，函数局部 static 的初始化与读取均无线程安全问题。
    // 按值返回 QJsonArray 依赖 Qt 隐式共享（CoW），拷贝廉价，调用点无需改动。
    static const QJsonArray cached = [] {
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
    // bash（lcc s11）：新增可选 run_in_background boolean（required 仍只有 command）；
    // 子代理侧经 SubAgent filterSubTools 删除该参数（双重禁令之 schema 层）。
    // 描述已破 lcc 逐字平价（lite 有意偏差）：宿主壳切 PowerShell 5.1 后如实声明语法族，
    // 与 BashRunner::start 的 powershell.exe 启动同源，防模型写 bash 风格命令失败重试
    tools.append(makeTool(ToolNames::BASH,
                          QStringLiteral("Run a Windows PowerShell command (powershell.exe "
                                         "-NoProfile -NonInteractive -Command). Use PowerShell 5.1 "
                                         "syntax, not bash/sh: chain with ; not && where semantics "
                                         "differ, no /dev/null redirections."),
                          { {QStringLiteral("command"), QStringLiteral("string")},
                            {QStringLiteral("run_in_background"), QStringLiteral("boolean")} },
                          {QStringLiteral("command")}));
    tools.append(makeTool(ToolNames::READ_FILE, QStringLiteral("Read file contents"),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("limit"), QStringLiteral("integer")} },
                          {QStringLiteral("path")}));
    tools.append(makeTool(ToolNames::WRITE_FILE, QStringLiteral("Write content to a file"),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("content"), QStringLiteral("string")} },
                          {QStringLiteral("path"), QStringLiteral("content")}));
    // lcc s02 原码 edit_file 漏了 required，此处修正
    // lcc s10 破坏性改名跟随：old_text/new_text → old_string/new_string（schema 与 run_edit 读键同步，
    // runEditFileIn 已改）；描述文案 s10 不变
    tools.append(makeTool(ToolNames::EDIT_FILE, QStringLiteral("Replace exact text in a file once."),
                          { {QStringLiteral("path"), QStringLiteral("string")},
                            {QStringLiteral("old_string"), QStringLiteral("string")},
                            {QStringLiteral("new_string"), QStringLiteral("string")} },
                          {QStringLiteral("path"), QStringLiteral("old_string"), QStringLiteral("new_string")}));
    // lcc s02 原码 glob 的 required 误写为 "require"，此处修正
    tools.append(makeTool(ToolNames::GLOB,
                          QStringLiteral("Find files matching a glob pattern; ** matches recursively."),
                          { {QStringLiteral("pattern"), QStringLiteral("string")} },
                          {QStringLiteral("pattern")}));

    // todo_write（lcc s05）：todos 为嵌套 array<object{content,status}> schema，
    // makeTool lambda 只支持平铺 string/integer 参数，按 lcc 原文手工构造；
    // 外层包装与 makeTool 产物一致（type:function + function.parameters）。
    // s17 偏差登记：description 原为 lcc tools_manager.py:91 逐字一句，扩为含**时序纪律**的
    // 指令串（全量重发 / in_progress 先于开工 / completed 后即改 / 禁攒批终局刷）——用户反馈
    // 模型不及时调用 todo_write，面板滞后于真实进度；无测试锁此串（grep 零命中）
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
        todosSchema[QStringLiteral("maxItems")] = AgentConst::kTodoMaxItems;
        todosSchema[QStringLiteral("items")] = items;

        QJsonObject properties;
        properties[QStringLiteral("todos")] = todosSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = properties;
        inputSchema[QStringLiteral("required")] =
            QJsonArray::fromStringList({ QStringLiteral("todos") });

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::TODO_WRITE;
        function[QStringLiteral("description")] =
            QStringLiteral("Create and manage the task list for your current coding session. "
                           "Resend the full list on every call; mark an item in_progress right "
                           "before starting it and completed right after finishing it — update "
                           "whenever a status changes, not in bulk at the end.");
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
        // s17.2：子代理 system prompt 是独立单源（SubAgent::subSystemPrompt 仅一句
        // "coding agent at <workDir>"，**没有**主循环的临时目录引导语），且它拿不到
        // sessionDataRoot——temp 路径只能由宿主在 prompt 文本里转述给它。
        // 本仓工具表是函数级 static const QJsonArray cached（禁运行期插值、全会话字节
        // 恒定），故这里只给「把你自己 system prompt 里那条路径抄进来」的自指式指令，
        // 不写实际路径。禁翻区：QStringLiteral、永不 tr()（纪律同 function 级 description）。
        promptSchema[QStringLiteral("description")] = QStringLiteral(
            "The task for the subagent. The subagent starts with no knowledge of your "
            "session temp directory, so when the task involves temporary/test/scratch "
            "files, copy the absolute temp path from your own system prompt into this "
            "text; otherwise the subagent will drop throwaway files in the project root.");

        QJsonObject properties;
        properties[QStringLiteral("prompt")] = promptSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = properties;
        inputSchema[QStringLiteral("required")] =
            QJsonArray::fromStringList({ QStringLiteral("prompt") });

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::TASK;
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
    tools.append(makeTool(ToolNames::LOAD_SKILL,
                          QStringLiteral("Load the full SKILL.md content by skill name."),
                          { { QStringLiteral("name"), QStringLiteral("string") } },
                          { QStringLiteral("name") }));

    // compact（lcc s08 第 9 个工具）：schema-only —— 空 properties、无 required（逐字对齐
    // lcc COMPACT 定义，描述无句号结尾）；不入 handler 表，executeTool 在钩子链前特判
    {
        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = QJsonObject();

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::COMPACT;
        function[QStringLiteral("description")] =
            QStringLiteral("Summarize earlier conversation to free context space");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // ---- lcc s10 任务图六件套（第 10~15 个工具，按 lcc tools 顺序追加于 compact 之后）----
    // create_task：makeTool 不支持 additionalProperties，手工构造；required 仅 [subject]
    {
        QJsonObject subjectSchema;
        subjectSchema[QStringLiteral("type")] = QStringLiteral("string");
        QJsonObject descriptionSchema;
        descriptionSchema[QStringLiteral("type")] = QStringLiteral("string");

        QJsonObject props;
        props[QStringLiteral("subject")] = subjectSchema;
        props[QStringLiteral("description")] = descriptionSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = props;
        inputSchema[QStringLiteral("required")] = QJsonArray{ QStringLiteral("subject") };
        inputSchema[QStringLiteral("additionalProperties")] = false;

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::CREATE_TASK;
        function[QStringLiteral("description")] =
            QStringLiteral("Create a task and return its runtime-generated ID.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // update_task：task_id 带 pattern；addBlockedBy 为 camelCase 逐字对齐 lcc（数组 items 带 pattern、
    // minItems 1）；required 双键；additionalProperties false
    {
        const QString idPattern = QStringLiteral("^task_[0-9a-f]{8}$");

        QJsonObject taskIdSchema;
        taskIdSchema[QStringLiteral("type")] = QStringLiteral("string");
        taskIdSchema[QStringLiteral("pattern")] = idPattern;

        QJsonObject itemSchema;
        itemSchema[QStringLiteral("type")] = QStringLiteral("string");
        itemSchema[QStringLiteral("pattern")] = idPattern;

        QJsonObject blockedBySchema;
        blockedBySchema[QStringLiteral("type")] = QStringLiteral("array");
        blockedBySchema[QStringLiteral("items")] = itemSchema;
        blockedBySchema[QStringLiteral("minItems")] = 1;

        QJsonObject props;
        props[QStringLiteral("task_id")] = taskIdSchema;
        props[QStringLiteral("addBlockedBy")] = blockedBySchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = props;
        inputSchema[QStringLiteral("required")] =
            QJsonArray{ QStringLiteral("task_id"), QStringLiteral("addBlockedBy") };
        inputSchema[QStringLiteral("additionalProperties")] = false;

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::UPDATE_TASK;
        function[QStringLiteral("description")] =
            QStringLiteral("Add dependencies using IDs returned by create_task.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // list_tasks：逐字对齐 lcc LIST_TASKS——properties 为空对象，且无 required、无 additionalProperties
    //（makeTool 恒写 required，故手工构造，勿"顺手补齐"；先例见 todo_write/compact）
    {
        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = QJsonObject();

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::LIST_TASKS;
        function[QStringLiteral("description")] =
            QStringLiteral("List tasks with status, owner, and dependencies.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // get/claim/complete_task：task_id 无 pattern（逐字对齐 lcc，勿与 update_task 的 schema 混同），
    // required [task_id]，无 additionalProperties —— makeTool 可表达
    tools.append(makeTool(ToolNames::GET_TASK, QStringLiteral("Get a task by ID."),
                          { {QStringLiteral("task_id"), QStringLiteral("string")} },
                          {QStringLiteral("task_id")}));
    tools.append(makeTool(ToolNames::CLAIM_TASK,
                          QStringLiteral("Claim a pending task whose dependencies are complete."),
                          { {QStringLiteral("task_id"), QStringLiteral("string")} },
                          {QStringLiteral("task_id")}));
    tools.append(makeTool(ToolNames::COMPLETE_TASK,
                          QStringLiteral("Complete the task claimed by this agent."),
                          { {QStringLiteral("task_id"), QStringLiteral("string")} },
                          {QStringLiteral("task_id")}));

    // ---- lcc s12 定时任务三件套（第 16~18 个）：仅主循环注册，子代理白名单不含；
    // 描述与参数逐字对齐 lcc SCHEDULE_CRON/LIST_CRONS/CANCEL_CRON。模型对定时的
    // 感知仅来自 schema 本身（lcc s12 无 system prompt 新增段——对齐，非遗漏）。
    // makeTool 空 props/空 required 产出 "properties":{} 与 "required":[]，
    // 恰合 LIST_CRONS（区别于 s10 list_tasks 无 required 键才手工构造）----
    tools.append(makeTool(ToolNames::SCHEDULE_CRON,
                          QStringLiteral("Schedule a prompt with a 5-field cron expression."),
                          { {QStringLiteral("cron"), QStringLiteral("string")},
                            {QStringLiteral("prompt"), QStringLiteral("string")},
                            {QStringLiteral("recurring"), QStringLiteral("boolean")},
                            {QStringLiteral("durable"), QStringLiteral("boolean")} },
                          {QStringLiteral("cron"), QStringLiteral("prompt")}));
    tools.append(makeTool(ToolNames::LIST_CRONS,
                          QStringLiteral("List scheduled cron jobs."),
                          QList<QPair<QString, QString>>{}, QStringList{}));
    tools.append(makeTool(ToolNames::CANCEL_CRON,
                          QStringLiteral("Cancel a cron job by ID."),
                          { {QStringLiteral("job_id"), QStringLiteral("string")} },
                          {QStringLiteral("job_id")}));

    // ---- lcc s13 Agent Teams Lead 侧七件套（第 19~25 个）：仅主循环注册（子代理白名单
    // 不含，天然不进 sub）；描述与参数逐字对齐 lcc TEAM_TOOLS（agent_teams_manager.py
    // :217-295，anthropic input_schema → lite OpenAI function 信封转写，同 s00~s12 各族
    // 先例）。模型对团队的感知仅来自 schema + prompt_teams 静态纪律段本身。----
    // spawn_teammate：name/task_id 带 pattern，makeTool 不支持 → 手工构造；required 三键
    {
        QJsonObject nameSchema;
        nameSchema[QStringLiteral("type")] = QStringLiteral("string");
        nameSchema[QStringLiteral("pattern")] = QStringLiteral("^[A-Za-z0-9_-]{1,64}$");

        QJsonObject roleSchema;
        roleSchema[QStringLiteral("type")] = QStringLiteral("string");

        QJsonObject promptSchema;
        promptSchema[QStringLiteral("type")] = QStringLiteral("string");
        // s17.2：与 task 的 prompt 同款自指式引导（纪律见彼处注释）——队友 system prompt
        // （TeammateRuntime::systemPrompt）同样没有临时目录引导语，只说"文件/壳工具用 Task
        // 的工作目录、该目录不是沙箱"。围栏根取 m_workDir、worktree 落其子树内，故会话
        // .temp 对队友可写，转述路径是有效指令。
        promptSchema[QStringLiteral("description")] = QStringLiteral(
            "First message for the teammate. The teammate starts with no knowledge of "
            "your session temp directory, so when its task involves temporary/test/scratch "
            "files, copy the absolute temp path from your own system prompt into this text; "
            "otherwise it will drop throwaway files in the project root.");

        QJsonObject taskIdSchema;
        taskIdSchema[QStringLiteral("type")] = QStringLiteral("string");
        taskIdSchema[QStringLiteral("pattern")] = QStringLiteral("^task_[0-9a-f]{8}$");

        QJsonObject requirePlanSchema;
        requirePlanSchema[QStringLiteral("type")] = QStringLiteral("boolean");

        QJsonObject props;
        props[QStringLiteral("name")] = nameSchema;
        props[QStringLiteral("role")] = roleSchema;
        props[QStringLiteral("prompt")] = promptSchema;
        props[QStringLiteral("task_id")] = taskIdSchema;
        props[QStringLiteral("require_plan")] = requirePlanSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = props;
        inputSchema[QStringLiteral("required")] =
            QJsonArray{ QStringLiteral("name"), QStringLiteral("role"),
                        QStringLiteral("prompt") };

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::SPAWN_TEAMMATE;
        function[QStringLiteral("description")] =
            QStringLiteral("Spawn a persistent teammate.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // list_teammates：逐字对齐 lcc——properties 为空对象且无 required 键（同 list_tasks
    // /compact 手工先例，勿用 makeTool 以免产出 "required":[]）
    {
        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = QJsonObject();

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::LIST_TEAMMATES;
        function[QStringLiteral("description")] =
            QStringLiteral("List active teammates.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    tools.append(makeTool(ToolNames::SEND_MESSAGE,
                          QStringLiteral("Message a teammate."),
                          { {QStringLiteral("to"), QStringLiteral("string")},
                            {QStringLiteral("content"), QStringLiteral("string")} },
                          {QStringLiteral("to"), QStringLiteral("content")}));
    tools.append(makeTool(ToolNames::REQUEST_SHUTDOWN,
                          QStringLiteral("Ask a teammate to shut down."),
                          { {QStringLiteral("teammate"), QStringLiteral("string")} },
                          {QStringLiteral("teammate")}));
    tools.append(makeTool(ToolNames::REQUEST_PLAN,
                          QStringLiteral(
                              "Require a teammate plan before workspace changes."),
                          { {QStringLiteral("teammate"), QStringLiteral("string")},
                            {QStringLiteral("task"), QStringLiteral("string")} },
                          {QStringLiteral("teammate"), QStringLiteral("task")}));
    tools.append(makeTool(ToolNames::REVIEW_PLAN,
                          QStringLiteral("Approve or reject a plan."),
                          { {QStringLiteral("request_id"), QStringLiteral("string")},
                            {QStringLiteral("approve"), QStringLiteral("boolean")},
                            {QStringLiteral("feedback"), QStringLiteral("string")} },
                          {QStringLiteral("request_id"), QStringLiteral("approve")}));

    // create_worktree：name 带 pattern/maxLength 且 additionalProperties false → 手工构造；
    // name pattern 逐字对齐 lcc :286（转义串在 C++ 字面量中为 \\.，与原 JSON 语义同形）
    {
        QJsonObject nameSchema;
        nameSchema[QStringLiteral("type")] = QStringLiteral("string");
        nameSchema[QStringLiteral("pattern")] =
            QStringLiteral("^(?!.*\\.\\.)[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
        nameSchema[QStringLiteral("maxLength")] = 64;

        QJsonObject taskIdSchema;
        taskIdSchema[QStringLiteral("type")] = QStringLiteral("string");

        QJsonObject props;
        props[QStringLiteral("name")] = nameSchema;
        props[QStringLiteral("task_id")] = taskIdSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = props;
        inputSchema[QStringLiteral("required")] =
            QJsonArray{ QStringLiteral("name"), QStringLiteral("task_id") };
        inputSchema[QStringLiteral("additionalProperties")] = false;

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::CREATE_WORKTREE;
        function[QStringLiteral("description")] =
            QStringLiteral("Create and bind a task worktree.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // s14 connect_mcp（lcc s14 7b4ac1c CONNECT_TOOL 移植；仿 create_worktree 先例手工构造，
    // 因需保持「无 enum」偏差——空注册表无候选名可枚举，lcc 的 enum [docs,deploy] 随 mock 弃，
    // 偏差登记见 McpManager.h 顶部第 2 条，勿"修回"加 enum）。
    {
        QJsonObject nameSchema;
        nameSchema[QStringLiteral("type")] = QStringLiteral("string");

        QJsonObject props;
        props[QStringLiteral("name")] = nameSchema;

        QJsonObject inputSchema;
        inputSchema[QStringLiteral("type")] = QStringLiteral("object");
        inputSchema[QStringLiteral("properties")] = props;
        inputSchema[QStringLiteral("required")] = QJsonArray{ QStringLiteral("name") };

        QJsonObject function;
        function[QStringLiteral("name")] = ToolNames::CONNECT_MCP;
        function[QStringLiteral("description")] =
            QStringLiteral("Connect to an MCP server and discover its tools.");
        function[QStringLiteral("parameters")] = inputSchema;

        QJsonObject tool;
        tool[QStringLiteral("type")] = QStringLiteral("function");
        tool[QStringLiteral("function")] = function;
        tools.append(tool);
    }

    // s16 workflow（lcc s16 workflow.py 工具入口对照）：信封直接取 Lane A WorkflowManager::toolInfo()——
    // 名字/描述/参数 schema 单源，免手工二次书写漂移（connect_mcp 当年手工构造是为保「无 enum」偏差，
    // 此处 toolInfo() 与需求逐字节一致可全量复用，无手工段）。选形登记：toolInfo 为成员非静态→
    // cached lambda 内临时构造原型对象取值；WorkflowManager 构造纯内存注册表、不建目录不触盘
    // （Lane A 目录自愈在写/读入口的内联 mkpath，见 WorkflowManager.cpp:560/594/642
    //  与 WfJournal::open :796；引擎无 ensureStoreDirs 函数），主线程零线程原则
    // 下 cached 一次性初始化无并发风险；toolInfo() 不含任何 per-实例可变态，产物恒定。
    {
        const WorkflowManager wfPrototype;
        tools.append(wfPrototype.toolInfo());
    }

    return tools;
    }();

    return cached;
}
} // namespace ToolSchema
