# LiteHarness 模块结构总览

> **本文定位**：`AGENTS.md` 记「规则与坑」（怎么构建、哪些值不许散落、什么改动会踩雷），本文记
> 「结构与依赖边」（有哪些层、每个模块负责什么、一条消息怎么流过它们）。两者冲突时以代码与
> `AGENTS.md` 为准。
>
> **更新纪律**：新增/删除 `src/` 模块、改 `AgentLoop*` 各 TU 的分工、改 `AgentLoopInternal.h`
> 的符号归属，都必须同步本文对应表格。结构文档一旦失真比没有更有害。
>
> 所有结论均来自当前工作树代码，标注 `文件:行号` 可回查。

---

## 1. 分层与依赖方向

`src/` 是**扁平目录**（无子目录），分层是逻辑的不是物理的——靠 include 关系与注释纪律维持。

```
L1 应用壳        App · LiteHarness · SessionRegistry
                        ↓
L2 页面          BasePage · NewChatPage · ChatSessionPage · SettingsPage
                        ↓
L3 展示控件      MessageBubbleWidget · CollapsibleBlock(→ThinkingBlock/ToolBlock/TodoCard)
                 PermissionCard · SessionSidebar · ChatMsgEdit · SendMsgButton
                 WorkDirPathBar · FluentInputDialog · NavItem
                        ↓
L4 Agent 编排    AgentLoop（1 类 × 12 TU）· SubAgent
                        ↓
L5 引擎/传输     QOpenAi · BashRunner · CompactManager · MemoryManager · TaskStore
                 CronSchedulerManager · BackgroundTasksManager · SessionStore
                        ↓
L6 单源与设施    AgentConstants.h · ToolNames.h · LayoutConstants.h · AppSettings.h
                 I18n · ThemeAware · ToolTagKind.h · AgentLoopInternal.h
```

**两条铁律（本仓最重要的两条边方向）**

1. **L5/L6 零 GUI 依赖**：引擎层不 include 任何 widget 头，`CompactManager`/`MemoryManager`/
   `TaskStore`/`CronSchedulerManager` 甚至不是 QObject 或不发业务信号，宿主信息一律经**构造注入
   的 sink/回调**传入（例：`AgentLoop.cpp:39-42` 把 `sessionDataRoot()` 注成四个引擎的 workDir sink
   的 `[this]{ return sessionDataRoot(); }` 惰性 lambda）。
2. **不反向依赖**：引擎绝不 include `AgentLoop.h`；页面绝不 include 引擎内部头（唯一例外见 §9）。

---

## 2. 分层清单

### L1 应用壳与启动

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `App.cpp` | `main()`：装 translator、显示主窗、退出码 931 只透传（严禁二次拉起） | `I18n`、`LiteHarness` |
| `LiteHarness` | 主窗口（`FluFrameLessWidget`）：导航 + 堆叠装配、会话新建/恢复/重命名/删除、主题语言联动、覆盖 FluentUI 取色的 `appendOwnSheetOverride` | 全部页面、`SessionRegistry`、`SessionStore`、`QOpenAi`（配置初始化）、`NavItem`、`ThemeAware` |
| `SessionRegistry` | 导航 key ↔（页 / dataId / 导航子项）反查表 + 右键监听映射；只持 `QPointer`，零所有权 | `NavItem.h`（`NavKey` 单源）；`.cpp` 需完整 `ChatSessionPage` 类型（见 §9 异常 1） |

