# AGENTS.md — lite-harness

Qt6 桌面 AI 编码代理 harness：内置 LLM 工具主循环、会话、记忆、定时任务的单可执行文件。

## 构建

- **工具链:** Qt 6.9.0 + MSVC 2022 + CMake 3.20+，C++17，仅 Windows
- **Prefix:** `CMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64`
- **构建目录:** `build/`（VS 解决方案 `build/lite-harness.sln`）
- **输出路径:** `build/bin/lite-harness.exe`（CMake VERSION 0.1.0）
- **编译选项:** MSVC `/W4 /utf-8`（无 `/WX`）；仅链 Qt6 Widgets/Svg/Network + FluentUI::Controls/Utils
- **无测试、无 CI、无 lint 配置** — 通过构建和运行验证
- **首次构建前置:** `git submodule update --init 3rdparty/FluentUI`（必需）。`3rdparty/lcc` 仅为移植规格参考、从不参与构建，init 可选；`3rdparty/sqlite_orm` 仍登记于 .gitmodules 但已摘除出构建树（零引用）。
- **源文件收集:** `file(GLOB CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` + `GLOB_RECURSE "stylesheet/*.qss"`（经 `qt_add_resources` 打包为 `:/stylesheet/`）。**新增源文件/QSS 无需改 CMakeLists.txt**，重新 configure 即自动拾取。

## 架构（src/）

### Agent 核心链

- **AgentLoop** — LLM 主循环 + 18 工具分发（名单唯一来源 `ToolNames.h`）：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron。权限门（bash 硬拒绝表 + ASK 规则）与生命周期钩子（UserPromptSubmit/PreToolUse/PostToolUse/Stop）。任务图 6 工具与 cron 3 工具仅主循环注册。
- **QOpenAi** — OpenAI 兼容客户端：`ChatStream` SSE 流式（thinkingDelta/textDelta/messageFinished）+ blocking `create()`（嵌套事件循环，仍在主线程）。运行时配置来自环境变量 `QOpenAiBaseUrl` / `QOpenAiToken`（`initByEnv()` 于主窗口构造调用），模型名 `MODEL_ID`，缺省 `AgentConst::kDefaultModel`。
- **TaskStore** — 任务图存储（lcc s10 移植）：每任务一个 `<会话根>/.task/task_<hex8>.json`，每次操作直读磁盘；6 个 run_* handler + 14 内核方法，失败一律折叠为工具输出字符串。
- **SubAgent** — `task` 工具子代理（lcc s06）：全新上下文、黑盒只回最终文本、回合预算 50；仅开放 read/write/edit/glob + bash 异步。
- **BashRunner** — bash 执行单源（危险检测 / 截断 / 超时终态 / QProcess 启动），AgentLoop 前后端与 SubAgent 共用。
- **BackgroundTasksManager** — 后台 bash 任务台账（lcc s11）：`run_in_background` 严格布尔判定，宿主驱动 QProcess，结果以 `<task_notification>` 注入下一回合。
- **CronSchedulerManager** — cron 定时任务（lcc s12）：5 段表达式校验/匹配，`<会话根>/scheduled_tasks.json` 持久账本，QTimer 1s 轮询替代线程，at-least-once 两段投递。
- **CompactManager** — 上下文压缩（lcc s08）：五段管线 toolResultBudget→snipCompact→microCompact→fitToolResults→compactHistory + 溢出反应式压缩；转录落 `<会话根>/.transcripts/*.jsonl`，超大工具输出卸载到 `.task_outputs/tool-results`。
- **MemoryManager** — 记忆（lcc s09）：`<会话根>/.memory/`（MEMORY.md 索引 + slug.md 记录）；沉淀（会话自然结束）、召回（LLM 选择 + 关键词兜底 → system prompt 尾部）、整理（阈值重写带快照回滚）。

### 会话层

- **SessionStore** — 纯静态工具：全局索引 `<workDir>/.lite-harness/index.json`（QSaveFile 原子写，条目 dataId/title/model/workDir/时间戳）。只管索引，history.json 归 AgentLoop。
- **SessionRegistry** — 导航 key ↔（page / dataId / 导航子项）反查表 + 右键监听映射；QPointer 观察、零所有权。