### L2 页面

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `BasePage` | 导航页基类；主题契约：基类构造不首刷，派生页构造尾 `ThemeAware::bind` | `ThemeAware` |
| `NewChatPage` | 发起页（欢迎语 + 工作目录条 + 输入框），发 `newChatRequested(text)`；进入时重读 `settings.ini` 默认目录 | `BasePage`、`ChatMsgEdit`、`WorkDirPathBar`、`AppSettings`、`LayoutConstants` |
| `ChatSessionPage` | 每会话一页：布局 + **持有 `AgentLoop`** + `wireAgent()` 把全部后端信号接成 UI + 历史重放 + 侧栏接线 | `AgentLoop`、**`AgentLoopInternal.h`**（`ChatSessionPage.cpp:14`）、`MessageBubbleWidget`、`ToolBlock`、`PermissionCard`、`TodoCard`、`SessionSidebar`、`ChatMsgEdit`、`WorkDirPathBar`、`CompactManager` |
| `SettingsPage` | 主题/语言/默认工作目录/模型服务 `apiBaseUrl`+`apiToken`/上下文上限/轮次上限 | `BasePage`、`QOpenAi`（直改运行时配置）、`AppSettings`、`AgentConstants`、`I18n`、`FluentInputDialog` |

### L3 展示控件

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `MessageBubbleWidget` | 一条消息气泡：流式打字机 + 纵向时间线（思考块/工具卡/权限卡/记忆进度按到达顺序内嵌） | `ThinkingBlock`、`ToolBlock`、`LayoutConstants` |
| `CollapsibleBlock` | 折叠骨架基类：手动几何头部 + `contentHeight` 属性动画（300ms OutCubic）+ live 圆点轮播；构造禁调虚函数 | `ThemeAware` |
| `ThinkingBlock` / `ToolBlock` / `TodoCard` | 思考折叠块 / 工具执行卡（终态 + 3 种 live 形态）/ 任务清单时点快照卡 | `CollapsibleBlock`；`ToolBlock` 另用 `ToolTagKind`、`ToolNames`、`AgentConstants` |
| `PermissionCard` | 权限审批卡：待决主体 → 单行「已允许/已拒绝」留痕；发 `userResolved(bool)` | `ThemeAware`、`ToolTagKind` |
| `SessionSidebar` | 会话右栏信息面板（标题/模型/上下文占用/状态灯/任务清单）；**纯视图，不订阅 `AgentLoop`**，全部由 `ChatSessionPage` 调 setter 推入 | `ThemeAware`、`LayoutConstants` |
| `ChatMsgEdit` / `SendMsgButton` | 底部输入框（`sendMessage` / `stopRequested` / `modelChanged` + `setTurnBusy`）/ 圆形发送-停止两态钮 | `AgentConstants`、`LayoutConstants`、`ThemeAware` |
| `WorkDirPathBar` / `FluentInputDialog` / `NavItem` | 只读路径条（中间省略 + tooltip 全路径）/ Fluent 风格单行模态框 / 导航项最小派生（补 `removeChildItem`）+ `NavKey` 常量 | 仅 Qt / FluentUI |

### L4 Agent 编排

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `AgentLoop` | 回合状态机：`run()` → 召回 → 压缩前导 → 流式请求 → 工具批串行推进 → 终局落盘与记忆沉淀；**值成员**持有并编排全部 L5 引擎 | `QOpenAi`、`CompactManager`、`MemoryManager`、`TaskStore`、`CronSchedulerManager`、`BackgroundTasksManager`、`SubAgent`、`BashRunner`、`ToolNames`、`AgentConstants` |
| `SubAgent` | `task` 子代理：独立 messages / ChatStream / 工具队列 / 权限槽的黑盒，唯一出口是完成汇总文本 | `AgentLoop`（`friend`）、`QOpenAi`、`AgentLoopInternal.h`、`BashRunner`、`ToolNames` |

### L5 引擎 / 传输（零 GUI）

| 模块 | 职责 | 依赖 |
|---|---|---|
| `QOpenAi` | OpenAI 兼容 SSE 客户端：`ChatStream`（`thinkingDelta`/`textDelta`/`messageFinished`/`error`）+ `AsyncRequest`（一次性文本请求，`done` 恒恰好一次）+ 运行时 url/token/model 配置 | `AppSettings`、`AgentConstants` |
| `BashRunner` | bash 执行段单源：`dangerWarning` / `truncateOutput` / `finalizeOutput` / `start`（`powershell.exe -NoProfile -NonInteractive`） | `AgentConstants` |
| `CompactManager` | 五级压缩管线 `toolResultBudget → snip → micro → fitToolResults → compactHistory` + 溢出反应式压缩；非 QObject，全靠回调注入 | `QOpenAi::AsyncRequest`、`AgentConstants` |
| `MemoryManager` | 持久记忆：召回 / 提取 / 合并三条异步链，落 `.memory/MEMORY.md` + `<slug>.md` | `QOpenAi`、`AgentConstants` |
| `TaskStore` | 任务图文件存储：一任务一 `.task/task_<hex8>.json`，每操作直读盘；6 个 `run_*` 纯文本进出 | `AgentConstants` |
| `CronSchedulerManager` | 定时任务台账：cron 校验/匹配、到期队列、`scheduled_tasks.json` 持久化、两段式 at-least-once 交付；`QTimer` 1s 轮询不建线程 | 仅 QtCore |
| `BackgroundTasksManager` | 后台 bash 任务台账：登记 / 存结果 / 渲染 `<task_notification>`，进程由宿主驱动 | `ToolNames` |
| `SessionStore` | 全局索引 `<workDir>/.lite-harness/index.json` 读写 + 条目 upsert（`QSaveFile` 原子写）；纯静态 header-only（无 `.cpp`，方法全在类内定义即隐式 inline） | 仅 QtCore |

### L6 单源与设施

| 模块 | 单一事实源 |
|---|---|
| `AgentConstants.h` | 模型清单、`kMaxTokens`、bash 超时与错误文案、输出截断、上下文上限默认/校验界、glob 上限、**中间目录名**（`.task`/`.temp`/`.transcripts`/`.memory`，`AgentConstants.h:130-135`，拼法涉数据兼容不可改） |
| `ToolNames.h` | 18 个工具名（bash/read_file/write_file/edit_file/glob/todo_write/task/load_skill/compact/create_task/update_task/list_tasks/get_task/claim_task/complete_task/schedule_cron/list_crons/cancel_cron） |
| `LayoutConstants.h` | 聊天栏宽 800 / 边距 35 / 气泡系数 0.75 / 侧栏宽 280 |
| `AppSettings.h` | 配置存储：一律 `applicationDirPath()/settings.ini`（弃用注册表），键清单见 `AppSettings.h:3-6`（`defaultWorkDir`/`sidebarVisible`/`language`/`contextCharLimit`/`maxToolIterations`/`apiBaseUrl`/`apiToken`） |
| `ToolTagKind.h` | 工具名 → 语义类别（read/search/plan/delegate/run/write/other）→ QSS `toolTagKind` 动态属性 |
| `I18n.h` | 中英文切换：源文中文 + Linguist + **重启生效**（`exit(931)` 自重启） |
| `ThemeAware.h` | 「加载 QSS + 订阅 themeChanged + 重载」样板单源 `bind(qss, widget, extraRefresh)` |
| `NavItem.h` | `NavKey` 导航/堆叠键单源 |
| `AgentLoopInternal.h` | `AgentLoopDetail` 跨 TU 内部工具**声明**单源（§4） |

---

## 3. AgentLoop 家族：单类多 TU 的职责地图

`AgentLoop.h` 是**唯一类声明**；12 个 `.cpp` 各承载一组成员方法定义（不是多个类）。这样既保住了
「一个状态机一个所有者」的语义，又让单文件回到可读体量。**新增方法请按 §10 的决策树落位，不要
往 `AgentLoop.cpp` 里堆。**