### 单源常量 / 纯头

- **AgentConstants.h** — 模型清单、kMaxTokens、bash 超时/错误文案、输出截断、glob 上限与剪枝目录、数据目录名（`.task`/`.temp`/`.transcripts`/`.task_outputs/tool-results` — 拼法涉数据兼容，不可改）。
- **LayoutConstants.h** — 消息列宽/边距（NewChatPage/ChatSessionPage/ChatMsgEdit 同列对齐）。
- **NavItem.h** — 导航键常量（NavKey）+ NavItem（带 removeChildItem）。**ToolNames.h** — 18 工具名。**ToolTagKind.h** — 工具→样式标签（write/run/search/read/plan/delegate/other），经动态属性喂给 QSS 选择器。

### UI 层

- **LiteHarness** — 主窗口（FluFrameLessWidget）：FluVNavigationView + FluStackedLayout，会话新建/恢复（按索引升序）/重命名/删除，关闭时运行守卫。
- **ChatSessionPage** — 每会话一页：AgentLoop + 滚动消息流 + 输入框 + 只读路径条；历史回放与就地刷新。
- **NewChatPage / SettingsPage / BasePage** — 发起页、设置页（QSettings `defaultWorkDir`）、页面基类。
- **MessageBubbleWidget** — 气泡流式渲染（打字机），首次工具/思考事件后重建为时间线。
- **CollapsibleBlock** — 折叠动画基类（32px 头部 + 300ms OutCubic contentHeight 动画），子类 ThinkingBlock / ToolBlock / TodoCard。基类构造禁调虚函数，子类构造尾再 bind 主题。
- **ThemeAware** — 「加载 QSS + 订阅 themeChanged + 重载」样板单源（约 12 处旧复制已收敛）。
- **ChatMsgEdit / SendMsgButton / PermissionCard / WorkDirPathBar / FluentInputDialog** — 输入区、圆形 SVG 发送钮、内联审批卡（拒绝默认焦点、理由 EN→ZH）、工作目录条、通用单行输入对话框。

## 数据与路径

- 所有会话数据落在 **`<workDir>/.lite-harness/`**（`SessionStore::rootDirFor`）——是会话工作目录下的相对根，**不是**用户主目录。
- 带 sessionDataId 时隔离到 `sessions/<id>/`（history.json、.task、.memory、.transcripts、scheduled_tasks.json 等）；`skills/` 始终跨会话共享。
- QSettings：org/app 均为 `LiteHarness`（App.cpp 设定）→ 注册表 `HKCU\Software\LiteHarness\LiteHarness`。
- **零线程原则:** 全仓库主线程事件驱动，轮询/异步一律 QTimer + QProcess 信号，不起线程。

## 主题 / QSS

- 三套主题 `light` / `dark` / `atomOneDark`；QSS 位于 `stylesheet/<theme>/<Widget>.qss`，打包为 `:/stylesheet/`（调试期回退 `../stylesheet/`，解析逻辑在 FluentUI 的 FluStyleSheetUtils）。
- 组件接主题：构造函数**末尾**调用 `ThemeAware::bind("X.qss", widget, extraRefresh)`；widget 作为连接 context 自动随析构断开；extraRefresh 处理 SVG 重着色等组件特有步骤。
- **新增带 QSS 的组件:** 放置 `src/*.cpp/.h` 与三主题目录下同名 `.qss`，构造尾 bind 即可 — GLOB 自动拾取，**不改 CMakeLists.txt**。

## 关键约定

- 导航键统一取自 `NavItem.h::NavKey`；页面注册 `m_sLayout->addWidget(key, page)`，导航项用同一键。
- 魔法数字进 `AgentConstants.h`（agent 参数）或 `LayoutConstants.h`（布局尺寸），不散落字面量。
- 工具侧失败折叠为输出字符串交还 LLM，不抛异常、不弹窗。
- lcc 移植规格权威：源码注释以 `lcc sXX + hash` 标注对应阶段（s03–s12），参照仓库 `3rdparty/lcc`（不入构建）。
- FluentUI 头文件 `<FluUtils.h>`、`<FluThemeUtils.h>` 等位于 `3rdparty/FluentUI/{controls,utils}`。

## Git 提交
采用中文日志