| TU | 行 | 负责什么 | 关键定义 |
|---|---|---|---|
| `AgentLoop.cpp` | 331 | 生命周期与会话身份：构造/析构、workDir / sessionDataId / sessionDataRoot / model、回合入口与终局 | `run()`、`stop()`、`sessionDataRoot()`（`:131-136`） |
| `AgentLoopRequest.cpp` | 412 | 一次 LLM 请求回合：压缩前导 → 流式请求 → 工具批推进 → 记忆沉淀链 | `startChatRequest`、`doStartChatRequest`、`applyCompactPipelineAsync`、`continueWithToolResults`、`runNextTool`、`startMemoryChain` |
| `AgentLoopPrompt.cpp` | 374 | system prompt 六段组装 + 每轮刷新首位 system + 工具 function 定义（**禁翻区**，`QStringLiteral` 不包 `tr()`） | `makeSystemPrompt`、`rebuildSystemPromptMessage`、`createToolsDefinition` |
| `AgentLoopTools.cpp` | 315 | 工具分发：`executeTool` 分流、handler 表、统一收口 `onToolFinished`、成败判定单源 | `executeTool`、`onToolFinished`、`isToolFailure`、`AgentLoopDetail` 工具段定义 |
| `AgentLoopFileTools.cpp` | 288 | 沙箱文件工具（同步本地 IO）：逃逸判定 + read/write/edit/glob，全静态，宿主与子代理各传各的 workDir | `safePathIn`、`runReadFileIn`、`runWriteFileIn`、`runEditFileIn`、`runGlobIn` |
| `AgentLoopBash.cpp` | 159 | bash 异步执行链与后台任务结果收割 | `executeBashAsync`、`injectBackgroundResults` |
| `AgentLoopPermission.cpp` | 142 | 权限门：硬拒绝黑名单、破坏性命令升级判定、ASK 规则、用户裁决续跑 | `checkDenyList`、`checkPermissionRules`、`resolvePermission`、`AgentLoopDetail::bashDenyList` 定义 |
| `AgentLoopHooks.cpp` | 163 | 生命周期钩子注册表：注册顺序即执行顺序，四个 trigger（UserPromptSubmit/PreToolUse/PostToolUse/Stop）**首个非空返回短路** | `registerBuiltinHooks`（内置：`context_inject` / `permission` / `log_before` / `log_after`）、四个 `trigger*Hooks` |
| `AgentLoopHistory.cpp` | 174 | `history.json` 落盘（含索引 `lastActiveMs` 续活）与磁盘恢复；条目登记归 `LiteHarness::createSession` | `persistHistory`、`loadSavedHistory` |
| `AgentLoopSkills.cpp` | 196 | 技能扫描与 `load_skill`；技能目录**始终跨会话共享** `<workDir>/.lite-harness/skills` | `scanSkills`、`skillsCatalog`、`runLoadSkill` |
| `AgentLoopTodo.cpp` | 119 | `todo_write`（无状态：只校验入参并渲染面板文本，成功即以本次快照发 `todoUpdated`） | `renderTodos`、`runTodoWrite` |
| `AgentLoopSubAgent.cpp` | 86 | `task` 子代理的启动与统一收口（子代理信号直连转发为主循环信号） | `startSubAgentTask`、`cancelSubAgent` |
| `AgentLoopCron.cpp` | 64 | 三个 cron handler 与空闲边界交付；台账与匹配在 `CronSchedulerManager`，本文件只做宿主侧接线 | `runScheduleCron`、`runCancelCron`、`runListCrons`、`tryDeliverCron` |

**AgentLoop 对外信号面**（`AgentLoop.h:85-132`，UI 只认这些）：`thinkingDelta`、`textDelta`、
`toolStarted`、`toolOutputReady(…, ok)`、`subagentProgress`、`permissionRequired`、`todoUpdated`、
`memoryPhaseStarted`、`memoryChainFinished`、`scheduledUserMessage`、`finished`、`runningChanged`、
`error`。另有一组**私有驱动信号**（`startChatRequest` / `doStartChatRequest` /
`continueWithToolResults` 等，`AgentLoop.h:144` 起）只用于把阻塞链拆成事件驱动续跑，不对外、
UI 不得连接。

---

## 4. AgentLoopInternal.h：内部工具声明单源

`src/AgentLoopInternal.h`（59 行）声明 `namespace AgentLoopDetail` 的跨 TU 纯函数。**它不是对外
API**：只供 `src/AgentLoop*.cpp` 与两个友元 TU（`SubAgent.cpp`、`ChatSessionPage.cpp`）使用，其它
模块不得依赖其符号稳定性。

**归属规则（谁定义）**：原 `AgentLoop.cpp` 匿名 namespace 里被多单元共用的函数，按「谁最贴近其
语义谁定义」落位，本头只负责声明——

- `toolSummary` / `callToolName` / `callToolArgsText` / `parseToolCall` / `parseToolArgsText` /
  `callToolArgs` / `ToolCallView` → 定义在 **AgentLoopTools.cpp**（工具分发侧）
- `askPrefix` / `bashDenyList` → 定义在 **AgentLoopPermission.cpp**（权限门侧；`bashDenyList`
  三方共用：`checkDenyList` + 主循环 `executeBashAsync` + 子代理）

**三条禁令**

1. 不要再加 `AgentLoop::toolSummaryOf()` 这类**静态转发层**——曾经有（连同 `askPrefixOf` /
   `bashDenyList`），已删除，友元直接 include 本头取用（`AgentLoop.h:179-181` 记录了这次演进）。
2. 本头**不 include `AgentLoop.h`**：include 链单向，只含 QtCore 头，避免 UI/子代理为了一个摘要
   函数被迫吃下整个类。
3. 声明与定义不得重复：新增符号时同步更新本头 + 归属 TU + 本节说明，三处一起改。

---

## 5. 数据流：一条用户消息的完整往返

```
① 输入侧（UI → 内核）
ChatMsgEdit::sendMessage(text)
  → ChatSessionPage 构造内 lambda
      · 若 m_agentLoop->isRunning()：dismissPendingPermission() + 就地插一条 *Error:* 气泡并返回
      · addMessage(Role::User, text)
      · startAssistantStream(text)：new MessageBubbleWidget(Assistant) + startStreaming("处理中…")
                                    + AgentLoop::run(text)
（旁路：ChatMsgEdit::stopRequested → ChatSessionPage::stop → AgentLoop::stop()；
        ChatMsgEdit::modelChanged → AgentLoop::setModel）

② 回合入口 AgentLoop::run（AgentLoop.cpp:175）
setRunning(true)（同步红线，先于任何异步）→ 快照 m_maxToolIterations → triggerUserPromptSubmitHooks
→ user 消息入 m_messages → injectBackgroundResults() → m_memory.loadMemoriesAsync(...)
→ done 里 rebuildSystemPromptMessage() + startChatRequest(messagesJson)

③ 请求链（AgentLoopRequest.cpp）
startChatRequest → applyCompactPipelineAsync（仅触发全量压缩时挂侧链，句柄落 m_sideRequest）
→ doStartChatRequest：组 body（model / messages / tools=createToolsDefinition() /
  enable_thinking / reasoning_effort / temperature / top_p / max_tokens）
→ QOpenAi::chat().createStream(request, this)
   · ChatStream::thinkingDelta → 直连转发 AgentLoop::thinkingDelta
   · ChatStream::textDelta     → 直连转发 AgentLoop::textDelta
   · ChatStream::messageFinished(fullMsg) → 无 tool_calls：triggerStopHooks → cron 交付定稿
       → setRunning(false) → persistHistory() → emit finished → emit memoryPhaseStarted
       → startMemoryChain（异步沉淀，终态发 memoryChainFinished）
   · ChatStream::error(msg) → cancelSubAgent →（可压缩则反应式压缩重试）→ emit error

④ 工具批（continueWithToolResults → runNextTool → executeTool）
parseToolCall（非法即 onToolFinished 回填错误文本）→ compact 前部特判
→ triggerPreToolUseHooks（内置 permission 钩子在此判定）→ 三分流：
   · "ASK:" 前缀 → m_awaitingPermission=true + emit permissionRequired + **return，队列暂停**
   · 硬拒绝文本 → onToolFinished（错误结果回填，不发 toolStarted）
   · 放行 → emit toolStarted → bash 走 executeBashAsync（run_in_background 走后台台账）/
            task 走 startSubAgentTask / 其余查 handlers 表同步执行
   → triggerPostToolUseHooks → onToolFinished：emit toolOutputReady(name, summary, output,
     ok=!isToolFailure(output)) → 构造 role=tool 消息入 m_toolResultsReady → runNextTool 续跑
批尾：results 回填历史 → injectBackgroundResults → <reminder> 并入末条 tool content
→ 需要则 compactHistoryAsync → applyCompressedConversation → 回 startChatRequest（下一轮）

⑤ 权限回路
AgentLoop::permissionRequired(name, summary, reason)
  → ChatSessionPage::wireAgent：setPermissionPending(true) + new PermissionCard
     （有当前气泡则 appendPermissionCard，否则挂滚动区主布局）
  → PermissionCard::userResolved(allow) → setPermissionPending(false) + AgentLoop::resolvePermission(allow)
子代理同源：SubAgent::permissionRequired → 直连转发为 AgentLoop::permissionRequired

⑥ 子代理进度
SubAgent::progressEmitted(turnNo, toolName, summary) → 直连 AgentLoop::subagentProgress
  → ChatSessionPage → MessageBubbleWidget::appendSubagentProgress → ToolBlock（行数上限
     AgentConst::kSubagentProgressMaxLines）；task 终态仍由 toolOutputReady("task") 唯一收口
```

**回填 UI 总表**（全部集中在 `ChatSessionPage::wireAgent()`，别处不接 AgentLoop 信号）

| AgentLoop 信号 | 连接行 | 落到 |
|---|---|---|
| `finished` | `:189` | 兜底 `addMessage`（无气泡时）+ 侧栏权限态复位 + `refreshContextUsage()` |
| `error` | `:206` | `dismissPendingPermission()` + `finishStreaming()` + 一条 `*Error:* %1` |
| `thinkingDelta` / `textDelta` | `:222` / `:229` | 当前气泡 `appendThinkingText` / `appendText` |
| `toolStarted` | `:267` | `appendToolStart`（事前 live 卡） |
| `toolOutputReady` | `:239` | `appendToolExecution`；`ok && (write_file\|edit_file)` → 侧栏 `recordModifiedFile`；`memory` 有 M1 静默闸 |
| `subagentProgress` | `:278` | `appendSubagentProgress` |
| `permissionRequired` | `:321` | `PermissionCard` + 侧栏状态灯 |
| `todoUpdated` | `:350` | `TodoCard` 快照 + 侧栏任务清单 |
| `memoryPhaseStarted` / `memoryChainFinished` | `:293` / `:308` | 记忆进度 live 卡的挂出与收尾 |
| `scheduledUserMessage` | `:380` | 以「定时任务用户消息」入历史并直接发起新一轮 |
| `runningChanged` | `:393` | 输入框忙态 `setTurnBusy` + 侧栏状态灯；终局兜底复位审批灯（`setRunning(true)` 起即禁输入，早于 `run()` 卫兵） |

---

## 6. 运行时数据目录布局

根是**会话工作目录**下的相对路径，不是用户主目录（`SessionStore::rootDirFor`，`SessionStore.h:22-25`）。

```
<workDir>/
└── .lite-harness/                        ← 全局根（SessionStore.h:25）
    ├── index.json                        ← 会话索引（SessionStore，QSaveFile 原子写）
    ├── skills/<name>/SKILL.md            ← 技能，始终跨会话共享（AgentLoopSkills.cpp:18）
    └── sessions/<dataId>/                ← 会话数据根 = AgentLoop::sessionDataRoot()
        ├── history.json                  ← 会话历史（AgentLoopHistory.cpp:36）
        ├── .memory/MEMORY.md + <slug>.md ← 记忆索引与记录（MemoryManager）
        ├── .task/task_<hex8>.json        ← 任务图，一任务一文件（TaskStore）
        ├── .transcripts/transcript_*.jsonl ← 压缩前完整转写（CompactManager）
        ├── .task_outputs/tool-results/   ← 超大工具输出卸载（AgentConst 名单源）
        ├── .temp/                        ← prompt 引导语指向的临时目录（AgentLoopPrompt.cpp:20-22）
        └── scheduled_tasks.json          ← cron 台账（CronSchedulerManager.cpp:554）
```

- **回退语义**：`sessionDataId` 为空时 `sessionDataRoot()` 返回 `<workDir>/.lite-harness`（
  `AgentLoop.cpp:133-136`），保证未注入 ID 的独立构造路径行为不变；空历史不落盘以免污染全局根
  （`AgentLoopHistory.cpp:23`）。
- 目录名一律取 `AgentConst::k*DirName`（`AgentConstants.h:130-135`），拼法涉既有数据兼容，**逐字符
  不可改**；`.lite-harness` 中间层只在 `sessionDataRoot()` 拼一次，各引擎只拼自己的叶子段。
- 用户配置在**另一处**：`<exe 目录>/settings.ini`（`AppSettings.h:19`，弃用注册表）。

---

## 7. 构建与打包（结构视角，细节见 AGENTS.md）

- 根 `CMakeLists.txt:44` 的 `file(GLOB src CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` 一次收走全部
  源文件——**新增 TU 不需要改任何 CMake 文件**（本次 AgentLoop 拆分新增 13 个文件即零 CMake 改动
  即证）。`add_subdirectory(src)` 已注释，不存在 src 级 CMakeLists。
- 只有一个可执行目标 `lite-harness`，链 `FluentUI::Controls/Utils` + `Qt6::Network`；
  `3rdparty/FluentUI` 是子目录，`3rdparty/sqlite*` 与 `3rdparty/lcc` **不进构建图**。
- 应用数据全部内嵌资源：QSS → `:/stylesheet/`、图标 → `:/res/`、翻译 qm → `:/i18n/`。
  因此出包 = exe + Qt 运行时 + VC 运行库（CPack ZIP 唯一路径）。
- CI（`.github/workflows/Windows-Qt6.9.0.yml`）**没有测试/lint 步骤**：干净环境全量 configure +
  Release 全目标构建 + cpack 出 zip 即验收；tag `v*`/`s*` 末尾上传 GitHub Release。
  必须全目标构建（勿 `--target lite-harness`），原因见 AGENTS.md「构建」节。

---

## 8. 单源纪律一览（想改一个值，只改一处）

| 要改的东西 | 唯一入口 |
|---|---|
| 工具名 | `ToolNames.h`（schema / handler 表 / executeTool 特判 / toolSummary / 钩子比较 / 子代理白名单 / ToolBlock 标题全部引用它） |
| 工具摘要口径（实时与历史回放必须一致） | `AgentLoopDetail::toolSummary`（`AgentLoopTools.cpp`） |
| 工具成败判定（UI 显形与历史折叠同源） | `AgentLoop::isToolFailure`（`AgentLoopTools.cpp`） |
| bash 危险黑名单 | `AgentLoopDetail::bashDenyList`（`AgentLoopPermission.cpp`，三方共用） |
| 数据目录名 / 魔法数 / 模型清单 | `AgentConstants.h` |
| 布局尺寸（多页同列对齐） | `LayoutConstants.h` |
| 配置键与文件位置 | `AppSettings.h` |
| 工具配色类别 → QSS | `ToolTagKind.h` + `stylesheet/<theme>/*.qss` |
| 主题化样板 | `ThemeAware::bind`（禁止再复制「读 QSS + 订阅 themeChanged」三件套） |
| 版本号 | CMake `project VERSION` → `LITE_VERSION` 宏 |
| 导航/堆叠键 | `NavItem.h::NavKey` |

---

## 9. 已知分层异常与技术债（登记在案，勿默默踩）

1. **`SessionRegistry.cpp` 反向 include `ChatSessionPage.h`**：头文件里只前向声明，`.cpp` 需完整
   类型才 include，形成 L1→L2 的回边。可接受（无循环 include），但新增跨层引用前先看这里。
2. **`ChatSessionPage.cpp:14` 直接 include 内核非公开头 `AgentLoopInternal.h`**：靠
   `friend class ChatSessionPage`（`AgentLoop.h:140`）授权，目的是历史回放与实时链路共用同一
   `toolSummary` 口径。**这是有意为之的例外**，别据此开「UI 可以吃内核内部头」的先例。
3. **`SettingsPage` 直改全局运行时配置**（`QOpenAi::setUrl/setToken`）：设置页 → 引擎的直连边，
   生效语义见 `SettingsPage.h` 注释。
4. **仍偏大的文件**（下一批可读性优化的候选，按体量排序）：`MemoryManager.cpp` 1116、
   `MessageBubbleWidget.cpp` 836、`ChatSessionPage.cpp` 722、`TaskStore.cpp` 684、
   `QOpenAi.cpp` 668、`CompactManager.cpp` 659、`SettingsPage.cpp` 652、
   `CronSchedulerManager.cpp` 631。
5. **无自动化测试**：`tests/` 不存在，`CMakeLists.txt` 无 `add_test`/`enable_testing`，CI 不跑测试。
   因此重构的验证手段只有「编译期等价 + 冒烟运行 + 移动代码逐字一致」，行为回归**不可证**。
   若要补，最小切口是给已无 GUI 依赖的纯函数（`AgentLoopDetail::toolSummary` / `parseToolCall` /
   `BashRunner::dangerWarning` / `isToolFailure`）建一个 `Qt6::Test` 单测目标。

---

## 10. 新增代码该落在哪（决策树）

**新增一个 AgentLoop 成员方法** →
① 它属于哪条链路？请求/压缩前导 → `AgentLoopRequest.cpp`；提示词或工具 schema → `AgentLoopPrompt.cpp`；
分发或收口 → `AgentLoopTools.cpp`；文件沙箱工具 → `AgentLoopFileTools.cpp`；bash/后台任务 →
`AgentLoopBash.cpp`；权限判定 → `AgentLoopPermission.cpp`；钩子 → `AgentLoopHooks.cpp`；
历史读写 → `AgentLoopHistory.cpp`；技能 → `AgentLoopSkills.cpp`；todo → `AgentLoopTodo.cpp`；
子代理 → `AgentLoopSubAgent.cpp`；cron → `AgentLoopCron.cpp`；生命周期/会话身份 → `AgentLoop.cpp`。
② 有 ≥2 个 TU 共用的纯函数？→ 声明进 `AgentLoopInternal.h`，定义落「语义最贴近」的那个 TU，**不加转发层**。
③ 只在单 TU 内用？→ 留在该 `.cpp` 的匿名 namespace，别上头文件。

**新增一个引擎（无 GUI 依赖）** → L5：自带 `.h/.cpp`，宿主信息经构造注入 sink/回调，不 include
`AgentLoop.h`；GLOB 自动纳入构建。
**新增一个带样式的控件** → L3：`src/*.cpp/.h` + 三主题 `stylesheet/<theme>/<Widget>.qss`，构造尾
`ThemeAware::bind`；不改 `CMakeLists.txt`。
**新增一个用户可见设置项** → `AppSettings.h` 加键 + `SettingsPage` 加卡 + 取值经 `AgentConstants.h`
的取值函数（参考 `contextCharLimitValue()` 的「默认值 + 校验界 + 单点读取」三件套）。
