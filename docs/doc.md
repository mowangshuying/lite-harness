# lite-harness 文档总集

> 本文件是 `docs/` 下**唯一**的文档：原六篇（`README.md` / `architecture.md` / `regression-checklist.md` /
> `lite-harness交互 UI 设计逻辑.md` / `async-chain-design.md` / `chat-session-design.md`）已按权威等级
> 合并至此。**规则与坑**（构建 / 测试 / 数据路径 / 主题 / i18n / 发布）不在本文，在
> [`../AGENTS.md`](../AGENTS.md)。

---

<a id="index"></a>
## 第一部分 · 文档索引与同步纪律

### 1.1 各部分权威等级

| 部分 | 定位 | 状态 | 与代码的关系 |
|---|---|---|---|
| [第二部分 · 模块结构总览](#architecture) | **结构与依赖边**：六层清单、AgentLoop 家族 17 个 TU 的职责地图、一条消息的完整数据流、运行时目录布局、单源纪律一览、已知分层异常、新增代码落位决策树 | 活文档（权威） | 增删 `src/` 模块、改各 TU 分工、改 `AgentLoopInternal.h` 符号归属必须同步 |
| [第三部分 · 手工冒烟回归清单](#checklist) | **历轮验收场景沉淀**：十一组 P0/P1/P2 勾选项 | 活文档（发版必跑） | 每条对应一个已实现行为；实现变了要改条目，**只记现状、不记愿望** |
| [第四部分 · 交互 UI 设计逻辑](#ui) | **交互设计原理**：折叠式渐进披露的公共骨架与各类卡片（思考块 / 工具卡 / 权限卡 / 待办卡 / 子代理进度 / 记忆与压缩卡 / 侧栏 / 历史回放）为什么这么设计 | 活文档（设计说明） | 只讲原理与契约，不逐控件枚举 API；新增一类卡片/一种进行态时补一节，别在源码注释里另写一套原理 |
| [第五部分 · 异步链现状契约](#async) | **memory / compact 侧链的现行契约**：全异步不变量、`AsyncRequest` 语义、三条记忆链与压缩变体、`stop()` 收口顺序、UI 侧接线 | 活文档（改侧链契约必须同步） | 原「异步化实施规格」已落地，其迁移前现状表 / API 草案 / 分阶段计划 / 工作量估算**已删除**（正文行号锚定拆分前的旧 2063 行 `AgentLoop.cpp`，100% 不可回查）；历史决策与偏离登记只留 §7 |

别名（下文引用一律用别名）：**结构篇** = 第二部分，**清单篇** = 第三部分，**交互篇** = 第四部分，
**异步篇** = 第五部分。

> **已移除的部分**：原第六部分「早期会话页草图」（合并时即标注已过时，组件面与交互面均与现实现不符：
> 无气泡左对齐 / `clearMessages` 清空 / 只 3 个新文件，与今天的 `MessageBubbleWidget` 时间线、
> `startNewSession` 换页、40 个源文件完全对不上）已于本轮删除，设计演化留痕看 git 历史，不在本文。

### 1.2 该读哪部分（按问题查）

| 我想… | 去 |
|---|---|
| 编译、跑单测、出 zip | `AGENTS.md`「构建」「测试（tests/ + ctest）」 |
| 知道某个类在哪一层、允许 include 什么 | 结构篇 §1–§2 |
| 给 `AgentLoop` 加一个方法，不知道该落哪个 `.cpp` | 结构篇 §10 决策树 |
| 追一条用户消息从输入框到气泡的完整链路 | 结构篇 §5 |
| 找会话数据落在磁盘哪个文件、目录名能不能改 | 结构篇 §6 + `AGENTS.md`「数据与路径」 |
| 改一个阈值，先确认它是不是单源 | 结构篇 §8 单源纪律一览 |
| 提交前该跑哪些验收场景 | 清单篇（按改动面选组） |
| 理解工具卡 / 思考块为什么折叠、何时展开 | 交互篇 §1–§6 |
| 加一类新卡片（权限 / 待办 / 进度 / 结果卡）该怎么挂 | 交互篇 §7–§13 |
| 发版、打 tag、写 Release 简介 | `AGENTS.md`「发布（tag 与 Release）」 |
| 侧链能不能回退成阻塞写法、`stop()` 为什么要按那个顺序收口 | 异步篇 §1 / §5；历史决策与偏离登记看异步篇 §7 |

> **`§n` 的口径**：各部分保留自己原来的小节编号，`§n` **恒指同一部分内**的第 n 节；跨部分引用一律写成
> 「结构篇 §5」「异步篇 §2」这种带部分名的形式，不存在全局连续编号。

### 1.3 同步纪律

1. **冲突优先级：代码 > `AGENTS.md` > 结构篇 > 其余。** 发现文档与代码矛盾，以代码为准并**当场修文档**——
   只写一句「以代码为准」了事，正是失真得以积累的原因。
2. **行号锚点会腐化。** 本文大量结论标注 `文件:行号`，改动源文件后凡被触及的锚点都要重测。反面样本是原
   异步化实施规格：一次拆分（旧 `AgentLoop.cpp` 2063 行 → 13 个 TU）让它全文的行号锚点 100% 失效，长期
   靠"只读 §0/§8、别读行号"的免责声明续命。**现已按当前代码重写为异步篇**——写结论时优先锚**符号名**
   （类/方法/常量），只在顺序本身是契约时才锚行号（如异步篇 §5 的 `stop()` 收口顺序）。
3. **版本号是示例，也要跟着同步。** `AGENTS.md` / 本文（结构篇、清单篇）与若干源码注释把当前版本
    （`s18.2`）当示例写死；升版本时随 `git grep` 逐处改，做法见 `AGENTS.md`「发布」节第一条。
    **但 `s17` 在本仓有两个语境**：lcc 移植阶段名（`3rdparty/lcc` 子模块 tag 即 `s17`，属历史台账、
    永不随版本 bump 改）与当前版本示例（随 bump 改）。s17.1 起二者不再同名，grep 到 `s17` 须先判语境。
4. **零散笔记不在 `docs/` 落地。** 临时方案、验收记录、会话草稿一律写到
   `.lite-harness/sessions/<id>/.temp/`；收口时把仍然成立的内容折进本文对应部分、过时者删除，
   不在 `docs/` 里另立 md 文件——孤立文件既无人引用、也无人负责同步（本目录刚刚就是因为六篇各自漂移
   才合并成一份的）。
5. **新增内容优先加进已有部分。** 确需新开一部分时：给它一个 ASCII 锚点 id、在 §1.1 权威等级表登记一行、
   并从 `AGENTS.md` 或根 `README.md` 建一条入口链接。没有入口的文档等于不存在。

---

<a id="architecture"></a>
## 第二部分 · LiteHarness 模块结构总览（结构与依赖边）

> **本部分定位**：`AGENTS.md` 记「规则与坑」（怎么构建、哪些值不许散落、什么改动会踩雷），本部分记
> 「结构与依赖边」（有哪些层、每个模块负责什么、一条消息怎么流过它们）。两者冲突时以代码与
> `AGENTS.md` 为准。
>
> **更新纪律**：新增/删除 `src/` 模块、改 `AgentLoop*` 各 TU 的分工、改 `AgentLoopInternal.h`
> 的符号归属，都必须同步本部分对应表格。结构文档一旦失真比没有更有害。
>
> 所有结论均来自当前工作树代码，标注 `文件:行号` 可回查。

---

### 1. 分层与依赖方向

`src/` 是**扁平目录**（无子目录），分层是逻辑的不是物理的——靠 include 关系与注释纪律维持。

```
L1 应用壳        App · LiteHarness · SessionRegistry
                        ↓
L2 页面          BasePage · NewChatPage · ChatSessionPage · SettingsPage
                        ↓
L3 展示控件      MessageBubbleWidget · CollapsibleBlock(→ThinkingBlock/ToolBlock/TodoCard)
                 PermissionCard · SessionSidebar · ChatStatusStrip · ChatMsgEdit · SendMsgButton
                 WorkDirPathBar · InputDialog · NavItem · ContributionHeatmap
                        ↓
L4 Agent 编排    AgentLoop（1 类 × 17 TU）· SubAgent
                         ↓
L5 引擎/传输     QOpenAi · BashRunner · CompactManager · MemoryManager · TaskStore
                  CronSchedulerManager · BackgroundTasksManager · SessionStore
                  MessageBus · WorktreeManager · AgentTeamsManager · TeammateRuntime
                  McpManager · WorkflowManager · WorkflowRuntime · GoalManager · UpdateManager
                        ↓
L6 单源与设施    AgentConstants.h · ToolNames.h · LayoutConstants.h · AppSettings.h · SettingsCache.h
                 I18n · ThemeAware · ToolTagKind.h · AgentLoopInternal.h · LineEnding.h · CompactTokens.h
                 · UsageTokens.h · StreamRate.h · ActivityLedger.h · HeatmapGrid.h
                 · MemoryRules.h · ToolSchema.h · SessionKeys.h · ProjectInstructions.h · UpdateRules.h
                 · GoalRules.h · AgentRecovery.h · AgentPathGuard.h · LitePaths.h · CompactTranscript.h · UiScale.h
                 · ContextTrim.h
```

**两条铁律（本仓最重要的两条边方向）**

1. **L5/L6 零 GUI 依赖**：引擎层不 include 任何 widget 头，`CompactManager`/`MemoryManager`/
   `TaskStore`/`CronSchedulerManager` 甚至不是 QObject 或不发业务信号，宿主信息一律经**构造注入
   的 sink/回调**传入（例：`AgentLoop.cpp:43-54` 把 `sessionDataRoot()` 注成六个引擎的 workDir sink
   ——`m_compact`/`m_memory`/`m_cron`/`m_taskStore`/`m_bus`/`m_worktrees`，s13 新增后两个——的
   `[this]{ return sessionDataRoot(); }` 惰性 lambda）。
2. **不反向依赖**：引擎绝不 include `AgentLoop.h`；页面绝不 include 引擎内部头（唯一例外见 §9）。

---

### 2. 分层清单

#### L1 应用壳与启动

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `App.cpp` | `main()`：装 translator、显示主窗、退出码 931 只透传（严禁二次拉起） | `I18n`、`LiteHarness` |
| `LiteHarness` | 主窗口（`FluFrameLessWidget`）：导航 + 堆叠装配、会话新建/恢复/重命名/删除、主题语言联动、覆盖 FluentUI 取色的 `appendOwnSheetOverride`；**s17.8 会话活动缓存** `m_activityCache`（dataId → `QHash<QDate, DayTotal>`）——构造末尾一次性扫全部会话 `.activity/daily.json`、每页 `activitySessionChanged` 增量刷新、删除会话剔除，合并后 push 到 `NewChatPage::setActivityData` | 全部页面、`SessionRegistry`、`SessionStore`、`QOpenAi`（配置初始化）、`NavItem`、`ThemeAware`、`ActivityLedger.h` |
| `SessionRegistry` | 导航 key ↔（页 / dataId / 导航子项）反查表 + 右键监听映射；只持 `QPointer`，零所有权 | `NavItem.h`（`NavKey` 单源）；`.cpp` 需完整 `ChatSessionPage` 类型（见 §9 异常 1） |

#### L2 页面

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `BasePage` | 导航页基类；主题契约：基类构造不首刷，派生页构造尾 `ThemeAware::bind` | `ThemeAware` |
| `NewChatPage` | 发起页（欢迎语 + **会话活动热力图** + 工作目录条 + 输入框），发 `newChatRequested(text)`；进入时重读 `config.json` 默认目录；热力图数据由宿主 LiteHarness push（`setActivityData`），本页面从不直读磁盘、不订阅 AgentLoop | `BasePage`、`ChatMsgEdit`、`WorkDirPathBar`、`ContributionHeatmap`、`AppSettings`、`LayoutConstants`、`ActivityLedger.h`（形参类型） |
| `ChatSessionPage` | 每会话一页：布局 + **持有 `AgentLoop`** + `wireAgent()` 把全部后端信号接成 UI + 历史重放 + 侧栏与状态条接线 | `AgentLoop`、**`AgentLoopInternal.h`**（`ChatSessionPage.cpp:19`）、`MessageBubbleWidget`、`ToolBlock`、`PermissionCard`、`TodoCard`、`SessionSidebar`、`ChatStatusStrip`、`ChatMsgEdit`、`WorkDirPathBar` |
| `SettingsPage` | 主题/语言/默认工作目录/模型服务 `apiBaseUrl`+`apiToken`/模型清单 `modelOptions`（下拉选项，逗号分隔）/上下文上限/轮次上限/权限审批开关 `permissionApproval` | `BasePage`、`QOpenAi`（直改运行时配置）、`AppSettings`、`AgentConstants`、`I18n`、`InputDialog` |

#### L3 展示控件

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `MessageBubbleWidget` | 一条消息气泡：流式打字机 + 纵向时间线（思考块/工具卡/权限卡/记忆进度按到达顺序内嵌） | `ThinkingBlock`、`ToolBlock`、`LayoutConstants` |
| `CollapsibleBlock` | 折叠骨架基类：手动几何头部 + `contentHeight` 属性动画（300ms OutCubic）+ live 圆点轮播；构造禁调虚函数 | `ThemeAware` |
| `ThinkingBlock` / `ToolBlock` / `TodoCard` | 思考折叠块 / 工具执行卡（终态 + 3 种 live 形态）/ 任务清单时点快照卡 | `CollapsibleBlock`；`ToolBlock` 另用 `ToolTagKind`、`ToolNames`、`AgentConstants` |
| `PermissionCard` | 权限审批卡：待决主体 → 单行「已允许/已拒绝」留痕；发 `userResolved(bool)` | `ThemeAware`、`ToolTagKind` |
| `SessionSidebar` | 会话右栏信息面板（标题/模型/工作目录/上下文占用条+数字行（`#ctxNumbers`，**行内 K/M 紧凑、tooltip 千分位精确**）/状态灯/任务清单/团队名册/当前目标/后台任务/定时任务/页脚版本；后三节为 s17 P1 观测面，空态整节隐藏）；**纯视图，不订阅 `AgentLoop`**，全部由 `ChatSessionPage` 调 setter 推入 | `ThemeAware`、`LayoutConstants`、`UsageTokens.h`（形参类型 + `usageRatio` 比例算术 + `formatCompact` 紧凑读数） |
| `ChatStatusStrip` | 输入框正下方的常驻状态条（**一行三段文字**：轮步段/消耗段/速度段）——s16.1 从侧栏拆出，作为输入组末位成员同栏宽；占用**条**与占用**数字**（数字按用户裁决自本行首段迁回侧栏「上下文」节，收起侧栏时随条一并不可见=明示代价）与「运行状态」三态灯归侧栏，本组件不复制一份灯；消耗段**行内走 K/M 紧凑读数、tooltip 给千分位精确值**；**纯视图，不订阅 `AgentLoop`、也不读 config.json**（轮步段的步数上限由页面传入） | `ThemeAware`、`LayoutConstants`、`UsageTokens.h`/`StreamRate.h`（形参类型 + `formatCompact` 紧凑读数，其余算术仍在纯头） |
| `ChatMsgEdit` / `SendMsgButton` | 底部输入框（`sendMessage` / `stopRequested` / `modelChanged` + `setTurnBusy`）/ 圆形发送-停止两态钮 | `AgentConstants`、`LayoutConstants`、`ThemeAware` |
| `WorkDirPathBar` / `InputDialog` / `NavItem` | 只读路径条（中间省略 + tooltip 全路径）/ Fluent 风格单行模态框 / 导航项最小派生（补 `removeChildItem`）+ `NavKey` 常量 | 仅 Qt / FluentUI |
| `ContributionHeatmap` | 会话活动热力图（53 列 × 7 行 = 371 天滚动窗口，恒为满矩形、末格即今天，仿 GitHub contribution graph）；NewChatPage 英雄区下方。**纯视图 QWidget**，`setData(QHash<QDate, DayTotal>)` 由宿主 LiteHarness push（本页面从不直读磁盘、不订阅 AgentLoop——承 SessionSidebar 的 setter-push 纪律）；tooltip 显示当日五字段（**四个 token 字段走 K/M 紧凑读数，本视图无行内读数、tooltip 就是唯一展示面；`calls` 是次数不是 token，保持精确整数**）+ **覆盖边界免责**（「仅主循环与队友回合，不含压缩/记忆/workflow」）。**网格算术转调纯头 `HeatmapGrid.h`**（`windowStart`/`cellOf`/`dateOfCell`/`monthLabels`），本类只留像素几何与绘制（月份标签选列、按**月号**去重与首列补标让位给同号 1 号锚点的判据也在纯头——371 天窗口首尾同号跨年，不去重底部必现两个相同月份；保留补标又会与相邻月标签挤压，s18 两阶段修），`kWeeks`/`kDaysPerWeek` 是该头常量的别名、不重写 53/7（避免第二个真值源）。三主题色板硬编码在本 cpp（FluentUI fork 无 `FluThemeColorType`），`ThemeAware::bind` 订主题切换重刷 | `ThemeAware`、**`HeatmapGrid.h`**（网格算术单源）、`ActivityLedger.h`（`DayTotal` 形参类型、阈值判据 `levelForTokens`）、`UsageTokens.h`（`formatCompact` 紧凑读数） |

#### L4 Agent 编排

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `AgentLoop` | 回合状态机：`run()` → 召回 → 压缩前导 → 流式请求 → 工具批串行推进 → 终局落盘与记忆沉淀；**值成员**持有并编排全部 L5 引擎 | `QOpenAi`、`CompactManager`、`MemoryManager`、`TaskStore`、`CronSchedulerManager`、`BackgroundTasksManager`、`SubAgent`、`BashRunner`、`MessageBus`、`WorktreeManager`、`AgentTeamsManager`、`TeammateRuntime`、`McpManager`、`WorkflowManager`（s16 值成员 `m_workflow`；`WorkflowRuntime` 经宿主接线 TU 以 shared_ptr 持有）、`ToolNames`、`AgentConstants` |
| `SubAgent` | `task` 子代理：独立 messages / ChatStream / 工具队列 / 权限槽的黑盒，唯一出口是完成汇总文本。**s18 上下文压缩**：每请求主动成组裁剪（目标 4/5×`contextTokenBudget()` 现取）+ 溢出错误反应式压缩恰一次（成功收响应复位旗标），内核与队友共用 `ContextTrim.h`，system[0]+任务 prompt[1] 恒保留；无 usage 回读无 token 锚，估算=本地全量+tools schema（overhead 仅 schema，system 已在 messages 内——区别于队友） | `AgentLoop`（`friend`）、`QOpenAi`、`AgentLoopInternal.h`、`BashRunner`、`ToolNames`、`ContextTrim.h`、`CompactTokens.h` |

#### L5 引擎 / 传输（零 GUI）

| 模块 | 职责 | 依赖 |
|---|---|---|
| `QOpenAi` | OpenAI 兼容 SSE 客户端：`ChatStream`（`thinkingDelta`/`textDelta`/`messageFinished(fullMessage, finishReason)`——s15 双参 BREAKING/`usageReceived`/`error`）+ `AsyncRequest`（一次性文本请求，`done` 恒恰好一次）+ 运行时 url/token/model 配置 + 429/5xx 指数退避重试（`maxRetries`，可重试判定先于 4xx 硬错误；**s15**：idle 超时（60s 无数据）转可重试、消耗同一 retriesLeft 预算，30min 总超时仍直接终态=偏差登记；HTTP 529 或过载文本判据（`AgentRecovery.h` isOverloadedText）连续达 2 且已配置 `fallbackModelId` → 换请求体 model 重发、每流一次；退避保持 1s/2s/4s…64s 无抖动——不采 lcc 500ms+25% 抖动=偏差登记；`scheduleRetry` 重试前补清 pendingFrame/finishReason）。`usageReceived` 只在请求体带 `stream_options.include_usage` 时发射（末帧 `choices=[]` 且带 usage，故 usage 捕获先于 choices 检查；`finish_reason` 已到而 usage 未达时启 `kUsageGraceMs` 单次宽限） | `AppSettings`、`AgentConstants` |
| `BashRunner` | bash 执行段单源：`dangerWarning` / `truncateOutput` / `finalizeOutput` / `start`（`powershell.exe -NoProfile -NonInteractive`） | `AgentConstants` |
| `CompactManager` | 五级压缩管线 `toolResultBudget → snip → micro → fitToolResults → compactHistory` + 溢出反应式压缩；非 QObject，全靠回调注入。**micro/fit 两段为增量计量**（循环前置一次总量、每次替换 `total += 新项-旧项`，见 §9.9 第 9 条），`estimateTokens` 是 `CompactTokens::conversationTokens` 的转发 | `QOpenAi::AsyncRequest`、`AgentConstants`、`CompactTokens.h` |
| `MemoryManager` | 持久记忆：召回 / 提取 / 合并三条异步链，落 `.memory/MEMORY.md` + `<slug>.md` | `QOpenAi`、`AgentConstants` |
| `TaskStore` | 任务图文件存储：一任务一 `.task/task_<hex8>.json`，每操作直读盘；6 个 `run_*` 纯文本进出；**s13 租约扩展**：租约版 `claimTask` 六门（任务可读 / pending 且无主 / 该 owner 无内存租约 / 磁盘无该 owner 的 in_progress / blockedBy 全 completed / cwd 可解）；`completeTask` 完成后**故意不退租**（同回合后续工具仍需租约 cwd），释放只在两个回合边界 API（`releaseCompletedAssignment` / `releaseTeammateAssignment`，后者无条件清内存租约+触发 released 回调）；换工版本只在 claim 递增、释放不推；`assignmentCwd` 三分支热路径（无租约 Lead 键回落 / 无租约队友 fail-closed / 有租约读盘校验+自愈回写不推版本）；`scanUnclaimedTasks` 纯侦察；承重跨模块契约 claim 成功文案 `Claimed ` 前缀；租约台账纯内存 D7 重启作废；遗留非租约路径 `claimTaskUnleased`/`completeTaskUnleased` 逐字保留 | `AgentConstants` |
| `CronSchedulerManager` | 定时任务台账：cron 校验/匹配、到期队列、`scheduled_tasks.json` 持久化、两段式 at-least-once 交付；`QTimer` 1s 轮询不建线程；**s17 P1**：`ledgerSnapshot()` 台账快照经宿主 tick 边沿广播侧栏「定时任务」节，判据入 `tst_cronmanager`（本 TU 已入测试目标表） | 仅 QtCore |
| `BackgroundTasksManager` | 后台 bash 任务台账：登记 / 存结果 / 渲染 `<task_notification>`，进程由宿主驱动；**s15**：`hasPending()`（只查不清）作 AgentLoop tick 第 4 事件源；通知模板 `<summary>`[:200]（原 `<result>`[:500]）；成功占位逐字 "[Background task %1 started] Result will arrive as a task_notification."、start 失败串 "Error: Failed to start background task: %1" | **s17 P1**：`ledgerSnapshot()` 台账快照（在跑+待收割，收割即离账）经宿主 tick 边沿广播侧栏「后台任务」节 | `ToolNames` |
| `SessionStore` | 全局会话索引，**已并入配置文件**：落 `<config>/config.json` 的宿主键 `app/sessions`（盘上形态 `{"app":{"sessions":[{…}]}}`），条目 dataId/title/model/workDir/createdMs；读写**只经 `SettingsCache::Reader`**（生产传 `AppSettings::reader()`，测试注入自建 Reader），**严禁裸 `QSaveFile` 整文件重写**——同一文件还有宿主键与 FluentUI 键两个 owner，裸写会把对方刚落的键一起覆盖（QSettings 之间的合并保证只对 QSettings 实例存在）；`migrateLegacyIndex()` 一次性搬旧 `<config>/index.json`（目标键已是列表则只删残留、绝不回写；旧文件解析不出数组则保留待人工处理；写成功且回读条数一致才删）；`lastActiveMs` 字段与 `touchEntry()` 已删（全仓无读取方，且它是唯一的高频整表重写来源，续活链改由 history.json mtime 承担）；纯静态 header-only（无 `.cpp`） | 仅 QtCore |
| `MessageBus` | s13 团队邮箱内核（lcc s13 34775c8 message_bus.py）：`<会话根>/.mailboxes/<name>.jsonl` 一代理一邮箱，append 写+**破坏性读**（读走即删）= at-most-once，无游标无 ack；三重 fail-closed 路径门（收件名 `^[A-Za-z0-9_-]{1,64}$` / 邮箱目录会聚 sessionRoot / 解析后路径仍在目录内）；信封 6 字段，type 9 种由调用方给定不校验；M8 drain 删除失败→**失败即空批**+lastError（宿主下一 tick 重试）；坏行跳过（D9-defensive 偏离 lcc raise） | 仅 QtCore |
| `WorktreeManager` | s13 git worktree 隔离内核：落 `<会话根>/.worktrees/<name>`、分支 `wt/<name>`；create 十门 / remove 五门（`discardChanges` 只豁免 porcelain 门并加 `--force`，余四门绝对）；**铁律：git 失败绝不自动清理、remove 成功也永不删分支**；`removeWorktree` **刻意不是工具**（不进 ToolNames.h，仅宿主关停/手工，防模型误删）；create 返回折叠文本（`Error:`/`Partial operation:`/`Partial success:`）、remove bool+*error；`resolveWorktreeCwd` 是 `TaskStore::setCwdResolver` 注入目标 | 仅 QtCore |
| `AgentTeamsManager` | s13 团队协议内核：Lead 7 工具内核（`runSpawnTeammate`…`runCreateWorktree`）+ 队友侧 `sendTeammateMessage`（收件恒 lead）/ `claimNextTask` / `runTeammateTool`（工具总闸：plan 门只拦 bash/write_file/edit_file，read/glob 恒放行）；五本账**全内存态**（队友台账 / planGates / 在评审请求 / 协议待决 / 运行时句柄，D7 重启作废；句柄持 `TeammateRuntime*` 偏离 lcc 线程句柄）；协议门 submit_plan 一案一批、`applyPlanResponse` 11 项合取、`applyShutdownRequest` 8 项——**门翻转只发生在队友侧**（Lead `runReviewPlan` 不触 planGates）；`consumeLeadInbox` 返回原始批（回执不吞）、`formatTeamEvents` 静态 `[Team events]…` 文本（空批回空串，偏G 优于 lcc）；注入点 `setTeammateLauncher`（create-then-deferred-start 契约、nullptr=回滚 fail-closed）/ `setWorktreeCreator` / `setPermissionCheck` / `setHooksTrigger` / `setToolAdapter`×5 | QtCore + MessageBus/WorktreeManager/TaskStore |
| `TeammateRuntime` | s13 队友无头状态机（lcc TeammateRuntime 段）：lcc 守护线程→**QTimer 心跳转译**（`AgentConst::kTeamIdleScanIntervalMs`=2000ms，仅 Idle 态单轮探测：drain 邮箱→协议处置→否则 `claimNextTask` 自拉活）；不持 ChatStream，回合 LLM 由宿主经 `turnRequested` 驱动、`deliverTurnResult()` 回填，零嵌套事件循环；**s18 审计批**：满额预算每回合经 `refreshContextBudget()` 由宿主现取 `contextTokenBudget()` 刷新（改 `contextCharLimit` 后存量队友下一回合即对齐，不再钉死出生快照；overhead 字节恒定不重注）；`cancel()`→`finished`→宿主 delete；析构 FIND-L 兜底（未收口即**无 emit** 清算台账） | QtCore + AgentTeamsManager 门 |
| `McpManager` | s14 MCP 外部工具机制引擎（lcc s14 7b4ac1c mcp_manager.py；非 QObject 纯引擎、值成员接线）：`connect_mcp` 入口 + `normalizeName` 净化/`mcp__{server}__{tool}` 拼名（总长 ≤64）+ 每轮 `assemble` 快照原子替换（origins 溯源碰撞检测，发现期失败保上次快照）+ `hostPolicy`/`toolPolicy` allow/confirm 缺省 fail-closed；动态工具不入 ToolNames.h/静态 schema；**当前注册表为空**（方案1：不移植 MCPClient/MOCK_SERVERS；connect_mcp 任意名回流 Unknown server；头注释登记与 lcc 的 7 条偏差）；Lead 独占 | 仅 QtCore |
| `WorkflowManager` | s16 工作流编排引擎内核（lcc s16 42a555d workflow.py；非 QObject 纯 QtCore 引擎、值成员接线仿 `McpManager`）：注册表（当前唯一条目 `review-changes`=忠实移植 lcc 演示工作流）/ 身份与语义键（createRunId / createTaskId / validateRunId / validateMeta / stableHash / agentKey）/ journal（`.workflows/<runId>.journal.jsonl` 逐行追加 + resume 逐行校验）/ SimpleJsonSchema 校验与 fillSchema / Budget 与 ExecutionLimits / AGENT_CAP=1000 / CONCURRENCY=8 / runId NewOnly 原子预留（撞名重试 ≤32）/ QSaveFile 原子快照 / last_run 账 / 进程内 run 抢占集合（lcc 跨进程 fcntl 文件锁不采=偏差）；`setStoreDir` 推形（宿主在会话数据根变更点推 `<会话根>/.workflows`，目录创建自愈在引擎写/读入口各自内联的 `QDir().mkpath`——`reserveRunId` / `writeJsonAtomic` / `saveLastRun` / `WfJournal::open`，宿主不 mkdir；引擎里没有 `ensureStoreDirs` 这个函数）；头注释登记与 lcc 的偏差①-㉒；工具信封单源成员 `toolInfo()`；Lead 独占 | 仅 QtCore |
| `WorkflowRuntime` | s16 一次性异步编排对象（**非 QObject**，宿主以 shared_ptr 持有槽）：六原语 agent / parallel / pipeline / phase / log / workflow 全事件驱动状态机（`QTimer::singleShot(0)` 续跑，零线程零嵌套事件循环）；HIT 脏缓存拒用 / MISS schema 唯一一次重试 / resume 四闸先于 journal 截断 / 信封三写终态收口；`kFocusedSystemPrompt()` / `buildRunnerPrompt()` / `kAgentMaxTokens=2000`；`requestCancel()` 置旗由在途续跑点折叠终态；偏差 9-19 在头注释 | QtCore + WorkflowManager |
| `GoalManager` | s17 goal 引擎内核（lcc s17 a20fdec goal_manager.py；非 QObject 纯 QtCore 引擎、值成员接线仿 `McpManager`/`WorkflowManager`）：会话级 Stop 闸门，**账本纯内存不落盘**（区别于 `.workflows` 磁盘台账；`restore` 静态工厂备好但 v1 宿主不接线=偏差⑥/头注释偏差9）。三闸 API（异步拆分=偏差①b）：`stopGate(messages)` 判据出口（Allow / Defer（bg running 注入判据）/ NeedEval（携评估器 prompt + generation 快照））、`onEvaluatorText(raw, generation)` / `onEvaluatorError(text, generation)` 回填折叠七决策（achieved/failed/limit/error/block/defer/allow）；**代际守卫**（setGoal 含 replace / clear 成功即 `++m_generation`，迟到回填代际不符按默认 Allow 静默丢弃零记账）；`parseInput`（`/goal` 四类分流 none/status/cleared/set + `Kind::Error` 增项）、`setGoal/clear/status/beginQuery/observeUsage`；consecutiveBlocks 只 block 累加、Limit 后 active 保持；block_cap 构造期快照（改 config.json 新会话生效）；头注释登记与 lcc 偏差 1-10 | 仅 QtCore |
| `UpdateManager` | **s17.12 自更新编排层**（独立 `QObject`，646 行，**非 AgentLoop 家族 TU**——由 `SettingsPage` 持有（`m_updateManager`），而 §3 家族地图只列 `AgentLoop.h` 的成员方法 TU，故归本表）：检查（GET Releases API，UA 硬要求）→ 下载（流式落 `<configRoot>/.lite-harness/.update/incoming.zip`，边下边算 sha256，30s 静默看门狗）→ 校验（digest 不符即删档，**绝不碰安装树**）→ 解压（系统 `tar.exe -xf`，信号驱动）→ 换体（**原地逐文件**「改名腾位 → 写入新体」，实测：运行中 exe 可改名、原路径可写、改名后的运行中镜像不可删；故路线只能是「改名→写→重启→新实例删残留」，而「装到同级新目录」已被实测否掉——`configRoot = <exe>/../config`，换目录等于换数据根、会话/记忆/设置当场「消失」）→ 写 marker → 重启（复用主窗口 `close()` 守卫 + `startDetached` 带 workingDirectory，退出码 932 与语言重启的 931 区分）；`cleanupResidueAtStartup()` 由 `App.cpp` 在 `setApplicationVersion` 之后、建窗之前调用。**判据一律在 `UpdateRules.h`**（本 TU 链 `Qt6::Network` 进不了 ctest，把判据写在这里即失去全部直测）；跨进程互斥用 `QLockFile`（本仓此前无单实例护栏，实测两实例可同跑）。关键方法：`checkLatest` / `stageUpdate` / `applySwap` / `requestRestartAfterSwap` / `cleanupResidueAtStartup` / `probeLayout` | `Qt6::Network`、`UpdateRules.h`、`LitePaths.h`；重启交接借主窗口 `QWidget::close()` 守卫（`.cpp` include `<QWidget>`/`<QApplication>`，是唯一 GUI 触点——进程交接的形参类型，非驻留视图，故不与「L5 零 GUI」铁律冲突） |

#### L6 单源与设施

| 模块 | 单一事实源 |
|---|---|
| `AgentConstants.h` | 模型清单（config.json `modelOptions` 逗号分隔 + `defaultModel` 缺省项，未配置回落内置 `kBuiltinModelOptions`；读值经头内 `textValue()` 归一化「列表」形态——ini 后端裸逗号串落成 `QStringList`、json 后端数组落成 `QVariantList`，两者裸 `.toString()` 都得空串）、**权限审批总开关**（config.json 键 `permissionApproval`，`kPermissionApprovalKey` + `permissionApprovalEnabled()`，缺省/读不出回退 true=多问一次的 fail-safe 方向；消费方两处同源：`AgentLoopHooks.cpp` permission 钩子与 `AgentLoopTeam.cpp` 队友 permissionCheck 注入）、`kMaxTokens` 与 s15 恢复常量（`kEscalatedMaxTokens=16000`/`kMaxConsecutive529=2`/`kMaxRecoveryRetries=2`）、`fallbackModelIdValue()`（读 config.json `fallbackModelId`，空=禁用换模仅退避）、bash 超时与错误文案、输出截断、上下文上限默认/校验界、glob 上限、**中间目录名**（`.task`/`.temp`/`.transcripts`/`.memory`/`.mailboxes`/`.worktrees`/`.workflows`（s16 编排台账），见 `AgentConstants.h` 的 `k*DirName` 常量组，拼法涉数据兼容不可改）、**token 口径**（`estimateTokens` 字符→token 折算：CJK 1:1、其余 4:1；全局预算 `contextTokenBudget()` **= 声明值本身**，s17.9 起不再 ÷4、常量 `kCharsPerTokenBudget` 已退役，恒等接缝 `contextTokenBudgetFrom()` 供 `static_assert` 与测试钉死；上下文窗口上限 `contextCharLimitValue()`） |
| `ToolNames.h` | 27 个工具名（18 基础：bash/read_file/write_file/edit_file/glob/todo_write/task/load_skill/compact/create_task/update_task/list_tasks/get_task/claim_task/complete_task/schedule_cron/list_crons/cancel_cron + 7 团队：spawn_teammate/list_teammates/send_message/request_shutdown/request_plan/review_plan/create_worktree + 1 MCP 入口：connect_mcp + 1 编排入口：workflow（s16，Lead 独占））；`submit_plan` 为队友专属名——单源在引擎匿名 ns+宿主队友 schema 字面量，**刻意不入本表**；另含 `MCP_PREFIX`（`mcp__` 前缀常量，供权限钩子识别命名域，非工具名）；动态 `mcp__{server}__{tool}` 工具不入本表、由 `McpManager::assemble` 每轮追加；队友侧 send_message/list_tasks/claim_task/complete_task 复用基础名 |
| `LayoutConstants.h` | 聊天栏宽 800 / 边距 35 / 气泡系数 0.75 / 侧栏宽 225 / 导航宽 225（两栏同宽） |
| `AppSettings.h` | 配置存储门面：一律 `LitePaths::configFilePath()` = **`<exe 目录>/../config/config.json`**（弃用注册表；改造前钉 `applicationDirPath()/settings.ini` 即 exe **同**目录，现与索引/会话数据同根，一个存储区一个锚。`filePath()` 是本头最后的字符串单源，路径本身不再自算）。**与 FluentUI 配置合流成同一个文件**：`App.cpp` 恒注入 `FluConfigUtils::configure(LitePaths::configRoot(), LitePaths::configFileName(), FluConfigFormat::Json)`，原 `fluConfigDir`/`fluConfigFile`/`fluConfigFormat` 三键**已删除**——它们定位的正是本文件自身，读键得先知道文件在哪，成了自指，且落点已由 `LitePaths` 钉死成单源。宿主键一律挂 `app/` 组（`scopedKey()` 单点拼前缀，键清单见 `AppSettings.h:5-9`：`defaultWorkDir`/`sidebarVisible`/`language`/`contextCharLimit`/`maxToolIterations`/`apiBaseUrl`/`apiToken`/`modelOptions`/`defaultModel`/`maxRetries`/`fallbackModelId`/`permissionApproval`/`goalEvaluatorModelId`/`stopHookBlockCap`/`uiFontScale`/`windowGeometry`/**`sessions`**（会话索引，值 = `QVariantList of QVariantMap`；**读写归 `SessionStore.h` 不归本头**，但它经同一支 `reader()` 走同一格式同口径，所以登记进这份键清单——漏登记会让清单看起来就是键空间地图，下一个加键的人会以为顶层才是 `sessions` 的位置）），与 FluentUI 自有的 `config/*`、`translation/*` 各占一个子对象、互不可见。**格式与折叠口径成对交出**：`fileFormat()` 取 vendor 的 `FluConfigStore::jsonFormat()`（宿主不自注册同名格式），`keyCaseSensitivity()` 返 `Qt::CaseSensitive` 与之配对——vendor 的 json 后端本就注册为大小写敏感，宿主无权为迁就自己的缓存去改第三方库的语义。**只剩路径单源 + 两个转发**：`value()`/`setValue()` 一律转给 `SettingsCache::Reader`；原 `ini()`（按值返回新鲜 QSettings 供各处自取）**已摘除**——它让「每次调用现构造一个」看起来是正当用法，即问题②的复发通道 |
| `ToolTagKind.h` | 工具名 → 语义类别（read/search/plan/delegate/run/write/other）→ QSS `toolTagKind` 动态属性 |
| `I18n.h` | 中英文切换：源文中文 + Linguist + **重启生效**（`exit(931)` 自重启） |
| `ThemeAware.h` | 「加载 QSS + 订阅 themeChanged + 重载」样板单源 `bind(qss, widget, extraRefresh)` |
| `NavItem.h` | `NavKey` 导航/堆叠键单源 |
| `AgentLoopInternal.h` | `AgentLoopDetail` 跨 TU 内部工具**声明**单源（§4） |
| `LineEnding.h` | 文本文件行尾口径单源：`dominant`（主导行尾判定，平局偏 CRLF）/ `toLf`（匹配域归一，孤立 CR 不动）/ `apply`（写回域还原，幂等）/ `replaceOnce`（两级匹配单次替换）。`edit_file` 与 `write_file` 共用，杜绝「read_file 交还 LF 文本、edit_file 按原文字节匹配」导致的多行失配与混合行尾。`countOccurrences` 报命中次数（经 `replaceOnce` 的 `matchCount` 出参，与 `matched` 同为 nullptr 容错），供 `edit_file` 拒绝歧义编辑 |
| `CompactTokens.h` | 会话体 token 计量纯头（header-only，零 CMake 改动即入 ctest）：`messageTokens`（单条项，只依赖入参自身）/ `conversationTokens`（逐条独立求和）。**承重不变量①②③写在头注释**：项间零耦合 ⇒ 替换一条后 `新总量 == 原总量-旧项+新项` 是数学恒等而非近似，`microCompact`/`fitToolResults` 的增量维护据此成立；改成整段一次序列化会同时破 ①② 并让压缩触发点静默漂移，`tst_compacttokens` 即为此设的红线。`CompactManager::estimateTokens` 退化为它的转发，宿主（`AgentLoop`/`TeammateRuntime`）与压缩管线共用同一实现 |
| `ContextTrim.h` | 上下文成组裁剪内核纯头（`ContextTrim` 命名空间，header-only **不占测试目标 TU 清单**，`tst_contexttrim` 直接 include 生产头）。`trimConversation(messages, keepHead, currentTokens, targetTokens)`：从 keepHead 起最旧优先扫描，`current > target` 期间按「可安全成组删除」单元丢历史——**assistant 带 tool_calls 必须连同其后连续 tool 结果整组删**（孤立删 tool 破坏 OpenAI 形态配对即上游 400）；扫描位上的孤立 tool（畸形历史兜底）**跳过不删**；只删不重组，保留消息相对顺序不变；前 keepHead 条恒保留（队友=1 任务卡、SubAgent=2 system+任务 prompt）。减量按 `CompactTokens::conversationTokens` 逐条独立求和（不变量①）；`currentTokens` 由调用方传入其当前口径估算——队友=锚定/兜底含 overhead（减法为有界近似，与抽头前逐值等价），SubAgent=本地全量+tools schema（数学恒等）；返回是否动过，锚定调用方据此作废 token 锚（SubAgent 无锚不簿记）。比例常量单源在本头：主动 4/5（`kCompactTargetRatio*`）、反应式 1/2（`kReactiveTargetRatio*`），原 TeammateRuntime.cpp 匿名 ns 常量逐字搬入。抽头动机：内核原长在 `TeammateRuntime.cpp`（QObject + QOpenAi 依赖链进不了 ctest），配对逻辑安全关键却零直测；SubAgent 复用若复制一份即开出第二处漂移源。抽头后 `TeammateRuntime::compactConversationTo` 退化为**一行转发**（字节等价），SubAgent 接主动+反应式两路（s18，此前「SubAgent 无上下文压缩」留观缺口兑现）。48 条零夹具 |
| `CompactStageNames.h` | 本地四段压缩卡的 UI 伪工具名单源（`CompactStageNames` 命名空间，header-only 仅 Qt6::Core、**不占测试目标 TU 清单**，`tst_toolnames` ⑦ 组直接 include 生产头）。`BUDGET`/`SNIP`/`MICRO`/`FIT` 四常量 = `compact_budget`/`compact_snip`/`compact_micro`/`compact_fit`，与 lcc s08 本地管线四段一一对应（顺序即管线顺序）。**不入 `ToolNames.h` 的理由是硬判据而非风格偏好**：`tst_toolnames`/`tst_toolschema` 钉住「常量清单 ↔ 静态 schema 27 项」恰好重合，伪名入表那两条断言立刻红；但**也不散写字面量**——发射侧 `CompactManager::emitLocalCard` 与呈现侧 `ToolBlock` 标题表共用本头，改一处即两侧同步，杜绝「卡发了但标题静默回退成『已执行』」这类失配。沿用 s09 `"memory"` 的 UI 伪键先例：`ToolTagKindNames` 未识别一律落 `other` 中性灰，不新增 kind 故三主题 QSS 零改动；`compact_` 前缀让用户在等宽标签上一眼认出压缩家族。9 条零夹具 |
| `UsageTokens.h` | 服务端 usage 用量计量纯头（header-only，零 CMake 改动即入 ctest）：`fromUsage`（usage 对象 → `Snapshot`）+ `Totals`（会话级只增累加 + `hitRatio`/`uncachedPrompt` 派生）+ **`formatCompact`（K/M 紧凑读数单源，本头唯一带「格式化」字样的口）**：`<1000` 原样整数、`<1e6` 折 K、`>=1e6` 折 M，一位小数四舍五入、整十位省 `.0`（`2000→2K`）、K 档进位到 1000 升 M 档（绝不出 `1000K`）、**刻意不设 G/B**。舍入一律「先除后取整」（`(m+50)/100` 而非 `(m*10+500)/1000`），被乘数不超出入参本身，绝对值走 unsigned 域故 `LLONG_MIN` 不 UB。四个消费视图（状态条消耗段 / 侧栏占用数字行 / 侧栏 goal 消耗 / 热力图 tooltip）共用它，**行内紧凑、tooltip 千分位精确**是成对口径：紧凑是读数不是账目，原值必须悬停可得。刻意不在文案加「≈」（K/M 后缀本身即四舍五入信号，加 ≈ 会新增 tr() 词条并让两套措辞分叉）。**与 `CompactTokens.h` 是两套东西，勿混**：后者算「窗口现在多大」（占用，会被后续回读覆盖），本头算「已经花掉多少」（消耗，只增不减）。**承重不变量写在头注释**：①有效性判据 `prompt_tokens>0` 与 `adoptUsageAnchor` 落锚门同一条——无效读数整条作废、不进分母，破了就会把「端点没回 usage」显示成「已用 0 token」，用户读作「没花钱」，两种状态对成本判断作用相反；②嵌套 `*_tokens_details` 缺失是常态（取不到即 0，不算解析失败，OpenAI 标准里本就可选）；③`hitRatio` 上界钳 1.0、`uncachedPrompt` 钳非负（实测个别兼容端点把 cached 报得大于 prompt，不钳即显示 120% 荒值，钳后原值仍在账上可查）。**覆盖面是部分口径、不是会话真实总开销**：只有带 `stream_options.include_usage` 的 ChatStream 路径回 usage（主循环 + 队友回合），压缩摘要/记忆链/workflow runner 走 `AsyncRequest`（该类型无 usage 通道）完全不在账上——UI tooltip 必须声明这条边界，否则数字误导成本判断。`tst_usagetokens` 82 断言钉①③ + `formatCompact` 的档位边界（999/1000、999949/999950、1e6）与 8 字符宽度上界 |
| `StreamRate.h` | token 输出速度纯头（header-only，零 CMake 改动即入 ctest）：`Window`（三时刻观测：t0 流建立 / t1 首个增量 / t2 usage 末帧）+ `makeWindow`（判据单源）+ `tokensPerSecond` + `Meter`（会话级只增账）。**与 `UsageTokens.h` 是两套东西，勿混**：后者算「已经花掉多少」，本头算「吐字有多快」——分母是时间，窗口取错时数字照样算得出、照样显示，但含义相反。**承重不变量①~④写在头注释**：①分母是生成窗口（t1→t2），**不含 TTFT**——把「请求发出→usage」当分母会把慢首字读成慢模型，故 TTFT 单独记录、单独显示；②无增量起点即窗口作废（纯 tool_calls 回合的 delta 只有 tool_calls 字段，不触发 textDelta/thinkingDelta），宁可不显示也不把 TTFT 冒充生成速度；③聚合 = Σtokens ÷ ΣgenerationMs，**不是**各窗口速率的算术平均（后者让 1 token 窗口与 2000 token 窗口等权，一条长回答被九条短回答稀释）；④有效性判据与 `UsageTokens::fromUsage` 同源（`prompt<=0` 即整窗作废），无效窗口不进分母也不改写 `last`，空账 UI 整行隐藏。**覆盖面比消耗账更窄**：仅主循环请求——队友回合增量不经宿主信号面、压缩/记忆/workflow 走 `AsyncRequest`（无 usage 通道），均无从定窗；tooltip 同步声明。`tst_streamrate` 26 断言钉①②③④ |
| `SettingsCache.h` | 设置读缓存纯头（header-only，零 CMake 改动即入 ctest）：`Identity`（文件身份三元组 exists/mtime/size）+ `sameIdentity` + `identityOf` + `Reader`（一个配置文件路径的读写门面，读带缓存、写带失效；构造入参含 `format` 与 `keyCaseSensitivity` **成对**，缺省为 IniFormat + `CaseInsensitive` 保既有 ini 侧调用点零改动）。**承重不变量①~⑤写在头注释**：①失效判据是文件身份（改 TTL/永久缓存即破 README「手改配置文件即可」承诺）②三元组缺一不可 ③写侧「先 sync 后 invalidate」顺序不可颠倒 ④查表折叠口径必须与所用后端的注册口径**成对**（缺省对齐 IniFormat 的 `IniCaseSensitivity`；换 json 后端就得同时传 `Qt::CaseSensitive`——口径配错不会报错，只会「缓存 hit、直取 miss」给出两个不同答案）⑤无锁依赖零线程约定。路径构造期注入是它能进 ctest 的全部原因（`AppSettings` 绑死 `applicationDirPath()`，测试里造不出也不该往 exe 目录写盘）。**口径纠正**：原登记说每次读「open + parse 整个 ini」不准确——Qt 自带 `ConfFileCache` 且在 size+mtime 未变时短路，本修复省掉的是每次调用的 QObject 分配 + 全局锁 + 缓存探测 + stat + 键折叠查表 + 析构 sync 簿记，收益真实但不大（勿按旧说法宣称成「消除了重复磁盘解析」） |
| `AgentRecovery.h` | s15 恢复契约串与文本判据纯头（`RecoveryRules` 命名空间，禁翻区）：逐字 `continuationPrompt()` / `asyncApprovalDenied()`（两拍续打、异步定值拒、isToolFailure 前缀判定、测试逐字断言的单源——改文案须同步 `tst_agentrecovery`、`isToolFailure` 判据与 TeammateRuntime 镜像）；`isRateLimitText` / `isOverloadedText` 鸭子文本判据供 `QOpenAi` 可重试判定 |
| `GoalRules.h` | s17 goal 契约串与判据纯头（`GoalRules` 命名空间，禁翻区，仿 `LineEnding.h`/`AgentRecovery.h` 先例；header-only **不占测试目标 TU 清单**，`tst_goalmanager` 直接 include 生产头）：七决策类型（Allow/Defer/Error/Achieved/Failed/Limit/Block + `actionName`）、CLEAR_ALIASES 六词逐字、设定/回显契约串族（emptyConditionError/tooLongConditionError/blockCapError/replacedReason/goalSetReason/clearedReason/noGoalSetText/goalClearedText/deferReason/limitReason/goalErrorReason）、`evaluatorSystemPrompt`/`evaluatorUserPrompt`（payload Compact JSON 单 `.arg`）/`blockContinueText`；**`parseEvaluatorJson` 六门**（invalidJson/notObject/needBoolOk/needReason/impossibleBool/bothOkImpossible，各有定值英文串、逐字断言；fence 剥除最前 + 顶层标量探针 `{"goal_probe":…}` 包裹拆开门2/门3）；**`transcriptText(messages, kGoalTranscriptCharLimit=24000)`** 新→旧逐条累加（itemSize=条长+2），唯一例外=最新一条独占超限就地插标记 `\n...[middle omitted]...\n`（头 3/4 + 尾 1/4）后 break，其余旧条目超限是**不进**而非截断，反转后 `\n\n` join；`plainContent` OpenAI 形适配（role=tool→`[tool_result %1]`、tool_calls→`[tool_use name args]`）。体量口径 UTF-16 码元（偏差登记在 `GoalManager.h` 头注释） |
| `LitePaths.h` | 运行时路径锚点单源（header-only 纯静态类，禁实例化，仅 QtCore、**零 IO**：不 mkpath、不判存在、不读配置，落盘时机归各消费点）。**全仓唯一的存储根锚点**：`configRootFor(exeDir)` = `QDir::cleanPath(exeDir + "/../config")`（折叠 `..`、尾斜杠/反斜杠/冗余 `.` 段三种写法收敛到同一个根；空/全空白入参返空串**绝不猜一个根**），无参 `configRoot()` 现取 `applicationDirPath()`、无 `QCoreApplication` 实例当场 `Q_ASSERT`。派生面：`configFileName`/`configFilePathFor`/`legacyIndexFilePathFor`（存储根**本级**两文件，后者只剩「旧会话索引一次性迁移源」一个用途，`legacy` 前缀刻意防新代码误当现行落点）、`storeRootFor`（`<root>/.lite-harness`）、`sessionsRootFor`/`skillsDirFor`、`agentsFileName`/`agentsFileFor`/`globalAgentsFile`（s17.11 项目指令文件）、`updateDirFor`/`updateStagingDirFor`/`updateArchivePathFor`/`updateMarkerPathFor`（s17.12 自更新，一律落 `.lite-harness/.update`，与安装根**同卷**故换体改名是同卷原子重命名）、`sessionDataRootFor`（空 dataId **回退会话存储区本身**，逐字保留「无 ID = 未隔离」语义）。**`.lite-harness` 中间层只在本头拼一次**，各引擎只拼自己的叶子段（多拼一层不报错但静默分叉出 `.lite-harness/.lite-harness/`，`tst_litepaths` 逐条数出现次数钉住）。`*For` 后缀是纯口（可 ctest 直测），无参口是生产便捷口。`tst_litepaths` 29 条 |
| `AgentPathGuard.h` | 沙箱围栏**多根**包含判定 `isWithinSandboxRoots(path, roots)`（纯 QtCore 静态口，从 `AgentLoop::safePathIn` 外提以便直测——`AgentLoop` 带 `Q_OBJECT`、其 TU 依赖 moc + QOpenAi + FluentUI，进不了只链 `Qt6::Core` 的测试目标）。判据三条缺一即被绕过：① **分隔符边界**——只认 `root + '/'` 前缀或 `path == root`，纯 `startsWith` 会放行 `work-evil` 这类共享名字前缀的兄弟目录；② **大小写不敏感**（Windows 文件系统语义，但放宽不得越过分隔符）；③ **fail-closed**——空根集合拒一切，集合里的空串跳过而非接管判定（`startsWith("")` 恒真 = 围栏全开）。根入参自行 `cleanPath` 归一。调用方经 `AgentLoop::sandboxRoots(toolCwd)` 单源构造 = `{toolCwd, sessionDataRoot()}`——**第二根是会话数据根**（存储锚统一到 `config/` 后 `.temp`/`.worktrees` 不再是 workDir 子树，单根口径会把模型自己的临时目录判成越狱）；加的是**自己那一个**会话根而非整个 sessions 根，兄弟会话互不可达。`tst_pathguard` 21 条 |
| `CompactTranscript.h` | 压缩转写回溯纯头（`CompactTranscript` 命名空间，header-only **不占测试目标 TU 清单**，`tst_compacttranscript` 直接 include 生产头并以注入的 `TranscriptReader` 回调绕开真实文件系统）。把 `history.json` 里的「压缩摘要消息」展开成被它替换掉的前序消息，供会话重启**只读回放**——不进 `m_messages`、不进请求上下文，token 计量/`persistHistory` 落盘/前缀缓存全不受影响。`isSummaryMessage` 判据 = role=user + `[Compacted]`/`[Reactive compact]` 前缀 + 含 `\nFull transcript: ` 行（第二条件防普通 user 消息误判；label 单源对齐 `CompactManager::summaryMessage` 两调用点，新增压缩入口换 label/模板必须同步本判据）；`transcriptPath` 解析该行路径（取最后一个匹配，`(transcript unavailable)` 占位返空串）。**路径守卫**：必须落 `<会话根>/.transcripts/` 直下且形如 `transcript_*.jsonl`（摘要正文可被污染，禁据此读任意文件）；防环（同一 transcript 只展开一次，QSet）+ 深度上限 `kMaxExpandDepth=8`；**任何一步不成立都保留原摘要消息降级**（绝不因回放丢消息、绝不因回放失败打断会话加载）。宿主经 `AgentLoop::replayHistoryMessages()` 读回只喂 `ChatSessionPage::replayHistory`。40 条零夹具 |
| `ActivityLedger.h` | 会话每日 token 消耗账本纯头（`ActivityLedger` 命名空间，header-only **不占测试目标 TU 清单**，`tst_activityledger` 直接 include 生产头）。`DayTotal{prompt, completion, cached, reasoning, calls}` + `totalProcessed() = prompt + completion`（对齐 GitHub contribution graph 单值语义）；`levelForTokens` 五档阈值 0/<5k/<20k/<50k/<100k/≥100k（固定值不落配置）；`dateKey`/`dateFromKey` **严格 ISO 口径**（10 字符、破折号在第 4/7 位、其余全数字、`QDate(y,m,d).isValid()`——Qt `fromString(Qt::ISODate)` 对 `2026/11/07` 斜杠写法宽松接受，判据必须自己拆到字符层）；`accumulateSnapshot` 无效快照丢弃（`prompt<=0`，与 `UsageTokens::fromUsage` 同源）；`readDaily` 缺失/坏 JSON/非对象/非日期键一律降级空表（**不抛不崩不猜**）、负数字段回 0；`writeDaily` 走 `QSaveFile` 原子；`aggregate` 跨会话合并同一天字段相加、`windowFilter` 闭区间。落盘点唯一：`AgentLoop::appendActivityLedger` 在 `adoptUsageAnchor` 里紧随 `m_usageTotals.add` 之后（同一份 Snapshot、两路账本一源），**主循环与队友回合两条路都落账**（s17.9 补）。NewChatPage 热力图数据源。58 条 = 恒跑 42 + 夹具 16 |
| `HeatmapGrid.h` | 热力图网格算术纯头（`HeatmapGrid` 命名空间，header-only **不占测试目标 TU 清单**，`tst_heatmapgrid` 直接 include 生产头）。`ContributionHeatmap` 的 53 列 × 7 行 = 371 天滚动满矩形的日期↔坐标映射：`windowStart`（首日 = today−370）/ `cellOf`（日期→坐标，窗口外返 {-1,-1} **不钳位不回绕**）/ `dateOfCell`（坐标→日期，末格 (52,6) 恒为 today）/ `monthLabels`（底部月份标签选列：含该月 1 号的列贴标、按**月号**去重、首列补标让位给同号 1 号锚点——371 天恒覆盖 ≥13 个月份实例而月号只有 12 个，起点月与终点月同号跨年，不去重底部必出现两个相同月份；按年+月去重抓不到这种重复；保留补标又会与相邻月标签挤压（月界距首日最近仅 1 列），让位后相邻标签恒 ≥4 列，s18 两阶段修）。**不按日历周对齐**（旧口径「列 = 按周日对齐的日历周 + 绘制时跳过未来日期」在非周六留尾列破口，s17.9 废除，绘制循环不得再跳过未来日期，`dayOfWeekIndex` 符号已不存在）。`kWeeks`/`kDaysPerWeek` 是 53/7 单源，视图只别名不重写（避免第二个真值源）。这套算术此前**零直测**（宿主 TU 是 QWidget 进不了 ctest）且出过两次用户可见故障（尾列留空破矩形 / 底部重复月份标签），off-by-one 只能靠断言钉。39 条零夹具（s18 月份标签去重批 +12；上一登记值 27 为 s17.9 抽头批原值，历史保留） |
| `MemoryRules.h` | 记忆子系统纯判据单源（`MemoryRules` 命名空间，header-only **不占测试目标 TU 清单**，`tst_memoryrules` 直接 include 生产头）。`parseFrontmatter` / `memorySlug` / `normalizedMemoryText` / `messageText` / `extractJsonArray` / `validateMemoryRecord` / `shouldStoreMemory` / `memoryDocument` 八判据，连同私用的 `memoryTypes()`（四类白名单）/ `temporaryMemoryMarkers()`（16 条临时标记）/ `collapseWhitespace` 等一并搬进**具名 namespace 且逐个 `inline`**（helper 不放匿名 ns 是硬要求：`memoryTypes`/`collapseWhitespace` 还被本头之外的 `MemoryManager` 调用，且 header-only 里的匿名 ns 会让函数内 `static` 身份按 TU 分裂）。抽头动机：这些判据此前全在 `MemoryManager.cpp`（include `QOpenAi.h` 链 `Qt6::Network` 进不了 ctest），整套记忆判据零直测，而它们错了不报错只让记忆静默变少或变脏（括号配对一漂整批丢条目、slug 一撞后写覆盖前一条、去重键一失效重复无限增长）。`MemoryManager` 侧保留同名 static 成员做**一行转发**，调用点与签名不变；`shouldStoreMemory` 改吃轻量 `ExistingMemory{name, description, body}`（纯头不能反向依赖链 QOpenAi 的 TU），映射在转发处一次完成。**实测偏差登记**：源码注释原称非法 JSON 转义会回落，实测 Qt 宽容接受并静默吞掉反斜杠（**不**回落），断言按实测行为钉、注释同步改正。76 条零夹具 |
| `ToolSchema.h` | 工具 schema 单源（`ToolSchema` 命名空间，header-only **不占测试目标 TU 清单**，`tst_toolschema` 直接 include 生产头）。`createToolsDefinition()` 逐工具构造发给服务端的 OpenAI function 定义，共 27 个（26 个来自 `ToolNames` 常量 + workflow 信封委托 `WorkflowManager::toolInfo()`）。函数体内 `static const QJsonArray cached` 的惰性缓存在 inline 函数里仍是跨 TU 单实例；`makeTool` lambda 只支持平铺参数并**恒写 `required`**，嵌套 array 与「无参数」两类必须手工构造（勿「顺手补齐」required）。抽头动机与硬约束：本体此前长在 `AgentLoopPrompt.cpp`（经 `AgentLoop.h` 传递 include `QOpenAi.h` 链 `Qt6::Network` 进不了 ctest），整套 schema 零直测，而它是「本地全绿、线上炸」的典型——`required` 多写一个键就整个会话第一个请求 400，编译与既有测试却全绿。`AgentLoop::createToolsDefinition()` 退化为**一行转发**，调用点与签名不变；**本头绝不 include `AgentLoop.h`**（那会把 moc + QOpenAi 依赖拖进只链 `Qt6::Core` 的测试目标）。55 条零夹具 |
| `SessionKeys.h` | 会话键算术单源（`SessionKeys` 命名空间，header-only **不占测试目标 TU 清单**，`tst_sessionkeys` 直接 include 生产头）。`keyForSeq(int)` / `keyForDataId(QString)` 两族键拼装（键常量沿用 `NavKey::SessionKeyFmt` / `SessionPrefix` 单源，禁止另造字面量）、`isSessionKey` 归属判定、`seqFromKey` 反解（只认无前导零的自增产物）、`isGeneratedDataId` 形状判定（8 位小写 hex）、`collidesWithSeqKey` 撞车谓词。抽头动机：`SessionRegistry.cpp` include `<QWidget>` 与 `ChatSessionPage.h`（硬绑 Qt6::Widgets + FluentUI 进不了 ctest），而键撞车后果不可逆（`insert` 用 `QHash::operator[]` 就地覆盖 → 两条会话共用一个 key 后，右键删除兄弟会话会连带把**另一会话的 `sessions/<dataId>` 整目录**递归抹掉）。`SessionRegistry::nextSessionKey()` / `::keyForDataId()` 退化为**一行转调**，签名与调用点不变。**本批证伪一条注释不变量**：原写「hex 永不等于十进制自增，避免键冲突」的**理由是错的**（数字串是 hex 子集），真正兜住的是数量级巧合（自增号到 10^7 前不足 8 位）；手改 config.json 或旧 index.json 迁移进来的短 dataId 可直接撞车，而 `createSession` 路径上**没有** `contains()` 卫兵。本头只算键、不改行为（加不加卫兵待裁决）。45 条零夹具 |
| `ProjectInstructions.h` | 项目指令文件（AGENTS.md）判据单源（`ProjectInstructions` 命名空间，header-only **不占测试目标 TU 清单**，`tst_projectinstructions` 直接 include 生产头）。`candidatesFor(workDir, configRoot)` 两级候选顺序（**工程根在前、存储根在后**，空根/全空白根跳过防拼出 `/AGENTS.md` 落到当前驱动器根）、`selectExisting(candidates, exists)` 命中即止（存在性经 `std::function` 注入，本头零 IO）、`signatureFor(path, mtimeMs, sizeBytes)` 惰性缓存签名、`formatSection(path, text, budgetChars)` 注入段拼装与字符预算截断（空 path/空正文/全空白正文一律返回空串 = 空段省略；超限只下发前缀并在段标题标注 `first N of M characters shown`，静默吞尾会让模型宣称「已遵循项目全部约定」）。抽头动机：读盘侧 `AgentLoopInstructions.cpp` 链 `AgentLoop.h`→`QOpenAi.h`→`Qt6::Network` 进不了 ctest，而四条判据全是「跑得出但喂错料」——候选顺序写反会让全局那份静默压住项目根规则且零报错，命中即止写成叠加会让两段指令互相打架。**本头绝不 include `AgentLoop.h`**。49 条零夹具 |
| `UpdateRules.h` | 自更新判据单源（`UpdateRules` 命名空间，header-only **不占测试目标 TU 清单**，`tst_updaterules` 直接 include 生产头）。`parseVersion`/`compareVersions`/`isUpdateAvailable`（**数值段比较**，字典序在补丁位上恰好反向；形态为「主」一段（s18 起启用）/「主.补丁」两段/「主.补丁.修订」三段（第三段自 s17.13.1 起启用），缺段按 0 计故 `s18 == s18.0`、`s17.13 == s17.13.0`；**出参个数即调用方声明的形态宽度**——只取两指出参遇三段一律拒，绝不静默截断，这条是 `shouldCleanResidue` 不误删旧体唯一副本的前提）、`pickAssetName`（只认 `-win64.zip` 后缀且含目标 tag，多命中取排序首项防 API 顺序漂移）、`normalizeDigest`/`digestMatches`（非 64 位十六进制、`sha512:` 前缀、空值一律归零，比对对空串恒不等 = **缺校验和即拒绝换体的 fail-closed**）、`normalizeEntryPath`/`derivePackageRoot`/`mapEntryToInstallRel`（zip-slip 防线：`..` 段、绝对路径、盘符全拒；顶层目录走**白名单** `bin/plugins/translations`，数据根 `config/` 与 `log/` 结构上进不来；包内根**从条目清单推导**而非写死；目录条目判定必须读**原始串**——归一化会吃掉尾斜杠）、`planSwap`/`rollbackOrder`/`cleanupOrder`（**任一条目落不进白名单即整批作废**而非跳过；回滚逆序、清理正序，两者刻意不同）、`backupSuffix`/`markerSchemaVersion`/`writeMarker`/`parseMarker`/`isDeletableBackupPath`（marker 内容来自磁盘而后果是删文件：**一条不可删即整体 invalid**）、`shouldCleanResidue`（版本闸门——备份是旧版可执行体的**唯一副本**，对不上就宁留不误删）、`releaseApiUrl`/`userAgentFor`/`assetSizeOrUnknown`（UA 是硬要求：本机实测不带 UA 被 GitHub 直接拒答；缺 size 归 -1 而非 0）、`progressPercentOrUnknown`（**下载进度分母判据单源**：`total<=0` 或 `received<0` 一律回 -1 = 未知，UI 走不定态旋转，**绝不拿 0 当分母渲染出看似合理的百分比**；`received>=total` 先钳 100 再乘 100 避开 INT64_MAX 溢出；整数截断而非四舍五入，99.6% 不许显示 100%）、`canSelfUpdate`/`refusalReason`（开发态布局闸门：exe 旁无 `Qt6Core.dll` 即拒，防发布包盖进构建树）。抽头动机：IO 侧 `UpdateManager.cpp` 链 `Qt6::Network` 进不了 ctest，而自更新是「一次做错、用户环境不可恢复」的操作；**定义顺序硬约束**——inline 自由函数无两阶段查找，`backupSuffix`/`normalizeEntryPath` 必须先于其使用者 `planSwap`/`derivePackageRoot` 定义。198 条零夹具 |
| `UiScale.h` | 显示尺度判据单源（`UiScale` 命名空间，header-only 纯头：无 IO、无 GUI、无 tr()，仅 QtCore——`QRegularExpression` 在 Core5Compat 测试目标不可用，故 QSS 扫描全手写字符判定；`tst_uiscale` 直接 include 生产头）。三键单源 `kFontScaleKey`/`kDpiRoundingKey`/`kWindowGeometryKey` + 四组判据：① **字号倍率** `fontScalePercent`（整数百分比 [80,200]，缺失/非整数/越界一律回退 100 = 不缩放，用 `toLongLong` 防 `toInt` 溢出）+ `runtimeFontScalePercent()` **启动快照**（函数内 static + inline 跨 TU 唯一，`App.cpp` 在 QApplication 构造后写一次）+ `scaledSize`（px 整数量子 `(base*percent+50)/100` 四舍五入非截断、下限 1 绝不出 0px 隐形文字、base≤0 原样透传）+ `scaled` 便捷口；② **高 DPI 取整策略** `dpiPolicyFromName`/`dpiPolicyName`（Round/Ceil/Floor/RoundPreferFloor/PassThrough，大小写不敏感 + trim，空/未知回退 PassThrough = Qt6 内建默认，**必须在 QGuiApplication 构造前落定**——构造后调用只 qWarning 不生效，故读值走 `AppSettings::bootstrapValue`，是「读一律 value()」的唯一例外）；③ **窗口几何** `encodeGeometry`/`decodeGeometry`（`saveGeometry()` 不透明二进制 base64 化防乱码配置值，空/全空白/垃圾串返空字节 = 无存档保持默认尺寸、**绝不猜一个尺寸**，格式合法性由 `QWidget::restoreGeometry` 自带校验兜）；④ **QSS 字号缩放** `scaleQssFontSizes`/`findPixelLiteral`（`ThemeAware::bind` 的唯一改写点，只认 `font-size`/`font:` 两属性名且只取值区**第一个** px 字面量，跳过 `font-weight:600`/`line-height:1.5` 与 `/* 注释 */`，值区不跨 `;`/`}`，100% 原样返回同对象；刻意不改 FluentUI 自带 QSS）。**字号有三个生效面**（应用字体 / 本仓 QSS / 代码设定字号），第三面（如 `WorkDirPathBar::secondaryPixelSize`）不经 QSS 收口点必须自己接，且**必须读启动快照、禁现取 `fontScalePercent()`**（否则①②钉在启动值而③跟着 ini 实时变成半生效）。68 条零夹具 |

---

### 3. AgentLoop 家族：单类多 TU 的职责地图

`AgentLoop.h` 是**唯一类声明**；17 个 `.cpp` 各承载一组成员方法定义（不是多个类）。这样既保住了
「一个状态机一个所有者」的语义，又让单文件回到可读体量。**新增方法请按 §10 的决策树落位，不要
往 `AgentLoop.cpp` 里堆。**

| TU | 行 | 负责什么 | 关键定义 |
|---|---|---|---|
| `AgentLoop.cpp` | 658 | 生命周期与会话身份：构造/析构、workDir / sessionDataId / sessionDataRoot / model、回合入口与终局、**token 计量单源**（usage 锚 + 增量外推；s18 审计批：兜底与 overhead 的 schema 项收拢进 `toolsSchemaTokens()` 单源——静态 27 工具惰性缓存 + 动态 `mcp__` 现估，MCP 接入后兜底不漏计） | `run()`、`stop()`、`sessionDataRoot()`（`:199`）、`adoptUsageAnchor()`、`estimatedContextTokens()`、`toolsSchemaTokens()` |
| `AgentLoopRequest.cpp` | 679 | 一次 LLM 请求回合：压缩前导 → 流式请求 → 工具批推进 → 记忆沉淀链 | `startChatRequest`、`doStartChatRequest`、`applyCompactPipelineAsync`、`continueWithToolResults`、`runNextTool`、`startMemoryChain`；**s15**：请求组装消费恢复态（`m_recoveryCurrentModel` 三元定 model、`m_maxTokensEscalated` 定 max_tokens）、max_tokens 两拍续打、终局 `[Error]` 入账、双 lambda fallback 粘滞 |
| `AgentLoopPrompt.cpp` | 244（s17.9 工具 schema 抽头批前为 717，490 行 schema 搬入 `ToolSchema.h`；s17.11 注入块加第 0 段 +9 行） | **静态** system prompt（只由 workDir / 会话根 / 静态文字决定，全会话字节恒定）+ 请求尾部 `<agent_context>` 注入块（**s17.11** 项目指令 AGENTS.md 段 / 技能目录 / 记忆索引 / 召回记录 / Connected MCP servers note / **s16** 工作流注册清单段）+ 27 工具 function 定义（含 s13 团队 7 件 schema + s14 connect_mcp schema 与团队纪律/MCP 静态段 + **s16** workflow schema（信封单源 `WorkflowManager::toolInfo()`，静态表内以原型对象取值免双写）与 workflow 编排纪律段（lcc prompt_workflow 逐字转译 + lite 尾三句）；**禁翻区**，`QStringLiteral` 不包 `tr()`） | `makeSystemPrompt`、`rebuildSystemPromptMessage`、`makeContextInjection`、`AgentLoop::buildContextInjection`、`createToolsDefinition` |
| `AgentLoopTools.cpp` | 462 | 工具分发：`executeTool` 分流、handler 表、统一收口 `onToolFinished`、成败判定单源（s15：`isToolFailure` 对 Permission denied 改前缀判定；bg 派发处不触发 PostToolUse；**s16**：workflow 为 executeTool 第三异步特判 `startWorkflowRun`，toolSummary 取工作流名） | `executeTool`、`onToolFinished`、`isToolFailure`、`AgentLoopDetail` 工具段定义（`makeToolResult` `:116`、`snapshotMessages` `:125`） |
| `AgentLoopFileTools.cpp` | 393 | 沙箱文件工具（同步本地 IO）：逃逸判定 + read/write/edit/glob，全静态，宿主与子代理各传各的 workDir；edit/write 的行尾与编码防线见 `LineEnding.h`；edit 另有三道防线（空 `old_string` 拒绝 / 体量上限 `kEditFileMaxBytes` 拒绝 / 多处命中歧义拒绝） |
 `safePathIn`、`runReadFileIn`、`runWriteFileIn`、`runEditFileIn`、`runGlobIn` |
| `AgentLoopBash.cpp` | 177 | bash 异步执行链与后台任务结果收割；**s15** bg 的 PostToolUse 时点由派发搬至 `QProcess::finished` 回调、以真实终态 output 触发（按值捕获 toolCall 零台账改动；FailedToStart 不触发=偏差登记） | `executeBashAsync`、`injectBackgroundResults` |
| `AgentLoopPermission.cpp` | 166 | 权限门：硬拒绝黑名单、破坏性命令升级判定、ASK 规则、用户裁决续跑 | `checkDenyList`、`checkPermissionRules`、`resolvePermission`、`AgentLoopDetail::bashDenyList` 定义 |
| `AgentLoopHooks.cpp` | 209 | 生命周期钩子注册表：注册顺序即执行顺序，四个 trigger（UserPromptSubmit/PreToolUse/PostToolUse/Stop）**首个非空返回短路**；**s15** ASK 门与 `mcp__` 策略门在异步回合（`m_asyncTurn`）改返 `RecoveryRules::asyncApprovalDenied()` 定值拒串（fail-closed 不弹卡，deny 表与 granted 短路顺序不动）；**s16** workflow 三态策略门位 `mcp__` 门后（deny→定值 "Permission denied by host policy" 硬拒 / confirm→ASK 卡且异步回合复用 asyncApprovalDenied / allow 放行；未登记名与空名 fail-closed 归 confirm）；**s17.2** 审批总开关 `AgentConst::permissionApprovalEnabled()`（config.json 键 `permissionApproval`，每次工具调用现取）为 false 时三道询问门（ASK 规则 / `mcp__` confirm / workflow confirm）整体跳过、异步 fail-closed 随之不可达，而 bash deny 表与 workflow 的 deny 定值硬拒**不受开关影响**（撤「问不问」非「许不许」） | `registerBuiltinHooks`（内置：`context_inject` / `permission` / `log_before` / `log_after`）、四个 `trigger*Hooks` |
| `AgentLoopHistory.cpp` | 236 | `history.json` 落盘与磁盘恢复；**不再碰会话索引**（原 `lastActiveMs` 续活链已随索引并入 config.json 一并删除，理由见本 TU 末尾注释）；条目登记归 `LiteHarness::createSession` | `persistHistory`、`loadSavedHistory` |
| `AgentLoopSkills.cpp` | 200 | 技能扫描与 `load_skill`；技能目录**全局共享一套** `<config>/.lite-harness/skills`（路径单源 `LitePaths::skillsDir()`；用户裁决：不再按 workDir 隔离，跨会话、跨项目、跨队友 worktree 同一份）| `scanSkills`、`skillsCatalog`、`runLoadSkill` |
| `AgentLoopInstructions.cpp` | 66 | **s17.11 项目指令文件（AGENTS.md）读盘侧**：两级候选 `<workDir>/AGENTS.md` → `<configRoot>/AGENTS.md` 命中即止、整读、按 路径+mtime+size 签名惰性缓存、命中来源 `LOG_INFO` 留痕；判据（顺序/命中即止/判空/截断/签名）全在纯头，本 TU 只做 IO 与接线（链 `AgentLoop.h`→`QOpenAi.h`→`Qt6::Network`，因而不进 ctest 清单）| `AgentLoop::projectInstructions() const`（`mutable` 缓存成员，承 `m_toolsSchemaTokens` 先例）|
| `AgentLoopTodo.cpp` | 169 | `todo_write`（无状态：只校验入参并渲染面板文本，成功即以本次快照发 `todoUpdated`）；校验+快照归一抽为 `normalizeTodoSnapshot` 静态单源，**历史回放共用**（s17 缺陷修复：回会话重建侧栏任务清单与快照卡，禁回放侧另写口径） | `renderTodos`、`normalizeTodoSnapshot`、`runTodoWrite` |
| `AgentLoopSubAgent.cpp` | 80 | `task` 子代理的启动与统一收口（子代理信号直连转发为主循环信号） | `startSubAgentTask`、`cancelSubAgent` |
| `AgentLoopCron.cpp` | 86 | 三个 cron handler 与空闲边界交付；台账与匹配在 `CronSchedulerManager`，本文件只做宿主侧接线；**s15** 另承载 `tryDeliverBackgroundWakeup`（第 4 事件源卫兵+空 prompt 唤醒） | `runScheduleCron`、`runCancelCron`、`runListCrons`、`tryDeliverCron`、`tryDeliverBackgroundWakeup` |
| `AgentLoopTeam.cpp` | 706 | **s13 团队宿主接线单 TU**：`initTeamEngine` 六注入装配（launcher/worktreeCreator/permissionCheck/hooksTrigger/toolAdapter×5）；`leadToolCwd` 租约感知 cwd 单点（有租约取 `assignmentCwd` 含 worktree 解析、无租约回落宿主 workDir）；三注入边界（`run()` 入口与批尾 `injectTeamEvents`、自然终局 `leadTurnEndSettlement` 先于 finalize/persist）+ 非自然终局 `settleLeadLease`；**`tryDeliverTeamEvents`** Gate③ MAJOR-1 空闲唤醒收割（挂 `m_cronTick` 1s 节拍、先于 `tryDeliverCron`——lcc 邮箱优先口径；仅 `!m_running` 且 lead 邮箱有信时 harvest→`scheduledUserMessage` 自动开回合，与 cron 同路 GUI 零改动；**s15**：emit 前置位 `m_pendingAsyncTurn=true`（异步回合 fail-closed），队友工具适配器在 `triggerPreToolUseHooks` 窗口同栈临时强制 `m_asyncTurn`——s14 在册「Lead 保留 ASK 卡=增强非缺陷」裁决**已废止翻转**，无人值守回合不得挂永久审批卡）；队友回合环 `onTeammateTurnRequested`/`onTeammateFinished`（镜像 SubAgent 先例）；`settleTeamOnExit` 析构清算；`teammateToolsDefinition()` 队友面 10 件 schema（D5 与主表分开、禁翻区） | `initTeamEngine`、`leadToolCwd`、`injectTeamEvents`、`tryDeliverTeamEvents`、`leadTurnEndSettlement`、`settleLeadLease`、`onTeammateTurnRequested`、`settleTeamOnExit` |
| `AgentLoopWorkflow.cpp` | 181 | **s16 workflow 宿主接线单 TU**：`startWorkflowRun` 单飞卫兵（占用即回填 "Error: another workflow run is already active"）+ runner 桥（`QOpenAi::AsyncRequest::sendText` focused system prompt + max_tokens 2000，复用 s15 传输恢复链；tokens 无 usage 回读走 `AgentConst::estimateTokens` 字符折算=偏差；content 原样回、由 Lane B `agent()` 自 parse）+ out 槽三件套指针匹配卫兵（延迟回填，lcc handler 内同步阻塞编排不采=核心偏差）；`cancelActiveWorkflow`（stop() 第六路收口 + 析构同点）：先清槽 → requestCancel → 在途 cancel → 半批 "(stopped)" 直写历史保 tool_use 配对——**全路径零 emit**（析构窗口红线，与 settleTeamOnExit 一族） | `startWorkflowRun`、`cancelActiveWorkflow` |
| `AgentLoopGoal.cpp` | 230 | **s17 goal 宿主接线单 TU**：`gateGoalTerminal(finalMsg)` 终局门（首行 `!m_goal.active()` 短路=无 goal 会话逐字节等价红线；插 `m_messages.append` 之后、`triggerStopHooks` 之前——goal 先于 summary 序）——NeedEval 挂起终局走 `startGoalEvaluation`（三级模型回退 `goalEvaluatorModelIdValue()`→`m_recoveryCurrentModel`→`m_model`，`QOpenAi::AsyncRequest::sendText` max_tokens=512 无 tools；先置 `m_goalPendingFinalMsg`/`m_goalEvalInFlight` 再发送；回调 QPointer self + generation 双卫兵拒迟到回填；req==null 兑底走 `onEvaluatorError` 合流记账）；`applyGoalDecision`：Block→`continueTurnAfterStopHook(blockContinueText)` 回灌续跑（清 pending），Achieved/Failed/Limit/Error→`emit goalNotice(m_goal.status())` 后经 `triggerStopHooks`→force 续跑或 `finishTurnWithoutTools`（挂起终局入账收口）；`cancelActiveGoal`（stop() 第七路收口 + 析构同点）：清旗→弃挂起终局→在途 cancel→清 handle——**全路径零 emit**（析构窗口红线，goal 门不产生 tool_use 无半批配对；与 settleTeamOnExit/cancelActiveWorkflow 一族）；**s17 P1**：`goalStateSnapshot()` 观测面组装（condition/iterations/consecutiveBlocks/blockCap/tokensUsed/lastReason，无 goal 回空对象；刻意不含 elapsed 防静息期 tick 签名秒秒变） | `gateGoalTerminal`、`startGoalEvaluation`、`applyGoalDecision`、`cancelActiveGoal`、`goalStateSnapshot` |

**AgentLoop 对外信号面**（`AgentLoop.h` signals 段，共 23 个，UI 只认这些）：`thinkingDelta`、`textDelta`、
`toolStarted`、`toolOutputReady(…, ok)`、`subagentProgress`、`permissionRequired`、`todoUpdated`、
`memoryPhaseStarted`、`memoryChainFinished`、`scheduledUserMessage`、`finished`、`runningChanged`、
`error`，加 s13 团队四件 `teammateProgress`、`teammateToolProgress`、`teammateSettled`、
`teamRosterChanged`（四者均有 UI 接线，见 §5 回填总表，不是内部信号），加 s17 `goalNotice`（goal 回显英文契约串）与 P1 观测面三件 `backgroundTasksChanged`、`cronJobsChanged`、`goalStateChanged`（载荷即引擎快照，1s tick 签名边沿，页面直传侧栏 setter），加 P1-9 流式观测两件 `streamRetryScheduled(attempt, totalAttempts, delayMs)`（退避重试已排期，单源在传输层 `QOpenAi::ChatStream::retryScheduled`）与 `streamFallbackModel(model)`（s15 529 过载换模型重发，与 `m_recoveryCurrentModel` 同条件、真切换才发）——两者只把此前对 UI 完全不可见的重试/回退显形，不改变其判据。另有一组**私有驱动信号**
（`startChatRequest` / `doStartChatRequest` / `continueWithToolResults` / `runNextTool`，
声明在 `AgentLoop.h` 的 private 段，`AgentLoop.h:265-272`）只用于把阻塞链拆成事件驱动续跑，不对外、UI 不得连接。

---

### 4. AgentLoopInternal.h：内部工具声明单源

`src/AgentLoopInternal.h`（80 行）声明 `namespace AgentLoopDetail` 的跨 TU 纯函数。**它不是对外
API**：只供 `src/AgentLoop*.cpp` 与两个友元 TU（`SubAgent.cpp`、`ChatSessionPage.cpp`）使用，其它
模块不得依赖其符号稳定性。

**归属规则（谁定义）**：原 `AgentLoop.cpp` 匿名 namespace 里被多单元共用的函数，按「谁最贴近其
语义谁定义」落位，本头只负责声明——

- `toolSummary` / `callToolName` / `callToolArgsText` / `parseToolCall` / `parseToolArgsText` /
  `callToolArgs` / `ToolCallView` → 定义在 **AgentLoopTools.cpp**（工具分发侧）
- `askPrefix` / `bashDenyList` → 定义在 **AgentLoopPermission.cpp**（权限门侧；`bashDenyList`
  三方共用：`checkDenyList` + 主循环 `executeBashAsync` + 子代理）
- `makeToolResult`（`:116`）/ `snapshotMessages`（`:125`）→ 定义在 **AgentLoopTools.cpp**（s13 协议
  装配单源：前者合成 `{role:tool, tool_call_id, content}`，断配对即上游 400；后者做会话历史 →
  wire messages 快照，全量 / mid(1) 口径仍由调用方显式表达，本函数不裁剪）
- `isContextOverflowError` → 定义在 **AgentLoopRequest.cpp**（`:21`；s13 由匿名 ns 提升为单源声明，
  主循环反应式压缩（s15 注：预算门 `m_reactiveRetries < 1`）与队友流错误侧 `AgentLoopTeam.cpp` 的 `isContextOverflowError(message) && guard->tryReactiveCompact()`（约 :477）共用同一判定；**s18** SubAgent 反应式压缩为第三消费点——`SubAgent.cpp` error 侧 `!m_reactiveCompactUsed && isContextOverflowError(msg)`，旗标成功 messageFinished 复位）

**三条禁令**

1. 不要再加 `AgentLoop::toolSummaryOf()` 这类**静态转发层**——曾经有（连同 `askPrefixOf` /
   `bashDenyList`），已删除，友元直接 include 本头取用（`AgentLoop.h:261` 的 `friend class ChatSessionPage` 注释记录了这次演进）。
2. 本头**不 include `AgentLoop.h`**：include 链单向，只含 QtCore 头，避免 UI/子代理为了一个摘要
   函数被迫吃下整个类。
3. 声明与定义不得重复：新增符号时同步更新本头 + 归属 TU + 本节说明，三处一起改。

---

### 5. 数据流：一条用户消息的完整往返

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

② 回合入口 AgentLoop::run（AgentLoop.cpp:273，本轮注释净 +1 行后实测）
**s17**：入口先过 `m_goal.parseInput(text)` 四类分流——Status/Cleared/Error 形 emit goalNotice 即返
  （**零模型早退**：不置 m_running、不入历史、不触 UserPromptSubmit，页面本地气泡回显）；Set 形以条件
  本身为 userMessage 走正常回合（引擎内部已 setGoal，宿主不二次调用防伪 replaced+代际双增）；None 形原样。
setRunning(true)（同步红线，先于任何异步）→ 快照 m_maxToolIterations → triggerUserPromptSubmitHooks
→ m_goal.beginQuery()（consecutiveBlocks 按 per-用户回合归零，**s15** 复位段旁、异步锁存之前）
→ user 消息入 m_messages（**s15**：空 userText 跳过 append——bg 唤醒空 prompt 回合不注入 user 消息，收割由圈顶 injectBackgroundResults 承载）→ injectBackgroundResults() → m_memory.loadMemoriesAsync(...)
→ done 里快照注入块 m_contextInjection = buildContextInjection(readMemoryIndex(), recalled)
  （**不重建 system**——system 全会话字节恒定，规格修1；**s17.11**：注入块首段 = `projectInstructions()`
   现读项目指令文件——两级候选 `<workDir>/AGENTS.md` → `<configRoot>/AGENTS.md` 命中即止不叠加，
   按 路径+mtime+size 签名惰性缓存，超 `kProjectInstructionsCharBudget`=16000 字符截断并在段标题
   如实标注「first N of M characters shown」；两级都不存在或正文全空白 → 该段省略，注入块与改造前
   逐字节一致。判据单源 `ProjectInstructions.h`，读盘侧 `AgentLoopInstructions.cpp`）
  → startChatRequest(messagesJson)

③ 请求链（AgentLoopRequest.cpp）
startChatRequest → applyCompactPipelineAsync（仅触发全量压缩时挂侧链，句柄落 m_sideRequest）
→ doStartChatRequest：组 body（model（s15 粘滞：m_recoveryCurrentModel 非空取之，否则 m_model）/ messages / tools=createToolsDefinition() /
  enable_thinking / reasoning_effort / temperature / top_p / max_tokens（s15 两拍：升档后 16000）/
  stream_options.include_usage=true）+ 尾部追加 <agent_context> 注入块
→ QOpenAi::chat().createStream(request, this)
   · ChatStream::thinkingDelta → 直连转发 AgentLoop::thinkingDelta
   · ChatStream::textDelta     → 直连转发 AgentLoop::textDelta
   · ChatStream::usageReceived → AgentLoop::adoptUsageAnchor（token 锚，见下「⑦ 上下文计量」）
· ChatStream::messageFinished(fullMsg, finishReason) →（**s15** 前置两拍判长：finishReason==length 第一拍升档 8000→16000 重打、半截不入账；第二拍半截入账 + user continuationPrompt 续打 ≤2、预算耗尽走成功终局不发 error；另 fallback 换模粘滞读取 m_recoveryCurrentModel）→ 无 tool_calls：**s17 gateGoalTerminal(fullMsg)**（active goal 时终局先过 goal 门：Defer/Allow 放行继续原链；NeedEval 挂起本终局→AsyncRequest 评估器往返→回填经 applyGoalDecision——Block 回灌 blockContinueText 走 continueTurnAfterStopHook 续跑、Achieved/Failed/Limit/Error emit goalNotice 后收口本终局、req==null 兑底经 onEvaluatorError 合流；无 goal 首行短路逐字节等价）→ triggerStopHooks → cron 交付定稿
        → setRunning(false) → persistHistory() → emit finished → emit memoryPhaseStarted
        → startMemoryChain（异步沉淀，终态发 memoryChainFinished）
    · 旁路：**s17** max_turns 两出口与流错误终局不经 goal 门（非「模型想停」，防白花评估与污染 blocks 账；差量登记=AgentLoopGoal.cpp 头注释⑤——lcc 撞上限出口仍跑 Stop 链保 summary 播报与 goal 交还日志，lite 整链不跑，goal 层效果一致）
   · ChatStream::error(msg) → cancelSubAgent →（可压缩则反应式压缩重试——**s15** 旗标 per-用户回合一次性、复位仅 run() 入口）→ 终局 append assistant "[Error] "+msg 入历史（persistHistory 前，仿 MCP assemble 先例；emit error 保留=用户裁决偏差，lcc 静默 return）

④ 工具批（continueWithToolResults → runNextTool → executeTool）
parseToolCall（非法即 onToolFinished 回填错误文本）→ compact 前部特判
→ triggerPreToolUseHooks（内置 permission 钩子在此判定）→ 三分流：
   · "ASK:" 前缀 → m_awaitingPermission=true + emit permissionRequired + **return，队列暂停**
     （**s15** 异步回合 fail-closed：m_asyncTurn 锁存时 ASK 门与 mcp__ 策略门改返定值拒串
     RecoveryRules::asyncApprovalDenied()——无 ASK: 前缀，走下方硬拒绝通道，不弹卡不挂起队列）
   · 硬拒绝文本 → onToolFinished（错误结果回填，不发 toolStarted）
    · 放行 → emit toolStarted → bash 走 executeBashAsync（run_in_background 走后台台账）/
             task 走 startSubAgentTask / workflow 走 startWorkflowRun（**s16** 第三异步特判：
             单飞卫兵→runner 桥→延迟回填，lcc 同步阻塞编排不采）/ 其余查 handlers 表同步执行
   → triggerPostToolUseHooks（**s15**：bg 分支派发处不触发、就地返回；钩子搬 QProcess::finished
     回调以真实终态 output 触发，见 §5-④ bash 行与 BackgroundTasksManager）
   → onToolFinished：emit toolOutputReady(name, summary, output,
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

⑦ 上下文计量（token 锚定，UI 占用条与压缩门槛同源）
ChatStream::usageReceived(usage) → AgentLoop::adoptUsageAnchor：prompt_tokens>0 才采纳，
  记 m_tokenAnchor=服务端真值、m_tokenAnchorCount=发送点历史条数、m_anchorInjectionTokens=
  发送点注入块估算，并清 m_historyRewrittenSinceAnchor
AgentLoop::estimatedContextTokens()：
  · 锚有效 → 锚真值 + 其后新增消息逐条 estimateTokens + 注入块变化量（当前 − 锚时）。
    锚路径**不再另计 overhead**——prompt_tokens 已含 system/tools/注入的发送点真值
  · 锚失效（首回合未回读 / 压缩改写历史 / 恢复 / 换 workDir / 换目录）→ 本地全量
    CompactManager::estimateTokens(m_messages)（含 system[0]）+ tools schema + 注入块
    （schema 项经 toolsSchemaTokens() 单源：静态 27 工具惰性缓存 + 动态 mcp__ 现估，
    兜底与 overhead 拆分共用，s18 审计批前 MCP 接入后兜底会漏计动态项）
消费三处，同一个函数：ChatSessionPage::refreshContextUsage → SessionSidebar::setContextUsage
  （条与条下数字同节并置，数字自 ChatStatusStrip 迁入）——比例算术单源 UsageTokens::usageRatio，
  一次计算喂两个视图，同一时刻不会给出两个读数；数字画法同样单源 `UsageTokens::formatCompact`（行内 K/M 紧凑，225px 侧栏里全量数字必被省略号截断；tooltip 仍走 QLocale::c() 千分位精确值——紧凑是读数、精确是账目，成对出现才不算丢信息）
AgentLoopRequest 压缩入口门槛 conversationTokens > T'（T'=contextTokenBudget()−overhead）
字符→token 折算单源 AgentConst::estimateTokens（CJK 1:1 / 其余 4:1，见 AGENTS「数据与路径」；s17.9 起 `kCharsPerTokenBudget` 已退役——预算不再二次换算）

⑦b 上下文消耗（与 ⑦「占用」语义正交：占用=窗口现在多大、会被后续回读覆盖；
   消耗=已经花掉多少、只增不减。同一条显示里混用两者会互相误导）
同一 usageReceived 载荷的第二个消费者：adoptUsageAnchor → UsageTokens::fromUsage → Totals::add
  · 主循环：AgentLoop::adoptUsageAnchor 内累加，且走在锚的 prompt_tokens>0 门**之前**——
    锚还要求发送点快照自洽，而消耗是既成事实，usage 有效就该记账
  · 队友：AgentLoopTeam 的 usageReceived lambda 累加进**宿主** m_usageTotals，不记在
    TeammateRuntime 身上——runtime 随清算消亡（settleTeamOnExit / cancel→deleteLater），
    记在它身上等于账本跟着尸体进土，已花的 token 会从累计里消失。QPointer guard 只守锚，
    账无条件走（这就是 guard 与本行的分工）
  · 派生：hitRatio = cached/prompt（上界钳 1.0）；uncachedPrompt = prompt-cached（钳非负）
消费一处：ChatSessionPage::refreshContextUsage → ChatStatusStrip::setTokenUsage
  （与占用同一刷新时机、同一数据源，不另起信号链）；calls==0 即整行隐藏——
  显示「0 token」会被读成「没花钱」，而真相是「还没拿到数据」，两者对成本判断作用相反；画法同 ⑦——行内 prompt/completion 走 `UsageTokens::formatCompact`（K/M），tooltip 逐条摊开的明细仍是千分位精确值
**部分口径**：仅主循环 + 队友回合（带 include_usage 的 ChatStream）；压缩摘要/记忆链/
  workflow runner 走 AsyncRequest 无 usage 通道，不在账上。tooltip 必须声明这条边界

⑦c 输出速度（与 ⑦b「消耗」正交：消耗=已经花掉多少，速度=模型吐字有多快。两者共用同一
   usage 载荷，但分母是时间——窗口取错时数字照样算得出、照样显示，含义却相反）
三个时刻全由宿主观测（纯头不读时钟）：t0 = createStream 返回、t1 = 本轮首个增量、t2 = usage 末帧
  · 生成窗口 = t1→t2（**不含 TTFT**）；TTFT = t0→t1 单独记录、单独显示，不混进速率
  · 流内重试退避发生在 t1 之前 → 计入 TTFT、不计入生成窗口；起表每请求重开，重试不重开
  · 纯 tool_calls 回合无增量起点（firstDeltaMs=-1）→ 窗口作废，不把 TTFT 冒充生成速度
  · 聚合 = Σtokens ÷ ΣgenerationMs（不是各轮速率的算术平均，否则 1 token 窗口与 2000 token
    窗口等权）
消费一处：ChatSessionPage::refreshContextUsage → ChatStatusStrip::setOutputRate（与消耗行同一
  刷新时机、同一数据源，不另起信号链；usage 末帧到达即收窗，恒先于 messageFinished）；
  calls==0 即整行隐藏——显示「0 tok/s」会被读成「模型慢」，而真相是「还没拿到能定窗的数据」
**口径边界比消耗账更窄**：仅主循环请求；队友回合增量不经宿主信号面、压缩/记忆/workflow 走
  AsyncRequest（无 usage 通道），都无从定窗。tooltip 必须声明这条边界
```

**回填 UI 总表**（全部集中在 `ChatSessionPage::wireAgent()`，别处不接 AgentLoop 信号）

| AgentLoop 信号 | 连接行 | 落到 |
|---|---|---|
| `finished` | `:320` | 兜底 `addMessage`（无气泡时）+ 侧栏权限态复位 + `refreshContextUsage()` |
| `error` | `:340` | `dismissPendingPermission()` + `finishStreaming()` + 一条 `*Error:* %1` |
| `thinkingDelta` / `textDelta` | `:384` / `:393` | 当前气泡 `appendThinkingText` / `appendText` |
| `toolStarted` | `:434` | `appendToolStart`（事前 live 卡） |
| `toolOutputReady` | `:405` | `appendToolExecution`；无当前气泡则独立气泡兜底；`memory` 有 M1 静默闸（`!m_memoryBubble` 即丢弃）；**任何一次到达都 `singleShot(0)` 补刷一次侧栏占用条**（tool 结果此刻还压在 `m_toolResultsReady`，同栈批尾才回填历史） |
| `subagentProgress` | `:446` | `appendSubagentProgress` |
| `teammateProgress` | `:460` | 队友折叠卡回合推进（s13） |
| `teammateToolProgress` | `:482` | 队友卡内逐工具行（s13） |
| `teammateSettled` | `:503` | 队友卡终局收口（s13） |
| `teamRosterChanged` | `:509` | 侧栏团队名册重绘（s13） |
| `permissionRequired` | `:553` | `PermissionCard` + 侧栏状态灯（三态灯在侧栏「状态」节，不在状态条） |
| `todoUpdated` | `:590` | `TodoCard` 快照 + 侧栏任务清单 |
| `backgroundTasksChanged` | tick 边沿（`AgentLoop::updateSidebarLedgerBroadcasts`） | 侧栏「后台任务」节（s17 P1，载荷即 `BackgroundTasksManager::ledgerSnapshot`） |
| `cronJobsChanged` | 同上 | 侧栏「定时任务」节（s17 P1，载荷即 `CronSchedulerManager::ledgerSnapshot`） |
| `goalStateChanged` | 同上 | 侧栏「当前目标」节（s17 P1，载荷为 `goalStateSnapshot()`；空对象=整节隐藏） |
| `activitySessionChanged`（s17.8） | `:631` | 转发给宿主 `LiteHarness` 灌活动缓存（现读该会话 `.activity/daily.json` → 合并 → push 到 `NewChatPage::setActivityData`）；**本表原先漏列此行**，s17.9 注释审计按 HEAD 实测补上 |
| `memoryPhaseStarted` / `memoryChainFinished` | `:520` / `:536` | 记忆进度 live 卡的挂出与收尾 |
| `scheduledUserMessage` | `:637` | 以「定时/团队唤醒用户消息」入历史并直接发起新一轮（cron 交付、s13 `tryDeliverTeamEvents` 空闲唤醒与 s15 `tryDeliverBackgroundWakeup` 后台唤醒三路共用此发射点，GUI 零改动；**s15** bg 唤醒为空 displayText/空 userText——槽内卫兵跳过空气泡防空标题锁死、后端跳过 user 入账，收割由 `run()` 圈顶 `injectBackgroundResults` 承载） |
| `goalNotice`（s17） | `:370` | **不入历史**的本地回显气泡（`addMessage` + 贴底）；handler 按 `isRunning()` 两形判别——零模型早退形（emit 时 run() 尚未 setRunning）先 `deleteLater` 收掉「处理中…」占位泡再回显（禁 `finishStreaming`，会把占位定稿成永久静态泡）；终局回显形（挂起中）不碰真回复气泡（防 finished 兜底双份）。goalNotice 载 `GoalRules.h` 英文契约串，禁翻区不入 .ts |
| `runningChanged` | `:656` | 输入框忙态 `setTurnBusy` + 侧栏状态灯；终局兜底复位审批灯（`setRunning(true)` 起即禁输入，早于 `run()` 卫兵） |

---

### 6. 运行时数据目录布局

根是**存储根 `<config>` = `<exe 目录>/../config`**（单源 `src/LitePaths.h`，纯字符串派生 + `QDir::cleanPath` 折叠 `..`），
不是用户主目录，也**不再随会话工作目录散落**。发布包布局本就是 `<pkg>/bin` + `<pkg>/config`、开发态是 `build/bin` + `build/config`，
两处都已在盘上存在，故本次收敛零新增形态。

> **改造前**同一个逻辑存储区被三套锚切开：settings.ini 钉 `applicationDirPath()`、会话索引 index.json 钉**进程 CWD**
> （`SessionStore::rootDirFor(QDir::currentPath())`）、会话数据与 skills 钉 `m_workDir`、FluentUI 配置另走
> CWD 相对的 `../config`。启动目录一变就出现「索引看得见、数据找不到」。现全部走 `LitePaths` 一个锚，
> 且索引本身也并进了 `config.json`（键 `app/sessions`，存储根本级只剩一份 json）。
> **旧数据不迁移**（用户裁决）：升级后旧 `<workDir>/.lite-harness/` 与旧 exe 同目录 `settings.ini` 成为孤儿，
> 表现为「历史列表为空」，属预期而非故障。

```
<exe 目录>/../config/                     ← 唯一存储根（LitePaths::configRoot()）
├── config.json                           ← 用户配置（宿主键挂 `app/` 组，**会话索引即其中的 `app/sessions` 键**）与 FluentUI 配置（`config/*`、`translation/*`）**合流同一个文件**
│                                           （路径单源 `LitePaths::configFilePath()`；格式 Json、键名**大小写敏感**，判据见 `tests/tst_configmerge.cpp`）
└── .lite-harness/                        ← 会话存储区（LitePaths::storeRoot()）
    ├── skills/<name>/SKILL.md            ← 技能，**全局共享一套**（跨会话、跨 workDir；LitePaths::skillsDir()）
    └── sessions/<dataId>/                ← 会话数据根 = LitePaths::sessionDataRoot(dataId) = AgentLoop::sessionDataRoot()
        ├── history.json                  ← 会话历史（AgentLoopHistory.cpp:36）
        ├── .memory/MEMORY.md + <slug>.md ← 记忆索引与记录（MemoryManager）
        ├── .task/task_<hex8>.json        ← 任务图，一任务一文件（TaskStore）
        ├── .transcripts/transcript_*.jsonl ← 压缩前完整转写（CompactManager）
        ├── .transcripts/snip_archive.jsonl ← snip 归档，**固定名只追加**（非全量重写；恒定标记文本，缓存友好）
        ├── .task_outputs/tool-results/   ← 超大工具输出卸载（AgentConst 名单源）
        ├── .temp/                        ← prompt 引导语指向的临时目录（`AgentLoopPrompt.cpp` 头部 temp 段注释，约 :24-26）
        ├── scheduled_tasks.json          ← cron 台账（CronSchedulerManager.cpp:554）
        ├── .mailboxes/<name>.jsonl       ← 团队邮箱，一代理一文件（MessageBus；破坏性读；s13）
        ├── .worktrees/<name>             ← git worktree 隔离检出（WorktreeManager；分支 wt/<name> 永不随 remove 删除；s13）
        ├── .workflows/                   ← s16 编排台账：<runId>.json 原子快照 / <runId>.journal.jsonl HIT 账本 / <runId>.output.json 信封 / last_run.txt（WorkflowManager；lcc 跨进程 .lock 不采=偏差）
        └── .activity/daily.json          ← s17.8 会话每日 token 消耗账本（`{"YYYY-MM-DD": {prompt, completion, cached, reasoning, calls}}`；`AgentLoop::appendActivityLedger` 在 usage 回读点原子落盘，NewChatPage 热力图数据源；无效快照不写入）
```

- **回退语义**：`sessionDataId` 为空时 `sessionDataRoot()` 返回 `<config>/.lite-harness`（
  单源 `LitePaths::sessionDataRootFor(root, dataId)` 的 `dataId.isEmpty()` 分支），保证未注入 ID 的独立构造路径行为不变；空历史不落盘以免污染会话存储区
  （`AgentLoopHistory.cpp:23-27`：23 签名、25 说明注释、26-27 才是 `if (m_sessionDataId.isEmpty() || m_messages.size() <= 1) return;` 判据本体）。**这条回退形态是删除护栏的判据**：它必须**不**落在 `sessionsRoot` 之下，
  否则「未隔离」会被误判成合法会话目录、前缀检查恒真，删一个未隔离会话等于删全部会话（`tst_litepaths` 钉住）。
- 目录名一律取 `AgentConst::k*DirName`（声明在 `AgentConstants.h` 的 `k*DirName` 常量组；s13 扩 `.mailboxes`/`.worktrees`、s16 扩 `.workflows`。**此处刻意不写行号**：本行曾三次登记行号（323-331→341-349→346-354），每次给该头文件上方加注释都会整体位移，数字锚在此处零信息量、纯漂移源），拼法涉既有数据兼容，**逐字符
  不可改**；`.lite-harness` 中间层只在 `sessionDataRoot()` 拼一次，各引擎只拼自己的叶子段。
- 用户配置**不另设锚点**：`<config>/config.json`（路径单源 `LitePaths::configFilePath()` → `AppSettings::filePath()`，弃用注册表；读写门面 `value()`/`setValue()`，缓存与失效判据见 `SettingsCache.h`）。改造前它钉在 `applicationDirPath()/settings.ini`（exe **同**目录），现与索引、会话数据同根，一个存储区一个锚。
- **沙箱围栏随之改为双根**：`AgentLoop::sandboxRoots(toolCwd)` = `{toolCwd, sessionDataRoot()}`，围栏判定外提到纯 QtCore 头 `AgentPathGuard.h::isWithinSandboxRoots(path, roots)`。原因：会话数据根统一锚到 `config/` 后，prompt 明确引导模型写入的 `.temp`（以及队友 `.worktrees`）**不再是 workDir 子树**，旧的单根 `startsWith` 口径会把模型自己的临时目录判成越狱——主循环每次写临时文件弹一张审批卡，队友侧无交互批准通道则直接折成硬拒文本。
- **s17 goal 不新增任何数据目录**：`GoalManager` 账本（active goal / 事件台账 / consecutiveBlocks / totalTokens）全内存，随 `AgentLoop` 生命周期存亡——区别于 `.workflows` 的磁盘台账（lcc 落盘转录不采=偏差⑥；`restore` 静态工厂备好但 v1 宿主未接线，重启后 goal 不恢复是当前契约）。

---

### 7. 构建与打包（结构视角，细节见 AGENTS.md）

- 根 `CMakeLists.txt:49` 的 `file(GLOB src CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` 一次收走全部
  源文件——**新增 TU 不需要改任何 CMake 文件**（本次 AgentLoop 拆分新增 13 个文件即零 CMake 改动
  即证）。`add_subdirectory(src)` 已注释，不存在 src 级 CMakeLists。
- 第二个可执行目标 `lite-harness-tests`（`LITE_TESTS` 默认 ON，`CMakeLists.txt:128-171`）只链
  `Qt6::Core`，产物落 `build/tests/` 而非 `build/bin/`——CPack staging 与根级清理都围绕 `bin/`
  展开，测试 exe 既进不了包也不会被清理规则误删。
- 只有一个可执行目标 `lite-harness`，链 `FluentUI::Controls/Utils` + `Qt6::Network`；
  `3rdparty/FluentUI` 是子目录（构建必需），`3rdparty/lcc` 为移植规格参考**不进构建图**；
  `3rdparty/sqlite*` 与 `cmake/FindSQLite3.cmake` 已 `git rm`（摘除子构建后全仓零引用，纯死码）。
- 应用数据全部内嵌资源：QSS → `:/stylesheet/`、图标 → `:/res/`、翻译 qm → `:/i18n/`。
  因此出包 = exe + Qt 运行时 + VC 运行库（CPack ZIP 唯一路径）。
- CI（`.github/workflows/Windows-Qt6.9.0.yml`）**无 lint 步骤**：干净环境全量 configure +
  Release 全目标构建 + `ctest` 单测 + cpack 出 zip 即验收；tag `v*`/`s*` 末尾上传 GitHub Release。
  必须全目标构建（勿 `--target lite-harness`），原因见 AGENTS.md「构建」节。
  单测跑在构建之后、打包之前——测试失败即中止流程，不让坏产物进 zip。

---

### 8. 单源纪律一览（想改一个值，只改一处）

| 要改的东西 | 唯一入口 |
|---|---|
| 工具名 | `ToolNames.h`（schema / handler 表 / executeTool 特判 / toolSummary / 钩子比较 / 子代理白名单 / ToolBlock 标题全部引用它） |
| 工具摘要口径（实时与历史回放必须一致） | `AgentLoopDetail::toolSummary`（`AgentLoopTools.cpp`） |
| 工具成败判定（UI 显形与历史折叠同源） | `AgentLoop::isToolFailure`（`AgentLoopTools.cpp`；s15：`Permission denied` 等值比较改 `startsWith` 前缀判定——异步定值拒长串必须判失败，`[Background task start error]` 判据删——新失败串 `Error:` 前缀天然覆盖；`TeammateRuntime.cpp` 镜像判定两处双写、注释互指，改文案族须两处同步） |
| bash 危险黑名单 | `AgentLoopDetail::bashDenyList`（`AgentLoopPermission.cpp`，三方共用） |
| tool_result 消息装配（协议硬要求，断配对即上游 400） | `AgentLoopDetail::makeToolResult`（`AgentLoopTools.cpp:120`；stop 的两处合成、cancelSubAgent、正常回填、历史恢复占位全部经它） |
| 会话历史 → wire messages 快照 | `AgentLoopDetail::snapshotMessages`（`AgentLoopTools.cpp:129`；口径全量 / mid(1) 由调用方显式表达） |
| 上下文溢出错误判定 | `AgentLoopDetail::isContextOverflowError`（声明 `AgentLoopInternal.h:65`、定义 `AgentLoopRequest.cpp:23`；主循环反应式压缩、队友流错误侧与 SubAgent 溢出重试（s18）三方共用） |
| 数据目录名 / 魔法数 / 模型清单 | `AgentConstants.h` |
| 布局尺寸（多页同列对齐） | `LayoutConstants.h` |
| 配置键与文件位置 | `AppSettings.h`（键清单 + 路径 + 读写门面）；**读缓存与失效判据单源 `SettingsCache.h`**（身份三元组 / 大小写归一 / 写侧 sync-后-invalidate 全在 Reader 一处，`AppSettings` 只是它的转发——禁再在消费点现构造 `QSettings` 绕开缓存） |
| 工具配色类别 → QSS | `ToolTagKind.h` + `stylesheet/<theme>/*.qss` |
| 主题化样板 | `ThemeAware::bind`（禁止再复制「读 QSS + 订阅 themeChanged」三件套） |
| 版本号 | CMake `project VERSION`（数字段 18.2）→ `LITE_VERSION` 宏（拼 `s` 前缀 = 版本名 s18.2；s18 起主段为自有发布阶段、s18.1 起带第二段补丁位，lcc 参照阶段到 s17 为止，形态口径见 AGENTS「发布」节）|
| 文本文件行尾口径（匹配域 LF / 写回域按主导行尾还原） | `LineEnding.h`（`read_file`/`write_file`/`edit_file` 共用，禁再各自裸字节匹配） |
| token 估算与预算 | `AgentConst::estimateTokens`（字符→token：CJK 1:1、其余 4:1，实测 qwen3.8-flash 中文 ≈1.00 字符/token 佐证）+ `AgentConst::contextTokenBudget`（**s17.9 丁-3 裁决：= 声明值本身，不再 ÷4**；经恒等纯函数 `contextTokenBudgetFrom()` 暴露，头文件 `static_assert` + `tst_agentconst` 四条 identity 断言双重钉死，改回任何系数即编译失败 + 测试转红。退役常量 `kCharsPerTokenBudget`）；**会话体逐条计量单源 `CompactTokens.h`**（`messageTokens` 单条项 / `conversationTokens` 求和，`CompactManager::estimateTokens` 只是它的转发，宿主与压缩管线共用同一实现，禁再另写一套全量序列化） |
| 上下文占用（侧栏占用条 + 条下数字行，与压缩门槛同源） | `AgentLoop::estimatedContextTokens()`（usage 锚 + 增量外推，无锚回落全量估算）；**读数画法单源 `UsageTokens::formatCompact`**（行内 K/M 紧凑、tooltip 千分位精确，与消耗段同一条函数） |
| 上下文消耗（状态条消耗行；与上面的占用语义正交，勿混用） | `AgentLoop::sessionUsageTotals()` → `UsageTokens.h`（`fromUsage` 解析 + `Totals` 只增累加 + `hitRatio`/`uncachedPrompt` 派生 + `formatCompact` 行内紧凑读数）。**部分口径**：只含主循环与队友回合，不含压缩/记忆/workflow（`AsyncRequest` 无 usage 通道）——UI tooltip 同步声明该边界 |
| 输出速度（状态条速度行；与消耗行同层级、语义正交，勿混用） | `AgentLoop::outputRate()` → `StreamRate.h`（`Window`/`makeWindow` 判据 + `Meter` 只增账）。**分母 = 生成窗口（首个增量 → usage 末帧），不含首字延迟与工具执行时间**；TTFT 单独记录、单独显示；聚合是 Σtokens ÷ ΣgenerationMs 而非各轮速率的算术平均。**口径比消耗账更窄**：仅主循环请求（队友增量不经宿主信号面、压缩/记忆/workflow 无 usage 通道）——UI tooltip 同步声明该边界 |
| 工具名清单（27 个 = 18 基础 + 7 团队 + 1 MCP 入口 connect_mcp + 1 编排入口 workflow（s16）；`submit_plan` 不入表） | `ToolNames.h`（schema / handler 表 / 权限门 / 子代理白名单 / ToolBlock 标题 / 回归清单核对项全部引用它） |
| 导航/堆叠键 | `NavItem.h::NavKey` |
| 团队数据目录名 / 编排台账目录名 | `AgentConst::kMailboxesDirName`（`.mailboxes`）/ `kWorktreesDirName`（`.worktrees`）/ `kWorkflowDirName`（`.workflows`，s16）——三者均声明在 `AgentConstants.h` 的 `k*DirName` 常量组，各引擎禁另拼 |
| 预留队友名 | `AgentConst::kReservedTeammateNames`={lead, agent} + `isReservedTeammateName`（大小写折叠，均在 `AgentConstants.h`，锚符号不锚行号；spawn 占用即拒） |
| 团队心跳界 | `kTeamIdleScanIntervalMs`(2000, 声明在 `AgentConstants.h`)——原同步适配器等待界 `kTeamBashStartWaitMs`/`kTeamBashKillWaitMs` 已随 Lane B 异步化摘除（Gate④ MINOR-A），勿复活 |
| Lead 收件名 / Lead owner 键 | 邮箱名 `lead`：引擎侧 `AgentTeamsManager.cpp` 匿名 ns `kLeadName` 单源，宿主 wake 点重复字面量并以注释交叉锚定；TaskStore owner 键 `agent`（`kLeadOwnerKey`）是另一语义——`agent` 双重身份（邮箱预留名 + Lead owner 键） |
| `submit_plan` 工具名 | 引擎匿名 ns 字面量 + 宿主队友 schema 字面量（Lead 面 27 件永不含它） |
| claim 成功承重前缀 | `'Claimed '`（lcc 跨模块字符串契约；定义在 `TaskStore` 两 claim 路径文案、消费在 `AgentTeamsManager`——改文案必同审两处） |
| 恢复契约串 / 异步定值拒串（s15） | `AgentRecovery.h::RecoveryRules`（`continuationPrompt()` / `asyncApprovalDenied()` 逐字；改动须同步 `tst_agentrecovery` 逐字断言、`isToolFailure` 前缀判据与 `TeammateRuntime` 镜像——两处双写注释互指） |
| workflow 工具信封（s16，名称/描述/schema） | `WorkflowManager::toolInfo()`（`src/WorkflowManager.h` 成员；`AgentLoopPrompt.cpp` 静态表内以原型对象取值，禁双写名称/描述/schema） |
| workflow 策略门 reason 词条（s16） | `AgentLoopHooks.cpp` 英文 reason 与 `PermissionCard.cpp::translateReason` 表同步登记（「Workflow policy requires confirmation」，s14 遗留的 MCP 词条同批补入） |
| goal 契约串与判据（s17） | `GoalRules.h` 单源（七决策/`parseEvaluatorJson` 六门逐字串/设定与回显契约串族/`evaluatorSystemPrompt`/`blockContinueText`/`transcriptText` 24000 预算）——改文案须同步 `tst_goalmanager` 逐字断言；`goalNotice` 载英文契约串保持原文，**禁翻区不入 .ts** |
| goal 评估模型与 block 上限（s17） | config.json 键 `goalEvaluatorModelId`（三级回退首级，空=不启用独立评估模型，`AgentConst::goalEvaluatorModelIdValue()`）与 `stopHookBlockCap`（`stopHookBlockCapValue()` 缺失/非法/<1 回退 8；`GoalManager` 构造期快照，改后新会话生效）；两键设置页无编辑卡，手改配置文件 |

---

### 9. 已知分层异常与技术债（登记在案，勿默默踩）

1. **`SessionRegistry.cpp` 反向 include `ChatSessionPage.h`**：头文件里只前向声明，`.cpp` 需完整
   类型才 include，形成 L1→L2 的回边。可接受（无循环 include），但新增跨层引用前先看这里。
2. **`ChatSessionPage.cpp:19` 直接 include 内核非公开头 `AgentLoopInternal.h`**：靠
    `friend class ChatSessionPage`（`AgentLoop.h:261`）授权，目的是历史回放与实时链路共用同一
   `toolSummary` 口径。**这是有意为之的例外**，别据此开「UI 可以吃内核内部头」的先例。
3. **`SettingsPage` 直改全局运行时配置**（`QOpenAi::setUrl/setToken`）：设置页 → 引擎的直连边，
   生效语义见 `SettingsPage.h` 注释。
4. **仍偏大的文件**（下一批可读性优化的候选，按体量排序，实测行数；**行数口径**=文件换行符数，
   末行无换行符则 +1——按 `-split` 计数会因空尾元素每文件虚高 1，勿据此判漂移；本轮文档校验已按 HEAD 全量复核，本批含注释改动的文件取**工作树值**（提交后即 HEAD））：`ChatSessionPage.cpp` 1291、
   `TaskStore.cpp` 1225、`MemoryManager.cpp` 857（s17.9 记忆判据抽头批前为 1117，八个纯判据搬入 `MemoryRules.h`）、`WorkflowRuntime.cpp` 1078（s16 Lane B 编排引擎，新晋）、
   `WorktreeManager.cpp` 917、`WorkflowManager.cpp` 879（s16 Lane A 引擎，新晋）、`MessageBubbleWidget.cpp` 887、
    `QOpenAi.cpp` 855（s15 传输层扩展后）、`AgentTeamsManager.cpp` 853、`SettingsPage.cpp` 1202（s17.2 权限审批开关 +36；s17.12 自更新卡 +344，含重构收口、「更新」组标题 + 卡上移、组间距补回与下载进度环；**卡片类拆分已明确不做，理由见 §9.11 ③**）、
     `SessionSidebar.cpp` 807（s16.1 腾出两节后，状态灯整盏搬回侧栏「状态」节；s17 P1 再加「后台任务」/「当前目标」两节）、
     `CompactManager.cpp` 887（计量抽纯头后转发 + 两段循环改增量；s17.9 summary 解耦与本轮注释审计再净 +9；**s18.1/s18.2 两批回写 767→887**，逐修订 `git diff --numstat <rev>~..<rev> -- src/CompactManager.cpp` 实测归因：`4ecb942` **+3**（仅注释）、`6e5982b` +4/−4 **净 0**、`b16a674` +117/−4=**+113**（本地四段各发一张轻量卡 + `CardSink` 首参带 toolName + `emitLocalCard`）、`432e70d` +9/−5=**+4**（prepare 入口日志降级为 `LOG_DEBUG` 及其理由注释），四处合计 **+120** 与差值逐字吻合；登记点 `61b0179` 原始字节实测正是 **767**、HEAD `432e70d` 实测 **887**。**同批再次复现文本管道少计坑**：`git show | Out-File` 口径数得 642/727，与真值差 125/160，复算必须 `git cat-file blob` 落原始字节再 `ReadAllLines`）、`TeammateRuntime.cpp` 701（s13 队友预算 +147 行后新晋；s15 镜像判据同步后回落；s18 压缩内核搬入 `ContextTrim.h` 改一行转发再 −32；原登记 728 未含 token 审计批 +5，HEAD 实测 733，本批按工作树值回写）、
     `AgentLoopTeam.cpp` 701（s13 接线 + s15 异步 fail-closed + s17.2 审批开关放行 +11 + s17.9 队友落活动账 +9）、
     `ChatStatusStrip.cpp` 290（s16.1 新晋，承接消耗/速度两段文字；占用数字与状态灯随后按用户裁决搬回侧栏「上下文」/「状态」节；s17.1 加轮步段成三段）（`src/` 合计 32775 行，本轮**工作树**实测（HEAD `a1ac43e` 为 32719，差 +56 = 本批「热力图算术抽头」净增：新纯头 `HeatmapGrid.h` +53、`ContributionHeatmap.h` +3、`ContributionHeatmap.cpp` 净 0——三处改转调是删旧算术加转发，故不增行），口径 = `ReadAllLines` 含空行；上一轮登记的 31777 已过期——**差值 833 是跨多批次的累计，不得归因于最近两批**：`git diff --numstat a6e6441..HEAD -- src/` 实测仅 +36（逐修订单批实测：`50d3e56` 热力图批 **+8**、`9664287` 预算恒等批 **+28**、`ce579f9` bump 批 0），本轮注释/文档审计批再 +10，其余 787 行属 31777 登记点之后、`a6e6441` 之前的各批；**归因一律用 `git diff --numstat <rev>~..<rev> -- src/` 逐修订实测，勿拿两个登记值相减当单批增量**（本轮就犯过一次：把 833 全记到最近两批头上）；上一轮登记的 31258 已过期——s17.6..HEAD 净增 205 行（`CompactTranscript.h` 186 + `AgentLoopHistory.cpp` 12 + `AgentLoop.h` 5 + `ChatSessionPage.cpp` +3/-1），余量差系该值登记时的 HEAD 早于 s17.6 末批；s17 新增四件 `GoalRules.h` 410 / `GoalManager.h` 156 / `GoalManager.cpp` 348 / `AgentLoopGoal.cpp` 230；s17.1 摘除「查看完整输出」入口后曾由 31017 回落 320 行——`ToolOutputDialog.{h,cpp}` 257 + `ToolBlock` 63；此后 s17.2–s17.7 各批净增，现值以本轮实测为准）。**s18.3 后性能批（本批）实测：`src/` 合计 36997 行**（工作树、`ReadAllLines` 含空行口径；同口径 `git archive HEAD src` 解包实算 HEAD = 36814，本批净增 **+183** = `git diff --numstat HEAD -- src/` 逐文件实测 ADDED 222 / DELETED 39，15 文件全在 `src/`、零新增源文件）。**上句登记的 32775 已过期，且过期部分不属本批**：32775 登记时 HEAD 为 `a1ac43e`（该点 32719 + 当时工作树 56），`git diff --numstat a1ac43e..HEAD -- src/` 实测其后各批（s18.2 发布批、s18.3 多 Provider 批）净增 **+4095** → HEAD 现值 36814 = 32719 + 4095，与 `git archive` 解包实算**独立互证同值**；故距 32775 的共差 4222 行 = 他人批次 4039 + 本批 183，**勿把 4039 记到本批头上**（口径遵本节「归因一律用 `git diff --numstat <rev>~..<rev> -- src/` 逐修订实测，勿拿两个登记值相减当单批增量」禁令）。本批同步两处逐文件行数账：`AgentLoopFileTools.cpp` 377→393、`AgentLoopHistory.cpp` 231→236；其余 13 个改动文件经实测确认不在逐文件行数账表内。断言数本批零新增，带/无夹具两形态仍 `pass=2695`/`pass=1684`（与「覆盖现状」节 s18.3 登记同值），该节无需改写。
     **清单边界说明**：本表只收「拆了能提升可读性」的文件，非严格体量前 N。`AgentLoopPrompt.cpp` 234（登记时原值 717，s17.9 schema 抽头后搬出 490 行）、
     `LiteHarness.cpp` 800、`ToolBlock.cpp` 582（s17.1 摘除「查看完整输出」入口与其对话框后回落；**s18.2 批回写 565→582**，`b16a674` +18/−1=**+17**：四个本地段伪名标题分支「已卸载/已归档/已微压缩/已裁剪」+ 全量压缩 live 标题「压缩中」；基线 `51b1663` 原始字节实测正是 **565**、此后仅该一笔触及本文件，归因闭合）未列入是**有意的**——前者的体量几乎全是静态 system
     prompt 与 27 工具 schema 的**禁翻区字面量**（拆开反而破坏「全会话字节恒定」的可核对性），后两者
     是单函数长 UI 装配，线性可读。勿因「716 高于清单内多个候选」就把它补进候选或动手拆 prompt（**s17.9 更新**：该前提已随 schema 抽头消失——本文件现 234 行，不再是偏大候选；但「勿拆静态 system prompt」的裁决仍然有效，理由不变：其体量是禁翻区字面量而非逻辑）。
5. **测试覆盖面**：**三十五套件**已落地（s17.9 三批抽头新增 `tst_memoryrules` / `tst_toolschema` / `tst_sessionkeys`，上一登记值「三十二套件」为热力图批现值、历史保留；另本句括号内实列 8 个名字而原文称「七件」，系热力图批把 `tst_heatmapgrid` 追加进括号却未同步件数，现按实列更正）（`tst_heatmapgrid` / `tst_compacttranscript` / `tst_uiux` / `tst_tooltagkind` /
   `tst_toolnames` / `tst_navkeys` / `tst_layoutconst` / `tst_agentconst` 八件此前只进了 `tests/main.cpp`
   与 CI 注释，本节枚举漏登记，现补——计数以 `tests/main.cpp` 的 `failed += tst_*()` 调用行为准）——
   `tst_lineending` / `tst_messagebus` / `tst_taskstore_lease` /
    `tst_worktree` / `tst_agentteams` / `tst_mcpmanager` / `tst_agentrecovery`（s15 契约串逐字 + 判据矩阵，零夹具）/
    `tst_backgroundtasks`（s15 id 序列 / 严格布尔 / hasPending 只查不清 / collect 模板逐字，零夹具）/
    `tst_workflowmanager`（s16 身份/哈希/journal/schema/抢占/快照纯函数门，155 = 恒跑组 118 + 夹具门控文件组 37）/
    `tst_workflowruntime`（s16 limits 恒跑组 + 端到端 10 分组含 review-changes 全环 / resume 全 cached / 脏缓存拒用 / budget / 屏障 / 嵌套门 / 抢占 / 取消，约 87）/
    `tst_compacttokens`（计量单源纯头 `CompactTokens.h`：可加性 / 项与位置无关 / **替换恒等式** / 替换序列逐步对表 / 确定性 / 折算口径锚点，19 静态点·实跑 48 条，零夹具）/
    `tst_settingscache`（设置读缓存纯头 `SettingsCache.h`：身份三元组穷举 / 缺失文件态 / **200 次热读只装载一次** / 四类外部手改立即生效（等长·变长·新增·删除）/ 缓存与现取逐键一致（含类型与大小写变体）/ 裸逗号串 QStringList 形态不被抹平 / 写侧落盘与失效成对，实跑 89 条 = 恒跑组 35 + 夹具门控组 54）/
    `tst_usagetokens`（用量计量纯头 `UsageTokens.h`：有效性门 / 只增累加 / `hitRatio` 钳界与 `uncachedPrompt` 钳非负 / 嵌套 `*_tokens_details` 缺失即 0（不算解析失败）/ 逐键对表 / **`formatCompact` 紧凑读数**（K/M 档位、999950 进位升 1M、整十位省 `.0`、行内不超 8 字符），82 条，零夹具）/
    `tst_streamrate`（输出速度纯头 `StreamRate.h`：空账不除零 / 无效窗口不进分母也不改写 last / 定窗判据（生成窗口不含 TTFT）/ **Σtokens ÷ ΣgenerationMs 而非各轮速率的算术平均**（同一批窗口两口径差 5 倍，必须钉死）/ 纯 tool_calls 回合无增量起点即窗口作废 / 聚合与到达顺序无关而 last 顺序敏感 / last 归属靠 `beginTurn` 降级（本轮无增量起点时不冒充本轮，UI 措辞随之改「上一可定窗」）/ 长会话累加不溢出，30 条，零夹具）/
     `tst_goalmanager`（s17 goal 引擎：`parseEvaluatorJson` 六门逐字定值串 / 七决策记账矩阵 / 代际守卫（setGoal·replace·clear 成功即 ++，迟到代按默认 Allow 静默丢弃零记账）/ `transcriptText` 24000 预算与独占超限插标记 / CLEAR_ALIASES 六词 / stopGate 三门载荷，实跑 209 条 = 静态点 195 + 别名循环展开，零夹具）/ `tst_cronmanager`（s17 P1：validateCron 五报错族逐字 / cronFieldMatches 逗号列表全不中必假=647b22e 回归钉 / cronMatches Vixie 双限定 OR、单限定只看限定侧 / ledgerSnapshot 走 durable=false 不触盘，26 条，零夹具）/ `tst_uiscale`（字号倍率校验界 80~200 与「缺失/空串/非数字/0/负数/越界」一律回退 100 / `scaledSize` 四舍五入不进位为 0 / DPI 策略名往返 / 几何 base64 编解码幂等 / `scaleQssFontSizes` 只缩放声明里的第一个 px 字面量，68 条，零夹具）/ `tst_fluconfigstore`（三段解析各自回落 / 格式名折叠 / 落点拼接不越界 / JSON 后端读写往返，75 条 = 恒跑组 49 + 夹具门控组 26）/ `tst_fluconfigutils`（注入 → 后端重建 → 读写往返端到端；同值重复注入必不重建、换定位等于换文件且**旧值不迁移**，47 条 = 恒跑组 4 + 夹具门控组 43）/ `tst_litepaths`（存储根反推单规则 / `.lite-harness` 中间层只出现一次 / **删除护栏前提**：数据根严格在 sessionsRoot 之下且回退形态不落其下，29 条，零夹具恒跑）/ `tst_pathguard`（多根沙箱判定：分隔符边界拒兄弟目录 / 兄弟会话不可互达 / fail-closed 空根拒一切，21 条，零夹具恒跑）/ `tst_configmerge`（宿主与 FluentUI 合写同一个 config.json 不互覆 / 大小写口径与格式**成对**交付 / 清单键列表形态在两种后端下都归一，35 条 = 恒跑组 14 + 夹具门控组 21）/ `tst_sessionstore`（会话索引并入 `app/sessions`：读写换轨 / 盘上真相必须是数组 / 整表回写不抹别人的键 / 旧 `index.json` 一次性迁移四分支，101 条 = 恒跑组 4 + 夹具门控组 97）/ `tst_compacttranscript`（压缩转写回溯纯头 `CompactTranscript.h`：摘要消息判据两形 label + 普通 user 消息不误判 / `Full transcript:` 解析与占位缺失降级 / 转录路径守卫（子目录·`..` 越界·他会话根·`snip_archive.jsonl`·错扩展名全拒）/ JSONL 坏行与非对象行跳过 / 链式展开（自引用只展开一次·深度上限·读失败一律保留摘要），40 静态点、零夹具恒跑）/ `tst_uiux`（P1 UI 判据：命中区下限钉 32 下界 / `app/` 组前缀只拼一次 / min<default<max 区间自洽 / 只读文件写失败返回 false 才可观测告警，静态点 16；组 1 恒跑 + 组 2 夹具门控）/ `tst_tooltagkind`（工具名→标签类别映射 16 分支逐条 + 未识别回落——拼错类别只静默变灰、无失败信号，26）/ `tst_toolnames`（工具名常量表契约：非空 / 无重复值 / 命名格式 / 与标签映射的覆盖关系——重复值会让某一处静默失配，静态点 7，断言全在循环外（循环只累加标志位），实跑同为 7；**s18.2 本地四段轻量压缩卡批追加 ⑦ 组 9 条后静态点 16、实跑同为 16**——钉新纯头 `CompactStageNames.h` 四个 UI 伪名的字面量逐条 / 良构·唯一·与真工具名及 `memory` 零交集 / 映射恒 `other` 中性灰，此处 7 是登记时原值按不改历史只登记保留）/ `tst_navkeys`（导航/堆叠页键非空唯一 + `SessionPrefix` 与 `SessionKeyFmt` 前缀同源——改一个不改另一个表现为「打开旧会话变成新会话」，14）/ `tst_layoutconst`（布局常量自洽：全为正 / 命中区下限 / 窗口最小宽与列宽关系 / 同义常量相等，23）/ `tst_agentconst`（数值域与键名契约：min<default<max、键名非空唯一、关键常量为正——`static_assert` 只保序不保唯一与正负，静态点 12，断言全在循环外，实跑同为 12；**s17.9 预算恒等批追加 4 条 `contextTokenBudgetFrom` identity 断言后静态点 16、实跑同为 16**，此处 12 是登记时原值按不改历史只登记保留）/ `tst_activityledger`（s17.8 会话活动账本纯头 `ActivityLedger.h`：五档阈值边界 / 日期键严格 ISO 往返（Qt `QDate::fromString(Qt::ISODate)` 对斜杠写法宽松接受、判据必须自己拆到字符层）/ `accumulateSnapshot` 无效快照丢弃 / `totalProcessed` / `aggregate` 跨会话合并 / `windowFilter` 闭区间 / QSaveFile 读写往返 / 脏 JSON 与非日期键降级到空表，实跑 58 = 恒跑组 42 + 夹具门控组 16）/ `tst_heatmapgrid`（s17.9 热力图网格算术纯头 `HeatmapGrid.h`：371 格满矩形 / 首日 = today−370 / 末格 (52,6) 恒为 today / 每列满 7 格且无未来日期 / `cellOf`↔`dateOfCell` 逐格互逆 / 窗口外返 {-1,-1} 不钳位不回绕 / 跨年与闰日边界 / **月份标签按月号去重、首列补标让位给同号 1 号锚点**（`monthLabels`：371 天恒覆盖 ≥13 个月份实例，首尾同号跨年不去重底部必现两个相同月份；保留补标又与相邻月标签挤压，让位后相邻恒 ≥4 列，s18 两阶段修），39 条，零夹具恒跑；此处 27 为 s17.9 抽头批登记原值按不改历史只登记保留）/ `tst_memoryrules`（s17.9 记忆判据抽头纯头 `MemoryRules.h`：frontmatter 五条回落路径且失败时 body 保留全文 / slug 空串与全非词字符回落 `memory`、CJK 走 unicode `\w` / `role==tool` 不入语料 / `extractJsonArray` 手写括号状态机（串内 `]` 不提前闭合、`\"` 不出串、嵌套深度、首候选非法回退下一候选、未闭合返空）/ 必填四要素与 scope 白名单 / 非 persistent 一律拒 + 16 条临时标记 + slug·描述·正文三重去重 / `memoryDocument`↔`parseFrontmatter` 写读往返含冒号与换行，76 条，零夹具恒跑；**同批实测证伪一条注释**：非法 JSON 转义并不如注释所称回落，Qt 宽容接受并静默吞掉反斜杠）/ `tst_toolschema`（s17.9 工具 schema 抽头纯头 `ToolSchema.h`：信封 type 恒 function 且 name 非空唯一 / 名单与 `ToolNames` 恰好 27 项完全重合 / `required` 每项都在 `properties` 内（服务端 400 主因）/ 三块无参工具**不写** `required` 键而 `list_crons` 写空数组（两形态不可合并）/ 数组专属关键字只挂 array 且 array 必配 items、字符串专属只挂 string / `todos.maxItems` 取 `AgentConst::kTodoMaxItems` 而非硬编码 / status enum 恰三值 / items 刻意不写 required / 全参数无 default / `additionalProperties:false` 仅四把锁 / workflow 信封逐字段等于 `WorkflowManager::toolInfo()` / SubAgent 白名单五工具全在 schema 内 / 惰性缓存两次取值逐字段一致，55 条，零夹具恒跑）/ `tst_sessionkeys`（s17.9 会话键算术抽头纯头 `SessionKeys.h`：新建键形态与 1..500 两两不同 / 恢复键**原文拼接**（不改大小写、不吞空白、空 dataId 退化为裸前缀——键与磁盘 `sessions/<dataId>` 目录名同源）/ 两族键共用前缀而三个非会话页键不得命中 / `seqFromKey` 只认无前导零产物 / 生成 dataId 形状恰 8 位小写 hex / **两族键不相交的真实理由是长度而非字母表**（`10000000` 既是合法 hex 也是合法 seq，屏障在 seq=10^7 处失效）/ 撞车谓词钉住两个反例（短 dataId `"1"` 撞 `Session_1`、8 位纯数字撞同值 seq），45 条，零夹具恒跑）——逐条断言口径以 **AGENTS.md 测试节为单源**，本节只留覆盖面轮廓，两处数字不同步时以实跑输出为准。静态断言点（行首调用点：限定名 `TestHarness::check` 1440 + `using` 引入后的裸 `check(` 331 + 本地 `CHECK(cond)` 宏调用 183；
     `CHECK` 宏定义自身那 6 行（`#define` 行 + 展开续体）不计入调用点）**1954** 处、全量实跑 **pass=2034 fail=0 skip=1**（s17.9 上下文预算恒等批按 HEAD 本机实跑 **pass=2038 fail=0 skip=1**、PCRE 同口径静态点复算 **1958**。**本条先前版本误记为 1957/1953 并声称「分项差 1 属既有口径漂移」，还把根因归给 `git grep -E` 的 `\s`——该归因不可复现（本机 `-E` 与 `-P` 逐条同值），真正根因是本批一次单遍管道计数与逐文件求和不一致；现按仓库净口径更正**：净口径 = 限定名 `TestHarness::check`（扣除 3 行 `#define CHECK` 续体）**1440** + 裸 `check(` **335** + 宏 `CHECK(` **183** = **1958**；同口径复算登记 1954 的那次 `a6e6441` = 1440+331+183 = **1954，与登记值逐字吻合、不存在既有漂移**，差 +4 恰为本批新增的 4 条 identity 断言。复算必须大小写敏感、行首锚定、逐文件求和交叉核对，历史条目不改、只追加本条更正。s17.9 热力图算术抽头批再追加：本机实跑 **pass=2075 fail=0 skip=1**（无夹具 1099），净口径静态点 = **1471+335+183 = 1989**（本批新增整套 `tst_heatmapgrid` 27 条，其静态点 21、实跑 27——循环内断言使两数不等，登记时勿混用））；**s17.9 三批抽头 + 热力图绘制转调批再追加**：本机实跑 **pass=2251 fail=0 skip=1**（带 `LITE_TEST_TMPROOT`）/ **pass=1275 fail=0 skip=35**（无夹具，与 `kSuiteFloors` 求和 1275 逐条自洽——该自洽性此前帮我抓到三次「按下界表登记值填源码 `check(` 出现次数」的偏差：`memoryrules` 73→76、`toolschema` 46→55、`sessionkeys` 38→45，皆因循环内断言实跑多于静态点）。净口径静态点 = **1630+335+183 = 2148**（口径：大小写敏感、行首锚定、逐文件求和，且 `#define CHECK` 的续体行不计入限定名——本口径逐字复现`a6e6441` 的 1954 与 `4caaa86` 的 1989 两个历史登记值后方才采用）。基线 `764f6c0` 同口径 = **1989**，差 **+159** 恰等于三个新文件限定名逐文件求和 74+47+38 = 159，其余 32 个旧文件计数零变化——归因闭合。**s17.11 项目指令文件批再追加**：本机实跑 **pass=2300 fail=0 skip=1**（带 `LITE_TEST_TMPROOT`）/ **pass=1324 fail=0 skip=35**（无夹具，与 `kSuiteFloors` 求和 **1324** 逐条自洽——本批表项 49 恰等于新套件静态点 49，该套件零循环内断言，两数相等）。新增整套 `tst_projectinstructions` 49 条（零夹具恒跑，静态点 49 = 实跑 49）。同口径分项复算：工作树 = 限定名 **1633**（扣 3 行 `#define CHECK` 续体后 **1630**）+ 裸 `check(` **384** + 宏 **183** = **2197**；HEAD 同口径 = **1630 + 335 + 183 = 2148**，**逐字复现上一登记值、不存在口径漂移**，差 **+49** 恰等于本批新套件的全部静态点（其余 35 个旧文件分项零变化）——归因闭合。**本条初稿曾误记为「限定名 1623 + 裸 379 = 2185、余 −10 待查」**：根因是用 `Get-Content` 逐文件循环计数，与本文既定的 `git grep` 逐文件求和口径不同（前者少计 10 处限定名、3 处裸调用），改用登记口径重测后基线立刻对上、待查项消失——再次印证「复算必须大小写敏感、行首锚定、逐文件求和交叉核对，且必须用登记时同一把尺」。行数台账同批补正：`src/` 合计 33014→**33218**（口径 `ReadAllLines` 含空行），差 **+204** 逐文件实测归因——新 TU `AgentLoopInstructions.cpp` **+66**、新纯头 `ProjectInstructions.h` **+88**、`AgentLoopPrompt.cpp` **+10**（234→**244**，注入块加第 0 段）、`LitePaths.h` **+19**（`agentsFileName`/`agentsFileFor`/`globalAgentsFile` 三口）、`AgentConstants.h` **+10**（`kProjectInstructionsCharBudget`）、`AgentLoop.h` **+11**（方法声明 + 两个 mutable 缓存成员），六处合计 204 与差值逐字吻合。套件三口径计数 36/36/36（`runSuite` 调用行 / `kSuiteFloors` 表项 / `tests/tst_*.cpp` 文件）。**s17.12 自更新判据批再追加**：新增纯头 `UpdateRules.h` + TU `UpdateManager.{h,cpp}` + 套件 `tst_updaterules`（137 条 = 静态点 137，零循环内断言，两数相等，零夹具恒跑）；本机实跑 **pass=2437 fail=0 skip=1**（带 `LITE_TEST_TMPROOT`）/ **pass=1461 fail=0 skip=35**（无夹具，与 `kSuiteFloors` 求和 **1461** 逐条自洽）；三口径套件计数 **37/37/37**；同口径静态点 = 限定名 **1633**（扣 3 行 `#define CHECK` 续体后 **1630**）+ 裸 `check(` **539** + 宏 **183** = **2352**；HEAD 同口径 = **1630 + 384 + 183 = 2197**，**逐字复现 s17.11 批的登记基线、不存在口径漂移**，差 **+155** 恰等于本批新套件的全部静态点（137 条自更新判据 + 18 条进度环分母判据）（其余 36 个旧文件分项零变化）——归因闭合。**本条初稿曾误记 HEAD 为「1630+337+166=2133、余 64 待查」**：根因是用 `git show HEAD:<file>` 逐文件取行再计数，该口径比本文既定的 `git grep -c` 少计 47 处限定名与 1 处宏（编码/分行差异，与 s17.11 批那次 `Get-Content` 误记是同一类坑的两个不同变体），改用登记口径重测后基线立刻对上、待查项消失。`src/` 合计 **34914** 行 / **125** 文件，基线 `45f4f81`（s17.11 批 HEAD）同口径 **33218** / 122 文件（= 上一批登记值，逐字吻合），差 **+1697** 逐文件实测归因——新纯头 `UpdateRules.h` **+497**、新 TU `UpdateManager.cpp` **+608**、`UpdateManager.h` **+136**、`SettingsPage.cpp` **+344**、`SettingsPage.h` **+74**、`LitePaths.h` **+24**、`App.cpp` **+7**、`LayoutConstants.h` **+7**，八处合计 1697 与差值逐字吻合，文件数差 +3 恰为三件新增（`UpdateRules.h`/`UpdateManager.cpp`/`UpdateManager.h`）。**s17.12.1 配色批再动一行**：`src/SendMsgButton.cpp` 54→53（`setIcon` 的两行调用折成一行），累计 **−1** → 合计 **34914** 行 / 125 文件、对基线 `45f4f81` 差 **+1696**；上句的 1697 与八处归因按「不改历史只登记」原样留档。**s17.12.2 更新卡按钮宽度批再动**：`src/LayoutConstants.h` **+22**（新增 `kCardUpdateActionWidth = 160` 及其取值依据注释）、`src/SettingsPage.cpp` **+10/−2**（新增 `cardUpdateActionButtonSize()` 与两处调用点切换），累计 **+30** → 合计 **34944** 行 / 125 文件、对基线 `45f4f81` 差 **+1726**；上句的 34914/+1696 按同一纪律原样留档。同批另改 `i18n/lite-harness_en_US.ts` **1/−1**（`Download &amp; Update` → `Download &amp;&amp; Update`，行数零变化故不入上面合计）：单 `&` 被 QPushButton 当助记符吃掉、实渲成 "Download  Update"，于是 naturalW 只有 138——**138 是缺陷态宽度不是文案宽度**，本批前两版取值 126/150 都是被这个假数带偏（`lrelease` 重出 `.qm` 后 41503→41505 字节，端到端装载 `.qm` 复测真基线 **151**）。阈值一律按 `scaledSize` 真实算式 `(base*percent+50)/100` 反推：160 在 100%/200% 达标，80% 档框 128px 仍裁 23px，三档全不裁需 **≥189**（旧注释按缺陷态 138 推的「≥172」同批作废）。基线一律点名 `45f4f81` 而不再写「HEAD」：s17.12 已入库，此处的比较对象是 s17.11 而非当前 HEAD，沿用旧措辞会让复算者拿错基线（本条初稿即如此）。增量归因含进度环本批净增 **+124**（分母判据 +39、卡片侧 +66、卡头成员与声明 +12、尺寸常量 +7）；早先两处（「更新」组标题 + 卡上移 +63、「权限」组后补回的组间距 +3）已并入上面的 `SettingsPage.cpp` **+344** / `SettingsPage.h` **+74**。**进度环未新增任何 qss**：初稿曾为本环补三份 `stylesheet/<theme>/UpdateProgressRing.qss`，理由是「`FluProgressRing.qss` 没进 `ControlsQss.qrc`」——该理由**已被推翻**，真实注册路径是 `controls/CMakeLists.txt` 的 `qt_add_resources`（L41/L168/L299 三主题齐全，生成物 `qrc_ControlsQss.cpp` 命中 7 次，启动日志 0 条 `open qss file failed`），只 grep `.qrc` 会误判成未注册，三份已删。复算一律 `git show` 落原始字节 + `ReadAllLines`，不走 PowerShell 文本管道。**s18 token 计量审计批行数台账补记（含三批漏记归因）**：上一登记值 34944（s17.12.2 批）之后三批改了 `src/` 却未同批回写台账，本批逐修订 `git diff --numstat <rev>~..<rev> -- src/` 实测补记：s17.13.1 四提交 `4dc993b` **+47** / `cda6a5a` **+4** / `b1413a9` **+15** / `626722f` **+70** 合计 **+136**；s18 版本判据批 `4970449` **+2**；s18 热力图月份标签批 `5dc738f` **+45**；两次版本 bump（`f2bbd9e`/`2d48e52`）各 +2/−2 净 0。累计 **+183** → HEAD（`5dc738f`）合计 **35127** 行 / 125 文件（`git show` 落原始字节 + `ReadAllLines` 实测，与 34944+183 逐字吻合）。**本批（token 计量审计修复）再动 +39**（7 文件 47 插 / 8 删）：`TeammateRuntime.h` **+4** / `TeammateRuntime.cpp` **+5**（`refreshContextBudget` 每回合满额预算刷新）、`AgentLoopTeam.cpp` **+6/−1**（回合驱动点注入 + launcher 注释更正）、`AgentLoop.h` **+5/−1** / `AgentLoop.cpp` **+18/−6**（`toolsSchemaTokens()` 单源：静态缓存 + 动态 mcp__ 现估，兜底与 overhead 两处内联块收拢）、`AgentConstants.h` **+6**（仅注释：中文标点 U+3000–303F 已在 CJK 首段覆盖勿扩区 + 变音拉丁/emoji 低估方向与反应式压缩兜底登记）、`CompactManager.cpp` **+3**（仅注释：toolResultBudget content-only 与 micro/fit 整条 JSON 的口径微差登记）。合计 **35166** 行 / 125 文件、对基线 `45f4f81` 差 **+1948**；上句 35127/+183 与历史 34944/+1726 按「不改历史只登记」原样留档。家族地图表两行行数同批回写（`AgentLoop.cpp` 634→646、`AgentLoopTeam.cpp` 701→706，两文件 HEAD 值与旧登记逐字吻合、无既往漂移）。本批零新增断言（改动 TU 全在宿主接线层进不了 ctest），带夹具实跑 **pass=2510 fail=0 skip=1** / 无夹具 **pass=1534 fail=0 skip=35** 与上一批逐字一致，`kSuiteFloors` 与 CI 下界不动。**坑再登记（本批两次踩中）**：① `git show HEAD:<file> | Measure-Object -Line` 文本管道对含中文源码系统性少计（本轮 HEAD 实测 25815 vs 真值 35127），复算必须落原始字节再 `ReadAllLines`——与上条 s17.12 批登记的同族坑；② Git Bash 内联 `powershell -Command` 嵌套引号会被 bash 先行插值咬坏（`$name`/转义引号全丢），多文件循环一律 Write 落 `.ps1` 再 `powershell -NoProfile -ExecutionPolicy Bypass -File` 执行（本机执行策略禁 `-File` 裸跑，Bypass 必带）。**s18 二轮审计注释纠偏批再动 +2**：`AgentLoop.cpp` **+3/−1**（锚路径已知项补登：锚后 tools schema 变化如 connect_mcp 新接入不做差量修正，轻微低估有界、下次 usage 自校正）、`CompactManager.cpp` **+4/−4**（micro/fit 段头注释「每轮整体重估」纠正为增量维护——抽头批前残留，与函数体内注释/AGENTS/tst_compacttokens 三方对齐，净 0）→ 合计 **35168** 行 / 125 文件、对基线 `45f4f81` 差 **+1950**；纯注释零行为变化，增量构建 EXIT=0、实跑 pass=2510 fail=0 skip=1 不变。二轮复核结论：上限链无误（锚快照时机/失效覆盖含 setWorkDir 直接置 −1/overhead 精确对消/队友 refresh 先于压缩闸门/侧栏分母钳界全自洽）；另两条观察留观未动行为——SubAgent 无上下文压缩（覆盖面缺口非计数错误）、队友目标 0.8×满额 vs 主循环 0.8×T′（略早压缩方向安全）。**s18 SubAgent 上下文压缩批（ContextTrim 抽头 + 两侧接线）再动 +103**：新纯头 `ContextTrim.h` **+68**（成组裁剪内核 + 四个比例常量，原 TeammateRuntime.cpp 匿名 ns 常量逐字搬入）、`SubAgent.cpp` **+57/−2**（421→**476**：`subToolsDefinition`/`compactMessages` 单源 + 每请求主动 4/5 + 溢出反应式 1/2 恰一次 + 成功收响应复位旗标）、`SubAgent.h` **+12**（98→**110**：两方法声明 + `m_reactiveCompactUsed` + 类注释）、`TeammateRuntime.cpp` **+12/−44**（733→**701**：内核搬出改一行转发、比例引用改限定名）→ 合计 **35271** 行 / **126** 文件、对基线 `45f4f81` 差 **+2053**；HEAD（`6e5982b`）同口径实测 **35168** / 125 与上一登记值逐字吻合、无漂移，四处合计 68+55+12−32=103 与差值逐字吻合，文件数差 +1 恰为新增 `ContextTrim.h`（口径 = `git show` 落原始字节 + `ReadAllLines`，HEAD 逐文件落盘交叉核对）。新增整套 `tst_contexttrim` 48 条 = 静态点 48（零循环内断言，两数相等，零夹具恒跑：no-op 边界 / 比例常量钉死 / keepHead 保护 / 最旧优先且顺序保持 / 成组配对整单元 / 连续两组 / 孤立 tool 跳过 / 幂等 / changed 旗标当且仅当删过）；三口径套件计数 **38/38/38**；`kSuiteFloors` 求和 1534→**1582** 与本机无夹具实跑 **pass=1582 fail=0 skip=35** 逐条自洽（本批表项 48 恰等于该套件静态点）；带夹具实跑 **pass=2558 fail=0 skip=1**（2510+48=2558），CI 下界 2259→**2302** 按 2558×0.9≈2302 等量上调（本套件零夹具恒跑，带/不带 `LITE_TEST_TMPROOT` 均满足）。**静态点台账同批补记（三批漏记）**：上一登记值 2352（s17.12 批）之后三批加断言未同批回写，本批按登记口径（大小写敏感、行首锚定、`git grep -c` 逐文件求和、限定名扣 3 行 `#define CHECK` 续体）逐修订重测：基线 `4dc993b~` = 1630+539+183 = **2352** 与登记值逐字吻合；`b1413a9`（s17.13.1 三段版本判据）裸 539→558 **+19** → 2371；`4970449`（s18 一段版本判据）裸 558→582 **+24** → 2395；`5dc738f`（月份标签去重）限定名 1633→1645 **+12** → **2407** = HEAD，三批合计 +55 与 AGENTS.md/CI 登记的实跑增量逐条对上。本批工作树 = 限定名 **1693**（扣 3 行续体后净 **1690**）+ 裸 **582** + 宏 **183** = **2455**，对 HEAD 差 **+48** 恰等于新套件全部静态点（其余 37 个旧文件分项零变化）——归因闭合。**坑再登记**：`git grep` 默认不搜未跟踪文件，工作树复算必须带 `--untracked`，否则新套件被静默漏掉、得出「工作树 == HEAD」的假象（本批首测即踩中）。上一批留观项「SubAgent 无上下文压缩（覆盖面缺口非计数错误）」本批兑现——内核经纯头入直测，但 **SubAgent/TeammateRuntime 接线本身仍属不可证区**（TU 绑 QOpenAi 进不了 ctest，主动/反应式触发时机、复位纪律与 keepHead 取值靠编译期等价 + 冒烟）；另一留观项「队友目标 0.8×满额 vs 主循环 0.8×T′」未动。「仍偏大」清单 `TeammateRuntime.cpp` 登记值 728 已过期（token 审计批 +5 未同批回写，HEAD 实测 733），本批 −32 → **701** 同批回写。行数台账同批补正（本仓此前漏记：三批抽头改了被本文登记的源码行数却未同批回写）：`AgentLoopPrompt.cpp` 717→**234**（490 行 schema 搬入 `ToolSchema.h`，本 TU 只留一行转发）、`MemoryManager.cpp` 1117→**857**（八个纯判据搬入 `MemoryRules.h`）、`src/` 合计 32775→**33014**，差 **+239** 逐修订 `git diff --numstat <rev>~..<rev> -- src/` 实测归因：`51b0b4c` **+97**、`5bca6c2` **+41**、`6a37cba` **+98**、热力图绘制转调批 **+3**，四批合计与差值逐字吻合；三批抽头本质是「搬出宿主 TU + 新建纯头」的近零净增，增量主要来自纯头文件头注释。另登记一处判断更正：先前称「doc.md 纯头清单不穷举、新纯头不登记不算漏」不成立——本清单 L6 段实际维护中，`ActivityLedger.h`/`HeatmapGrid.h` 均已入列，故本批三个纯头一并补入。——历史条目保留原值：上一轮登记的 1868 / 2001 已过期——token 紧凑读数批新增 `tst_usagetokens` 33 条（49→82，28 个调用点里 1 个是 6 探针循环，27 + 6 = 33）、再上一轮登记的 1943 / 1798 亦过期——s17.8 会话活动热力图批新增 `tst_activityledger` 58 条、其后压缩回放批新增 `tst_compacttranscript` 40 条、s17.6 P2 批新增 6 套件 98 条，增量只认实跑差值；本轮文档校验按 HEAD 本机实跑：`build/tests/lite-harness-tests.exe` 退出码 0，唯一 skip 是 junction 夹具不可用；口径勘误保留：`42d1134` 提交信息里的 1123 是静态点数、`04eed69` 的 1144 是推算链，两者均非实跑，实跑数以当轮输出为准，登记见 AGENTS.md 测试节）。**s18.2 本地四段轻量压缩卡批 + s18.1 发布批再追加**：只给 `tst_toolnames` 加 9 条 ⑦ 组断言（7→16）、**不新增套件**，三口径仍 **38/38/38**；本机实跑带夹具 **pass=2605 fail=0 skip=1** / 无夹具 **pass=1594 fail=0 skip=35**，`kSuiteFloors` 求和 1585→**1594** 与无夹具实跑逐条自洽（本批表项增量 9 恰等于该套件新增静态点 9，零循环内断言故两数相等）；CI 下界 `LITE_TEST_MIN_PASS` 现值 2302 本批未上调（2605>2302，门禁效力不减，避免逼平赌环境波动）。s18.1 发布批（`432e70d`）**零断言改动**，只把 `CompactManager` 的 prepare 入口观测点由 `LOG_INFO` 降为 `LOG_DEBUG`（降不删；理由与「压缩留痕改由 UI 侧承担」的口径见 AGENTS.md CompactManager 条）。**行数台账同批补记（含两笔漏记归因）**：上一登记值 **35271** 行 / **126** 文件是 `51b1663`（s18 SubAgent 上下文压缩批）的**工作树**值，其后四笔逐修订 `git diff --numstat <rev>~..<rev> -- src/` 实测：`266a4db` +34/−31=**+3**（工作流取消即时终态与运行环断环）、`fd8a470` +55/−10=**+45**（导航分组高度判据改「子项容器是否真在显示」）、`b16a674` +222/−16=**+206**（四段轻量卡，逐文件：`CompactManager.cpp` **+113** / `CompactManager.h` **+27** / 新纯头 `CompactStageNames.h` **+35** / `ToolBlock.cpp` **+17** / `AgentLoop.cpp` **+10** / `AgentLoop.h` **+3** / `MessageBubbleWidget.cpp` **+1**，七处合计 206 与 shortstat 逐字吻合）、`432e70d` +11/−7=**+4**（`CompactManager.cpp` +4，`App.cpp`/`SettingsPage.cpp` 各 +1/−1 净 0），四笔合计 **+258** → 现值 **35529** 行 / **127** 文件（35271+258=35529 逐字吻合，文件数 +1 恰为新增 `CompactStageNames.h`），对基线 `45f4f81`（33218）差 **+2311**。**其中 `266a4db`/`fd8a470` 两笔此前漏记台账**，本批按逐修订实测补记——再次印证「归因一律逐修订 numstat，勿拿两个登记值相减当单批增量」。**基线选取坑（本批首测即踩中）**：误把 `6e5982b` 当上一登记点，得 35168+210=35481 与实测 35529 差 **48 不闭合**；根因是 35271/126 登记的是 `51b1663` 的**工作树**值而非 `6e5982b` 的 HEAD 值（后者原始字节实测 **35168/125**，35168+103=35271 恰为该批净增、125+1=126 恰为新增 `ContextTrim.h`），改以 `51b1663` 为基线后立刻闭合。**故登记点必须点名具体修订并说清是 HEAD 值还是工作树值**，否则复算者会拿错基线（与 s17.12 批「基线一律点名 `45f4f81` 而不再写 HEAD」是同一条纪律的两个变体）。被测生产源需显式列进
     `CMakeLists.txt` 的测试目标清单（当前十个 QtCore-only 生产 TU：TaskStore / MessageBus / WorktreeManager / AgentTeamsManager / McpManager / BackgroundTasksManager / WorkflowManager / WorkflowRuntime / GoalManager / CronSchedulerManager，另加 vendor 侧 `3rdparty/FluentUI/utils/FluConfigUtils.cpp`——它没有 `Q_OBJECT`、依赖的 `FluThemeUtils.h` 只取枚举，故不需 QtGui/QtWidgets 也不需额外 moc，`tst_fluconfigutils` 全靠这一行）。**但 L4 主循环（`AgentLoop` 家族 17 TU、
    `SubAgent`、`TeammateRuntime`）与网络层 `QOpenAi` 仍零覆盖**：阻塞点是 `QOpenAi` 的函数局部 static
    单例无注入点，须先引入 transport 接口才能测。这些改动的验证手段仍是「编译期等价 + 冒烟运行 +
    移动代码逐字一致」，行为回归大面积**不可证**；s15 宿主接线（两拍续打、异步锁存与门、fallback 粘滞、
    bg 唤醒开回合）同属不可证区——直测仅覆盖 `AgentRecovery.h` 纯头与引擎判据；s16 宿主接线（单飞卫兵、
     runner 桥、延迟回填、stop 收口）与真 LLM 编排环同属本区——直测仅以 mock runner 与同步测试形态覆盖
     两引擎判据；s17 宿主接线（评估器异步往返、代际守卫与单飞锁存、Block 回灌续跑、cancelActiveGoal 收口、
     `/goal` 零模型早退分流）与真 LLM 评估环同属本区——直测仅覆盖 `GoalRules.h` 纯头与 `GoalManager`
     引擎判据（`tst_goalmanager`）。
   下一批最小切口（零改动或极小搬移即可入表；原列的 `CronSchedulerManager::validateCron`/`cronMatches`
   已兑现为 `tst_cronmanager`、`AgentPathGuard` 三个 inline 已兑现为 `tst_pathguard`、
   `SessionStore` 的静态方法已兑现为 `tst_sessionstore`）：`BashRunner::dangerWarning`（安全关键却 0 直测）/
   `AgentConst::estimateTokens` / `AgentLoopDetail::toolSummary`
    /`parseToolCall`/`isToolFailure`（困在带 moc 的 TU，需仿 `LineEnding.h` 抽纯头；s15 注：失败文案族本身
    已有直测——`tst_agentrecovery` 契约串逐字、`tst_backgroundtasks` collect 模板与 "Error:" 前缀文案，函数本体仍 0 直测）/ frontmatter 解析。
    **夹具前置是硬要求**：**十三个**依赖夹具的套件（MessageBus / TaskStore 租约 / Worktree / AgentTeams / WorkflowManager / WorkflowRuntime / SettingsCache / FluConfigStore / FluConfigUtils / ConfigMerge / SessionStore / UIUX / ActivityLedger）依赖 `LITE_TEST_TMPROOT`（值须以 `tmp-test` 结尾），缺失即整组
   `TestHarness::skip`——SKIP 不计失败，CI 另以 `LITE_TEST_MIN_PASS` 断言下界兜假绿（见 AGENTS.md 测试节）。
   刻意不引 `Qt6::Test`/moc：测试对象全是纯函数，无信号槽与数据驱动表需求（取舍见 `tests/TestHarness.h`）。
6. **队友 bash 异步桥（Gate③ MINOR-4，Lane B 已落地 0654a48）**：队友前台 bash 经 `setToolAsyncAdapter` 信号驱动（哨兵 `<async-tool-pending>` 截批 + resume 续跑，原同步等待界常量已摘除）；残留=Lead `create_worktree` 仍同步 handler（病态 git 冻结 ≤30s，Gate④ MINOR-B，发布后 P4.1 接异步壳）与异步 git 失败分类无行为测试（NIT-B）。
7. **SubAgent `task` 仍走遗留非租约路径**（裁决：不做租约切换）：`runClaimTask`/`runCompleteTask` 不经租约六门，与团队工具并存是刻意边界。
8. **lcc 线程模型 → 主线程状态机转译（总偏差）**：lcc 守护线程/文件锁 → QTimer 心跳/deferred 驱动/内存台账（D7/D9）；Gate③ 各 MINOR/NIT 的处置与本轮文档同步（NIT-4）见 `.slim/deepwork/port-s13-agent-teams.md` 台账与 `gate3-oracle-report.md`。
9. **压缩计量：循环内 O(n2) 全量重估已消除（本轮改），残留降为每请求常数次 O(n)**：**已改**——`microCompact` 与 `fitToolResults` 原为「循环内每轮对整段历史重算 `estimateTokens`」（每条工具结果一次全量重算 = O(n2)，且跑在 GUI 线程，叠加零线程约定直接表现为流式输出期可感知卡顿），现改为循环前置一次总量、每次替换做 `total += 新项 - 旧项` 的增量维护。**等价性是可证的，不是「应该没问题」**：计量已抽成纯头 `CompactTokens.h`，其不变量①（逐条独立求和、项间零耦合）使增量值与此刻全量重算**逐值恒等**（数学恒等而非近似）；`tst_compacttokens` 把该恒等式钉成断言，含同款替换序列的逐步对表，改口径即转红。两处提前 `continue`（短结果豁免 / 落盘失败写错误串）与条件替换分支均按「未改写则 total 不动」处理；`processCount<=0` 与空 `indices` 的前置 return 保住原「零次估算」路径不劣化（勿改成无条件前置算一次）。阈值语义、比例、落盘时序、文案逐字未动。**残留（未改，量级已不同）**——`prepareAsync` 各段之间仍有 4 次全量重估（每段一次，O(n) 非 O(n2)），`AgentLoop::estimatedContextTokens` 的锚失效兜底路径与 `contextOverheadTokens` 每次发请求各算一遍全量；这些是每请求常数次 O(n)，不再是卡顿主因，要进一步压需先给 `estimatedContextTokens` 补可测切口（它困在带 moc 的 L4 TU，同 §9.5 阻塞点）。**验证手段**：`tst_compacttokens` + 清单篇「五、P1 异步链与压缩」组——「压缩后历史体量 ≤0.8×T′」即触发点未漂移的实测判据。
10. **`AgentConst` 取值函数每次构造 QSettings（本轮改，走「缓存解析产物 + 文件身份惰性重载」）**：原状——`contextCharLimitValue()`/`maxToolIterationsValue()`/`maxRetriesValue()`/`textValue()`（`modelOptions`/`defaultModel`/`fallbackModelIdValue` 共用）每次调用都现构造一个 `QSettings`，而消费点已下沉到 per-tool 级（`ChatSessionPage` 每个工具结果读一次 `contextTokenBudget()`），头文件注释「调用频率为回合级/交互级，读盘成本可忽略」的断言不成立。现状——读侧一律经 `AppSettings::value()` → `SettingsCache::Reader`（`src/SettingsCache.h`，header-only 纯头，仿 `CompactTokens.h` 先例：`CompactManager` 那条「有非 QtCore 依赖的 TU 进不了测试清单」的阻塞点在这里同样存在，`AppSettings` 绑死 `applicationDirPath()` 使门面本身不可测，故把可测的部分抽成路径可注入的 Reader）。写侧收敛到 `AppSettings::setValue()`，全部 8 处写点（`SettingsPage` 6 + `ChatSessionPage` 1 + `I18n` 1）与全部读点（含 `QOpenAi::initFromSettings`、`NewChatPage`）一并迁移，**不留混用**：混用会让「一处写、另一处读到旧值」成为常态 bug。`AppSettings::ini()` 随之**摘除**——留着它等于保留「每次现构造」的复发通道，需要 QSettings 全量能力时应给 Reader 加方法并配测试。**两条当初被否决的方案，否决理由要记下来**：①「缓存 `QSettings` 实例并按值返回」不可行——Qt 6.9 的 `QSettings` 继承 QObject 且 `Q_DISABLE_COPY`（`qsettings.h:183`），既不可拷贝也不可移动，现有 `ini()` 能按值返回纯靠 C++17 保证复制消除，返回一个已存在的缓存实例就需要真拷贝、直接编译不过；故缓存的只能是**解析产物**。②「简单 static 缓存」是行为回归——README「运行时配置」节明确支持「其余手改配置文件即可」，永不失效的缓存让手改值永不生效。**时效权衡（用户已接受）**：失效判据 = 文件身份三元组 `(exists, mtime_ms, size_bytes)`，与 Qt 自身 `syncConfFile()` 的只读短路判据（size + lastModified，`qsettings.cpp:1346`）同族，故本缓存**不引入 Qt 之外的新 staleness 类别**；盲区是「同一毫秒内的等长改写」——文件系统时间精度到毫秒，若手改恰好等长且同毫秒则判不出变更。该盲区由写侧显式 `invalidate()` 覆盖本进程自己的写（`setValue` = 写 + `sync()` + 失效，顺序不可颠倒：先失效会让中间那次重解析读到磁盘旧内容并把旧值再缓存一轮），剩给「外部编辑器恰好同毫秒等长改写」这一格，实际不可达。**口径纠正（读 Qt 源码所得，勿再沿用旧说法）**：原登记说每次读「open + parse 整个 ini」**不准确**——Qt 有进程级 `ConfFileCache`（`qcache` + `QConfFile::fromName`）且只读时 size/mtime 未变即短路，不重读磁盘。故本修复省掉的是每次调用的对象生命周期与簿记：`new QSettings`（QObject + d 指针）+ 全局锁 `settingsGlobalMutex` + 缓存探测 + `QFileInfo` stat + `QSettingsKey` 大小写折叠查表 + 析构里的 `sync()`/ref 回插。收益真实但不大，别按「消除了重复磁盘解析」宣称。唯一净增益格是「文件缺失或为空」：Qt 的短路要求 `size > 0`，缺失/空文件它每次构造都走读盘分支，本缓存把该态也缓存住（`tst_settingscache` 组 1b 钉死）。**另两条实测事实进了不变量**：①键折叠口径必须与所用后端**成对**——ini 后端的 `IniCaseSensitivity == Qt::CaseInsensitive`（`qsettings_p.h:54`）下 Reader 存取两侧 `toLower()` 归一，否则「手改时大小写打错」这种以前能用的写法会突然失效；而合流后的 json 后端在 `FluConfigStore::jsonFormat()` 里注册为 **`Qt::CaseSensitive`**（vendor 自己的语义，宿主不得为迁就缓存去改它），此时缓存就得**逐字**查——口径配错不会报错，只会给出「缓存 hit、直取 miss」两个不同答案，故 `AppSettings::keyCaseSensitivity()` 与 `fileFormat()` 成对交出，`Reader` 的缺省仍取 ini 那一对（只走 ini 的调用点零改动）。②`iniEscapedString`（`qsettings.cpp:592`）见 `,`/`;`/`=` 就加引号，故「裸逗号串被解析成 `QStringList`」只在**人手编辑**时发生，程序写侧产出的是带引号 QString——`textValue` 的归一化分支据此在 `tst_settingscache` 组 4b 用裸文件复现并锁 ini 侧形态。**合流到 json 后新增的第二形态（本批实测回归）**：自定义格式的 `flatten()` 走 `QJsonValue::toVariant()`，数组落成 `QVariantList`（Qt 6.9 实测 typeId=9）而**不是** `QStringList`，旧写法只判 `QStringList` 就会对它 `toString()` 得空串、把用户手写的 `"modelOptions":["a","b"]` 静默判成「未配置」并回退内置清单；故 `normalizedText()` 两个判据并列，json 侧形态由 `tst_configmerge` 组 C 用 `metaType()!=QStringList` + `toString().isEmpty()` 两条现场取证钉住。**验证手段**：`tst_settingscache`（十四套件之一，实跑 89 条 = 恒跑组 35 + 夹具门控组 54）——身份判据穷举、缺失态、200 次热读只装载一次（`loadCount()` 观测口，把「命中零构造」从口头承诺变成可断言事实）、四类外部手改（等长/变长/新增/删除）立即生效、缓存与现取逐键一致（含类型与大小写变体）、裸逗号串 QStringList 形态不被抹平、写侧落盘与失效成对。**残留**：Reader 无锁，依赖全仓零线程约定（不变量⑤）；若将来引入工作线程读配置，须先给 Reader 加锁，而不是把缓存挪到线程局部。`AgentConst` 各取值函数的「范围校验/回退默认」语义逐字未动，本改动只换「值从哪来」。
11. **UI 侧「明确不做」清单（已评估并裁决，勿下轮重复提议）**：原 `docs/ui-review-p2p3.md`（UI 评审
    P2/P3 执行清单）已按 §1.3 第 4 条「零散笔记不在 `docs/` 落地」收回本文并删除——P2 五套件与 P3 六项
    均已落地，其明细与断言口径以 **AGENTS.md 测试节为单源**（覆盖面轮廓见 §9.5），勾选状态以提交信息为准，
    清单本身不再单独落盘。只有下面五条「评估过、决定不做」的判断需要长期留档，理由逐条随附：
    ① **QSS 三主题去重**：三份 `stylesheet/<theme>/` 是 FluentUI 换肤契约的一部分，合并成「公共 + 覆盖」
       会让改一处颜色需要读两层规则，调试成本高于重复成本（重复面本身见 `AGENTS.md` 主题节的逐条对表）。
    ② **`tr()` 重复字符串合并**：同一文案在不同上下文里可能独立翻译（如「取消」在设置页与审批卡语气不同），
       合并会锁死这种自由度，收益仅字数。
    ③ **设置页卡片类拆分**：`SettingsPage.cpp` 体量确实偏长（见 §9.4 候选清单），但拆分类会新增 6 个文件
       与跨文件成员访问，收益是「读起来短一点」，不改变任何行为——不满足「改动即收益」的门槛。
    ④ **流式文本批量合并**（`MessageBubbleWidget::appendText` 每 delta 一次文档插入）：收益是插入次数降
       5–20 倍，但要在「切段 / 收尾 / 读全文」三处补 flush，漏一处就丢字——风险与收益不成比例。
    ⑤ **历史回放虚拟化**（长会话 N 个气泡 widget 各自持有 `QTextBrowser` + `QTextDocument`）：这是内存占用
       最大的一块，但虚拟化会重写回放与滚动跟随的既有契约（气泡高度按需测量、滚动锚点、时间线顺序，
       见交互篇 §12），属结构性改动，须单独设计后再动。

---

### 10. 新增代码该落在哪（决策树）

**新增一个 AgentLoop 成员方法** →
① 它属于哪条链路？请求/压缩前导 → `AgentLoopRequest.cpp`；提示词或工具 schema → `AgentLoopPrompt.cpp`；
分发或收口 → `AgentLoopTools.cpp`；文件沙箱工具 → `AgentLoopFileTools.cpp`；bash/后台任务 →
`AgentLoopBash.cpp`；权限判定 → `AgentLoopPermission.cpp`；钩子 → `AgentLoopHooks.cpp`；
历史读写 → `AgentLoopHistory.cpp`；技能 → `AgentLoopSkills.cpp`；todo → `AgentLoopTodo.cpp`；
子代理 → `AgentLoopSubAgent.cpp`；cron → `AgentLoopCron.cpp`；**团队（队友生命周期/邮箱唤醒/审批翻转/工作树接线/退出清算）→ `AgentLoopTeam.cpp`**；**goal（终局门/评估器异步环/代际与单飞/第七路收口/零模型早退分流）→ `AgentLoopGoal.cpp`**；生命周期/会话身份 → `AgentLoop.cpp`。
② 有 ≥2 个 TU 共用的纯函数？→ 声明进 `AgentLoopInternal.h`，定义落「语义最贴近」的那个 TU，**不加转发层**。
③ 只在单 TU 内用？→ 留在该 `.cpp` 的匿名 namespace，别上头文件。

**新增一个引擎（无 GUI 依赖）** → L5：自带 `.h/.cpp`，宿主信息经构造注入 sink/回调，不 include
`AgentLoop.h`；GLOB 自动纳入构建。
**新增一个带样式的控件** → L3：`src/*.cpp/.h` + 三主题 `stylesheet/<theme>/<Widget>.qss`，构造尾
`ThemeAware::bind`；不改 `CMakeLists.txt`。
**新增一个用户可见设置项** → `AppSettings.h` 加键 + `SettingsPage` 加卡 + 取值经 `AgentConstants.h`
的取值函数（参考 `contextCharLimitValue()` 的「默认值 + 校验界 + 单点读取」三件套）。

---

<a id="checklist"></a>
## 第三部分 · 手工冒烟回归清单

> 沉淀历轮验收场景为可重复执行的手工回归清单。自动化测试仅覆盖 `tests/`（当前 `LineEnding.h` 行尾口径，`ctest --test-dir build -C Release --output-on-failure` 跑），无 lint；其余仍靠构建+运行+目测验收。

> **用法**：按改动面选组执行——P0 每次 src/ 改动必跑；P1 按受影响域跑；P2 里程碑/发版前抽查。
> **基线环境**：Qt 6.9.0 msvc2022_64 + MSVC 2022 + CMake，仅 Windows；模型服务凭 `config.json` 键 `apiBaseUrl` / `apiToken`（主窗口构造期 `QOpenAi::initFromSettings()` 读取），模型下拉取 `modelOptions` / `defaultModel`（内置首项 `qwen3.8-flash`）。
> **配置存储**：`<config>/config.json`，`<config>` = `<exe 目录>/../config`（路径单源 `LitePaths::configFilePath()` → `AppSettings::filePath()`，读写门面 `AppSettings::value()`/`setValue()`，缓存与失效判据在 `SettingsCache.h` 的 `Reader`；**`App.cpp` 在建根处 `QDir().mkpath(LitePaths::configRoot())` 且早于任何读写**——`SettingsCache::Reader` 不建目录，QSettings 往不存在的目录写会静默 AccessError，表现为「设置页改了没反应」且无报错，而打包时 CMake 的 staging 清理会把 config 目录从产物里剥掉，发布包首启根本没有它）——注册表方案已随用户裁决废弃，禁再引入默认构造 `QSettings`，也禁在消费点现构造 `QSettings` 绕开读缓存（原逃生口 `AppSettings::ini()` 已摘除，理由见 §9.10 第 10 条）。**FluentUI 自己的配置**（主题/语言镜像/导航宽度等）与宿主配置**合流成同一个 `config.json`**：`App.cpp` 在**任何 `FluConfigUtils::getUtils()` 之前**恒调 `FluConfigUtils::configure(LitePaths::configRoot(), LitePaths::configFileName(), FluConfigFormat::Json)`，三段一律取自单源、不留可配面（原 `fluConfigDir`/`fluConfigFile`/`fluConfigFormat` 三键已删除——它们定位的正是本文件自身，读键得先知道文件在哪，成了自指；且一旦允许注入出另一个路径，宿主与库就会各持一份内存表写两个文件，合流当场失效）；判据单源在 `3rdparty/FluentUI/utils/FluConfigStore.h`（纯 QtCore 定位层，JSON 走 `QSettings::registerFormat("json",…)` 自定义后端，注册为 `Qt::CaseSensitive`，落盘为嵌套对象且键名保持驼峰）。下文 `S` 指声明的上下文窗口上限（config.json `contextCharLimit`，**s17.9 起单位是 token**，键名沿用不改是为兼容既有配置）；`T` 指全局 token 预算（`contextTokenBudget()` **= S 本身**，触发与 UI 均此口径；旧写法 `≈ S/4` 已废——那个 ÷4 假设内容是英文、而分子 `estimateTokens` 对中文按 1:1 计，两侧口径相反）；`T′` 指 T 扣除 system+tools+注入 overhead 后的会话体预算。

### 一、P0 启动与构建

- [ ] 子模块前置 `git submodule update --init 3rdparty/FluentUI` → checkout 成功，configure 不再缺 `FluentUI::Controls`（`3rdparty/lcc` 从不入构建图无需 init；`3rdparty/sqlite` 已作死码删除）
- [ ] 干净 configure `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64` → 零报错，GLOB 拾取全部 `src/*.cpp|.h` 与 `stylesheet/**/*.qss`（新增源文件重 configure 自动拾取、不改 CMakeLists.txt）
- [ ] Debug 构建 `cmake --build build --config Debug --target lite-harness` → 0 error；src/ 各编译单元 `/W4` 0 告警（FluentUI 第三方豁免）
- [ ] 重链前先结束所有运行中的 `build/bin/lite-harness.exe`（含用户自开实例）→ 链接成功；若见 LNK1168 属已知坑（Debug/Release 共用 `build/bin/` 互相覆盖），非代码缺陷
- [ ] 双击 `build/bin/lite-harness.exe` → 主窗口存活 ≥10 秒无崩溃，标题栏（32px 手绘）与导航列渲染正常
- [ ] `config.json` 未配 `apiBaseUrl` / `apiToken` 启动 → 窗口正常显示、构造期 qWarning 提示未配置，仅在发起对话时以回合内错误文案体现（不闪退）

### 二、P0 会话生命周期

- [ ] 新建对话页发起 → 导航「会话」下新增子项并进入对应会话页；标题为空显示「新会话」，超 12 字符截断加「...」
- [ ] 检查 `<config>/config.json` 的 `app/sessions` 键 → 数组，条目含 dataId/title/model/workDir/createdMs；写盘经 QSettings（`syncConfFile` 内 `QSaveFile`+`commit`，异常退出不留半文件）。注意：`workDir` 只是条目字段，索引落点与所选工作目录无关
- [ ] 会话内发若干消息 → `<会话数据根>/.lite-harness/sessions/<dataId>/history.json` 存在且与可见消息一致（dataId 为 8 位 hex）
- [ ] 重启程序 → 会话按 createdMs 升序恢复、历史气泡回放完整；恢复不自动切换当前页（仍停在新对话页）；workDir 失效时静默回退进程当前目录
- [ ] 会话历史损坏（手工把 `history.json` 截半/写坏）→ 打开该会话弹「历史加载失败」Error 提示（`kNoticeErrorMs` 自动消失）并按空会话打开；全新会话（无 ID / 无文件）静默空页、**不**弹提示（判据：会话标题非空才提示）
- [ ] （**压缩后重启回放**）触发一次全量压缩（`compact` 工具或自动压缩）→ **会话气泡时间线**出现压缩卡（非侧栏——侧栏变的只是占用条与数字，`SessionSidebar` 无压缩卡）；摘要侧链挂起期间先见「压缩中」事前 live 卡，终态就地收成「已压缩」✓；**退出重启** → 打开同一会话，摘要之前的原始对话按原顺序回放出来（含工具块），摘要消息仍在原位；模型上下文仍是压缩后内容（侧栏占用读数与压缩后一致，非全量）。反例判定：重启后只剩一条 `[Compacted] …` 摘要 = 回放没接上（转写缺失/判据漂移）。另验降级：手工删掉该会话 `.transcripts/transcript_*.jsonl` 后重启 → 仅剩摘要、不弹错、不崩
- [ ] 右键会话导航子项 → 仅弹「重命名 / 删除会话」菜单；单击子项不因基类 itemClicked 误翻页（eventFilter 吞 press/release）
- [ ] 键盘上下文菜单：选中会话子项后按 Shift+F10 或菜单键 → 同样弹「重命名 / 删除会话」；鼠标右键与键盘**各弹一次**（鼠标路径走 press/release 延拍、键盘走 ContextMenu，两条路不得双弹）
- [ ] 重命名：输入新名确认 → 导航与索引标题同步（索引不再刷 `lastActiveMs`，该字段已删）；取消/空/未变 → 保持原值
- [ ] 删除：弹「此操作不可撤销」确认 → 确认后页面销毁、索引条目移除、`sessions/<id>/` 整目录递归清理；删的若是当前页则切回新对话页；运行中会话先 `stop()` 再删；数据清理失败仅 qWarning、UI 状态照常收清
- [ ] 有会话运行中点窗口关闭 → 弹「任务仍在运行，退出将丢失未完成回合。确定退出吗？」，**默认键钉在「取消」**（`FluMessageBox` 无 Yes/No 形态，只有确定/取消；不钉则实测回车与空格都命中排首位的「确定」＝直接丢未完成回合，属数据丢失级回归）；点「取消」/按 Esc → 不退出、任务继续；点「确定」→ 逐页 `stop()` 后退出

### 三、P0 Agent 主循环与权限门

- [ ] 工具名单核对（读码 `ToolNames.h` + 运行时注册观察）→ 恰 27 工具：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron / spawn_teammate / list_teammates / send_message / request_shutdown / request_plan / review_plan / create_worktree / connect_mcp / workflow；任务图 6 + cron 3 + 团队 7 + MCP 入口 connect_mcp + 编排入口 workflow 仅主循环注册（connect_mcp / workflow 为 Lead 独占，SubAgent 白名单与队友 schema 不含此名）；`submit_plan` 为队友专属名不在 `ToolNames.h`（引擎匿名 ns 单源，Lead 面不可见）
- [ ] `connect_mcp` 调用任意 server 名 → 空注册表回流 `Unknown server … Available: (none)` 错误文本、工具调用不崩溃、回合不中断（s14 空注册表口径）
- [ ] bash 执行普通命令（如 `echo`）→ 不触发审批卡直接执行
- [ ] bash 触发硬拒绝表（`rm -rf /`、`sudo`、`shutdown`、`reboot`、`mkfs`、`dd if=`、`> /dev/`；大小写不敏感 contains）→ 直接回填 `Blocked: … is on the deny list`，不弹询问
- [ ] bash 触发 ASK 规则（含 `rm ` 等破坏词「Potentially destructive command」）或 write_file/edit_file 越出工作目录（「Writing outside workspace」）→ 内联 PermissionCard 出现、默认焦点「拒绝」、理由中文与 EN→ZH 映射表一致（新增 reason 必须同步该表）
- [ ] 审批卡点「拒绝」→ 历史回填 `Permission denied`、LLM 续跑下一轮；点「允许」→ 执行且同工具不再二次询问（仅跳过询问规则，硬拒不受批准影响）
- [ ] （**s15** 异步回合 fail-closed）在 cron 定时 / 团队唤醒 / 后台唤醒触发的回合里命中 ASK 确认规则或 `mcp__` confirm 策略 → **不弹审批卡**、历史回填逐字定值拒串（单源 `RecoveryRules::asyncApprovalDenied()`："Permission denied: interactive approval is unavailable during an asynchronous turn"），工具气泡判**红标**（`isToolFailure` 对 `Permission denied` 前缀判定）；bash 硬拒表仍先于一切拒绝、已 granted 的同工具照常放行不弹卡（deny 表与 granted 短路顺序不动）
- [ ] （**s15** 回归）异步 fail-closed 不影响普通用户敲键回合：命中 ASK 照常出卡、允许/拒绝路径与上一致
- [ ] （**s17.2** 审批总开关）设置页「权限」→「工具执行权限审批」切到「无需审批」→ 落 config.json 键 `permissionApproval`=false，**下一个工具调用即生效**（无需重启、无需新会话）：命中 ASK 规则（`rm ` 破坏词 / write_file 越界）不再出卡而直接执行、`mcp__` confirm 与 workflow confirm 同放行、异步回合（cron/团队/bg 唤醒）也不再回填 `asyncApprovalDenied()` 定值拒串；**仍拦的三类定值硬拒**：bash 硬拒绝黑名单回填 `Blocked: … is on the deny list`、workflow 的 deny 定值串 "Permission denied by host policy"、文件工具 `safePathIn` 越界错误（撤的是「问不问」，不是「许不许」）；队友侧注入④同源同开关（关后队友不再收 `Permission required:`）；切回「需要审批」后各询问门原样恢复；键缺失/写成认不出的串一律按 true（fail-safe 多问一次）
- [ ] （**s16** workflow 基础环）普通回合调 `workflow`（name="review-changes"，args={changes:…}）→ allow 策略放行不弹卡；九次 agent 编排（audit→逐 finding 对抗 verify）后工具回填一段汇总 JSON 信封（含 runId/result）；请求工具池 27 个 function 定义含 workflow schema（信封单源 `WorkflowManager::toolInfo()`）；注入块第五段列注册清单（"Registered workflows…"），system 尾编排纪律段字节恒定（前缀缓存不破坏）
- [ ] （**s16** workflow 三态门）普通回合调 `workflow` 传未登记名/空名 → fail-closed 归 confirm、弹卡「工作流策略要求确认」（中文词条与 `translateReason` 表一致）；批准 → 仅派发一次（ASK 挂起先于 workflow 特判，批准重入不双派发）、拒绝 → 回填 `Permission denied`；异步回合（cron/团队/bg 唤醒）调 workflow（confirm）→ **不弹卡**、逐字 `asyncApprovalDenied()` 定值拒串回填判红标；deny 定值串 "Permission denied by host policy" 在内置注册表下 E2E 不可达（deny 表空），由读码 + 单测门禁覆盖（`tst_workflowruntime.cpp` 注）
- [ ] （**s17** goal 零模型早退四形）输入 `/goal`（status）、`/goal clear`（含 stop/off/reset/none/cancel 六别名）、空条件、超 4000 字符四形 → 即时回显状态/结果气泡，**无悬空「处理中…」占位泡残留**（handler 按 isRunning() 两形判别、零模型形 deleteLater 收泡，禁 finishStreaming）、不入历史、零模型请求
- [ ] （**s17** goal 自动续跑环）`/goal <完成条件>` 设定后模型停手时经独立评估器（无 tools、max_tokens 512）判定：Block → "[Goal still active]" 回灌自动续跑；achieved/limit（连续 block 达 stopHookBlockCap）/error → goalNotice 状态回显 + 该轮真回复正常定稿**不双份**、goal 保持 active 不静默清除；bg 任务 running 期间终局 → defer 放行且评估器 0 次调用
- [ ] （**s17** goal 停止收口）评估器往返中（回合 running）按停止 → 第七路 `cancelActiveGoal`：丢挂起终局、迟到回填被卫兵静默丢弃、**全路径零 emit** 无幽灵气泡；随后正常回合不受影响
- [ ] bash 运行超 120 秒命令 → 回填 `Error: Timeout (120s)`（`kBashTimeoutMs` 单源），回合继续不卡死
- [ ] 设置页「单轮最大调用次数」：合法值（如 50）落盘 → 下一回合生效（`run()` 入口快照 `maxToolIterationsValue()`，回合中途不变）；输 0 / 1001 / 非数字 → 弹「请输入 10 ~ 1000 之间的整数。」不落盘
- [ ] `config.json` `maxToolIterations` 缺失或非法（含手工篡改越界）→ 回退默认 500
- [ ] 回合进行中点停止 → 依序：子代理先级联取消（合成 `(cancelled)` 配对回填）、workflow 在途收口（s16 第六路：清槽→requestCancel→在途 runner cancel→半批 `(stopped)` 直写保配对、零 emit）、在途工具批收口（已完成批 flush + 未完 pending 每条合成 `(stopped)`）、QProcess 杀、SSE 流断、错误气泡「已停止。」；随后立刻发下一条 → 上游不报 400（tool_use/tool_result 配对完整）
- [ ] 压缩请求（prepare/批尾/反应式共用 sideRequest）进行中按停止 → 历史不被摘要替换（取消后 done 永久静默）
- [ ] 技能（`load_skill`）→ 技能目录固定 `<config>/.lite-harness/skills/<name>/SKILL.md`（单源 `LitePaths::skillsDir()`；**全局共享一套**，不随 `sessions/<dataId>` 隔离、也**不再按 workDir 隔离**（用户裁决））；`load_skill` 按名返回全文；技能**清单**（名 + 描述）进请求尾部注入块、正文不进 system 也不进注入块（D2 裁决：清单回填、正文按需加载）；无技能 + 无记忆时不产生空注入块
- [ ] 任意工具失败（read_file 不存在的文件等）→ 仅折叠为 tool_result 字符串交还 LLM，全程不抛异常、不弹窗
- [ ] 子代理（`task`）内 bash 失败同样可判定 → 非零退出码回喂带 `Error: command exited with status N:` 前缀（与主循环前台/后台共用 `formatBashResult`）；PowerShell 起不来由 `errorOccurred` 显式收口回 `Error: bash 启动失败…`；不得把失败洗白成裸输出或 `(no output)`（曾如此）；取消后仍静默丢弃输出、`onToolFinished` 恒一次
- [ ] `edit_file` 行尾两级匹配 → CRLF 文件上用**从 `read_file` 输出抄来的 LF 多行** `old_string` 能命中（曾必然报 text not found）；写回后全文件无裸 LF（不产出混合行尾）；LF 文件保持 LF 不被转成 CRLF；单行替换与「只替换第一处」语义不退化；混合行尾文件走 LF 归一化回退、写回按主导行尾归一
- [ ] `edit_file` 编码防线 → 非 UTF-8 文件（如 GBK 源文件）返回可判定错误拒绝编辑，**不得**写回把非法字节永久替换为 U+FFFD；带 UTF-8 BOM 的文件既能正常编辑（`fromUtf8` 会吃 BOM，守卫须按去 BOM 后的正文比较，否则误拒）、写回后 BOM 原样保留（不被静默删除）
- [ ] `edit_file` 三道防线 → ① 空 `old_string` 返回可判定错误拒绝编辑（**不得**把 `new_string` 静默前插到文件开头）；② 超过 `kEditFileMaxBytes`（5MB）的文件拒绝编辑并回体量错误（**不得**整文件读入冻结主线程，也不得部分读写毁文件）；③ `old_string` 在文件中命中多处 → 返回「命中 N 处，拒绝编辑（请补充上下文使其唯一）」，**不得**静默替换第一处（有意偏离 lcc `str.replace(old,new,1)` 语义）
- [ ] 单元测试 → `ctest --test-dir build -C Release --output-on-failure` 全绿（产物 `build/tests/lite-harness-tests.exe`，需 Qt bin 在 PATH 供 `Qt6Core.dll`）；测试 exe **不进** CPack 包（输出目录与 `bin/` 分开，staging 只取 `bin/`）
- [ ] `write_file` 行尾保真 → 覆盖已存在的 CRLF 文件时沿用 CRLF
（模型给的 content 天然是 LF，直写会翻转整文件行尾、产出全文件 diff 噪声）；新建文件按 content 原样落盘不臆造行尾；主导行尾只读开头 `kEndingProbeBytes`（64KB）窗口判定，已知边界：窗口内无换行的超长单行文件判为 LF
- [ ] 上下文占用计量（token 锚定）→ 端点回 `usage.prompt_tokens` 时侧栏占用条与条下数字行取锚值 + 锚后增量（**锚路径不另计 overhead**，prompt_tokens 已含 system/tools/注入真值）；端点不回 usage（部分兼容端点）→ 回落本地全量估算，占用条仍单调合理；压缩改写历史后锚作废（`m_historyRewrittenSinceAnchor`）→ 下一回合重新锚定，不得沿用旧锚
- [ ] 任务图 6 工具行为冒烟（不止静态名单核对）→ `create_task` 落 `<会话根>/.task/task_<hex8>.json` 一任务一文件、`list_tasks`/`get_task` 每次直读磁盘（外部改文件即时可见）、`update_task` 加依赖用返回的 task id、`claim_task` 仅对依赖已完成的 pending 成功、`complete_task` 后状态翻转且不得重复完成他人任务；任一失败一律折叠为工具输出字符串，不抛异常
- [ ] （**s16** 生产路径台账接线，**专防接线回归**）真实会话里直接调 `workflow`（如 `name=review-changes`）→ **不得**报 `Error: workflow store dir is not configured`；跑过后 `<会话根>/.workflows/` 出现且含 `<runId>.json` / `<runId>.journal.jsonl` / `<runId>.output.json` / `last_run.txt` 四件套。成因：`WorkflowManager::setStoreDir` 是 push 形、宿主 AgentLoop 是唯一注入点，而 workDir/会话 ID 自 s12 起改走构造注入（init-list 定值、构造体**不再**调用 `setWorkDir`）——s16 落地时把首推登记在 `setWorkDir` 内并注「构造体首调本函数即完成首推」，该前提已失效，两个推送点生产零调用方（`ChatSessionPage.cpp` 的 `m_sidebar->setWorkDir` 是 `SessionSidebar` 同名方法，非本函数），workflow 工具在生产路径整体不可用。**单测证明不了这条**：`tst_workflowmanager/tst_workflowruntime` 共 20 处直接 `mgr.setStoreDir(...)`，绕过注入面而全绿（AgentLoop 带 Q_OBJECT 需 moc、又链 QOpenAi（Qt6::Network），按 CMakeLists 的 QtCore-only 清单纪律进不了测试目标），故接线只能在冒烟运行验
- [ ] （**s16** 单飞卫兵）workflow 在途运行时第二个 `workflow` 调用 → 立即回填 `Error: another workflow run is already active` 判红标，不打断在途运行、不弹卡不挂起队列
- [ ] （**s16** resume cached）以既往 runId 调 `workflow`（name 一致 + `resume_from_run_id`，prompt/schema 未变）→ 各步骤 status=cached、零模型调用、信封重建；journal 损坏/脏缓存 → 引擎拒用缓存重跑（HIT 脏缓存拒用为单测门禁，冒烟验好路径不崩）；台账落 `<会话根>/.workflows/`（`<runId>.json` 快照 / `<runId>.journal.jsonl` / `<runId>.output.json` / `last_run.txt`）
- [ ] （**s16** stop 收口）编排进行中按停止 → `cancelActiveWorkflow`：先清槽 → `requestCancel`（置旗，终态下一拍折叠 failed）→ 在途 runner 请求 cancel → 半批 tool_result 以 `(stopped)` **直写历史保配对、全路径零 emit**（析构同点复用，红线与 settleTeamOnExit 一族）；UI 无多余报错，随后立刻发下一条不报 400

### 四、P1 设置与数据路径

- [ ] 设置页六卡说明位约定（共用基类 `InfoSlotSettingCard`）：值显示在**标题下方说明位**（基类 `m_infoLabel`，12px 次要灰），格式「简短解释：值」；解释前缀**恒完整、只省略值**（对整串 `ElideMiddle` 会把省略点切在交界上、说明与值两头都残，实证见 W=420 组）；右侧操作行只剩按钮；长值 ToolTip 兜底全量；分隔符走 `tr("：")`（英文回落 `": "`，其尾随空格经 lrelease + QTranslator 实测保留）。API Key 卡进说明位的**必须**是 `maskedApiToken` 摘要，明文绝不进值区也不进 ToolTip。已知边界：窄到连前缀都放不下（实测 ≤300px / infoW≈88）时退化为整串省略
- [ ] 设置页「默认工作目录」：选新目录 → `config.json` `defaultWorkDir` 更新；对话框取消 → 保持原值；清除 → 展示「未设置（使用进程当前目录）」
- [ ] 写盘失败可见：把 `config.json` 设为只读后改任意设置项（工作目录 / 端点 / 密钥 / 模型清单 / 上下文上限 / 迭代上限 / 审批开关）→ 弹「保存失败」提示（文案含 `config.json` 与「只读或被占用」），卡片值区保持旧值；恢复可写后重试 → 正常落盘且不再弹。注意：失败后**回读可能显示新值**（QSettings 进程级 `QConfFile` 缓存），故判据只看是否弹框、不看值区
- [ ] 设置页「上下文上限（token）」改为 300000 → 落盘即下一回合压缩管线现取生效（无需重启）；卡片展示千分位（QLocale::c() 固定，不随界面语言变），数值行**只有 `300,000 token` 一个数、不再有「≈N token」派生括号**（那个括号就是旧 ÷4 的化身，s17.9 丁-3 起随换算一起删除）；**三处必须同一个数**：设置页显示值 == 侧栏占用分母 == 压缩门槛基准（键名仍是 `contextCharLimit`，语义已转 token 域，改名会让既有配置静默失联）
- [ ] 上下文上限输入 9999 / 5000001 / 非数字 → 「无效数值」弹窗不落盘；输入带千分位的 `200,000` → 剥逗号解析成功；合法界 10000~5000000、默认 200000
- [ ] 键位巡检 → `language` / `defaultWorkDir` / `sidebarVisible` / `contextCharLimit` / `maxToolIterations` / `maxRetries` / `apiBaseUrl` / `apiToken` / `modelOptions` / `defaultModel` / `permissionApproval` 均落于 `<config>/config.json`（`<config>` = `<exe 目录>/../config`，单源 `LitePaths::configFilePath()`；手工编辑重启即生效，注册表路径不再被读写）
- [ ] 模型清单可配置：`config.json` 写 `"modelOptions": ["qwen-a", "qwen-b"]`（数组形态）→ 下拉恰列两项且顺序一致；写成 `"qwen-a,qwen-b"` 单串形态同样读回两项；手改首项为 `qwen3.8-flash,qwen3.8-max` 读侧解析成功（`textValue()` 归一化列表串，回归点：曾误判「未配置」静默回退内置清单——ini 侧裸逗号串→`QStringList`、json 侧数组→`QVariantList`，两种形态都得认）；清单为空/全空白 → 回退内置两项；条数超 32 截断
- [ ] `defaultModel` 指向清单内模型 → 生效；指向清单外或留空 → 回落清单首项（防配置指向不存在的模型）
- [ ] 设置页改「模型服务」端点/密钥 → 写配置文件后立即 `QOpenAi::setUrl()` / `setToken()`，下一回合请求即用新值（无需重启）；密钥卡默认掩码、点眼睛切明文且焦点不跳字
- [ ] 会话数据目录拼法目测+读码（`AgentConstants.h`）→ `.task` / `.temp` / `.transcripts` / `.task_outputs/tool-results` / `.memory` 零偏差（拼法涉数据兼容，改名即回归）；任务图每任务一个 `.task/task_<hex8>.json`
- [ ] **s17.11 项目指令文件（AGENTS.md）注入**：工程根放一份 `AGENTS.md` → 下一回合日志出现 `[instructions] loaded <workDir>/AGENTS.md`，请求尾部注入块首段带该路径与正文；工程根没有时回落 `<config>/AGENTS.md`（日志路径即命中层级，两级互斥不叠加）；两者都没有 → 注入块与改造前逐字节一致、无 `[instructions]` 行；正文超 16000 字符 → 段标题带 `first 16000 of N characters shown`；会话中途改文件 → 下一回合签名不符即取新值（无需重启）；`task` 子代理与队友的请求**不含**该段（刻意不对称，同临时目录引导那条）
- [ ] s13/s16 新增目录名（`.mailboxes` / `.worktrees` / `.workflows`（s16 编排台账），见 `AgentConstants.h` 的 `k*DirName` 常量组）同样涉及数据兼容，改名即回归；邮箱文件名 = 收件名（预留名 lead/agent 禁被 spawn 占用）；`.workflows/` 目录首用自愈在引擎写/读入口各自内联的 `QDir().mkpath`（`reserveRunId` / `writeJsonAtomic` / `saveLastRun` / `WfJournal::open`；引擎里**没有** `ensureStoreDirs` 这个函数，宿主只推路径不 mkdir）

### 五、P1 异步链与压缩（禁回退阻塞的验证点）

- [ ] 新回合首条消息 → 记忆召回在开聊前**异步**注入（等待期 UI 不冻结、无嵌套事件循环），注入请求 payload 尾部独立消息（`<agent_context>` user，不落 history.json），system 全会话字节恒定（前缀缓存锚点）；召回飞行中 cron 交付被 `m_running` 卫兵拒投不插队
- [ ] 压缩三路径各触发一次（超限自动 / `compact` 工具 / 溢出反应式）→ 全程滚动气泡、切主题、切页均响应；**三路径都先见「压缩中」事前 live 卡**（`compact` 标签 + 档位摘要轮播），终态就地收成「已压缩」✓——事前卡与终态卡**同一张**，不是两张
- [ ] 摘要侧链失败/超时（断网或让摘要模型报错）→ 该 live 卡就地收成「压缩失败」✕，且**历史不被改写**（查 `history.json` 与会话气泡均未被摘要替换）；反例判定：卡片显「已压缩」✓ 而历史没变 = `Error:` 前缀写歪、失败被判成成功
- [ ] **本地四段轻量卡（s18.2：四张独立卡，不合并）**：制造超限会话（超大工具输出 + >60 条历史 + 占用过 T′）→ 气泡时间线出现**四张不同的卡**，标题分别为「已卸载」/「已归档」/「已微压缩」/「已裁剪」，等宽标签分别为 `compact_budget` / `compact_snip` / `compact_micro` / `compact_fit`（中性灰），头部关键参数位带条数、正文带前后计量；**不是一张汇总卡**。各段空转时**不出卡**（判据 = 本段改动计数 >0）；四段卡**无「压缩中」事前态、无 `Transcript:` 行**（纯本地同步、不产生全量转写快照）。反例判定：① 四张卡标题全是「已压缩」= `CardSink` 仍写死 `compact` 或 `ToolBlock` 标题表漏配伪名；② 只出一张卡 = 四段被合并发射；③ 每请求恒出四张 = 空转段也发了卡。红线复核：`prepareAsync` 五段顺序/阈值/`done` 语义与改动前逐字一致，历史替换语义不变
- [ ] 超大工具输出（token 估算 >0.6T′）→ 卸载落 `.task_outputs/tool-results/`，历史内仅保留约 2000 字符预览（预览仍字符域）
- [ ] 压缩完成后查盘 → 原转录落 `.transcripts/*.jsonl`；压缩后历史体量 ≤0.8×T′；派生阈值读码 CompactManager：batch=4×T′ / large=0.6×T′ / 压缩目标=0.8×T′（`T′ = contextTokenBudget() − overhead`，overhead = system + tools schema + 注入块，钳位 `[T/4, T]`；勿写死绝对值），**唯 summary 输入裁剪是独立字符域常量 `kSummaryInputCharLimit`=320000**（s17.9 起与窗口解耦、不随声明窗口缩放，内容级启发不随迁；旧写法 `1.6S` 已废，要放宽只改这一个常量）；snip 迟滞 60/50（>60 条**且**过 token 闸门才归档、归档后总量 ≈50 条，10 条迟滞带内不重复 snip）、归档只追加落固定名 `.transcripts/snip_archive.jsonl`（非全量重写）、会话内归档标记为恒定文本（无条数/无路径，缓存友好）
- [ ] 人为制造上下文超限错误 → 服务端 4xx 响应体原文并入 error 文本（错误气泡可见 `context_length_exceeded` 等，不再恒为 Unknown error）；溢出关键词表单源 `isContextOverflowError`；反应式压缩自动重试恰 1 次（**s15** 起旗标为 **per-用户回合一次性**：单源复位仅在 `run()` 入口、成功收响应**不**复位——对照 lcc `hasAttemptedReactiveCompact`，同一回合压缩只试一次不复活预算）后重发
- [ ] 人为制造 429 / 5xx → 指数退避重试（1s,2s,4s…，次数上限 = `config.json` 键 `maxRetries`，默认 2、校验界 0~5，越界/非整数回退默认）；重试耗尽的错误文本含服务端响应体解析结果（`error.message [+ code]`，非 JSON 体截 400 字节），不再恒为 Unknown error；重试从头清空累积状态（buffer/thinking/content/toolCalls/usage）；**s15**：idle 超时（60s 无数据）同走本退避链——转可重试消耗同一预算、重试前补清 pendingFrame/finishReason，耗尽才终态且终态文案逐字节不变；30min 总超时仍直接终态=偏差登记）
- [ ] 4xx（除 429）仍硬错误不重试 → `context_length_exceeded` 反应式压缩路径不受重试改动影响；可重试判定位于 4xx 分支**之前**（顺序颠倒会让 429 落进硬错误、退避链路成死代码，回归点）
- [ ] （可选——依赖真实服务端，代码级已测：`tst_agentrecovery` 判据矩阵 + QOpenAi 传输层编译期等价）429/限流文本与 529/过载文本（HTTP 529 或 `AgentRecovery.h` isOverloadedText）走退避；连续 529 达 `kMaxConsecutive529`=2 且配置 `fallbackModelId` → 换请求体 model 重发（每流一次、回合内粘滞 m_recoveryCurrentModel）；未配置只退避
- [ ] 会话自然结束后 → 记忆沉淀 fire-and-forget 挂回合尾巴，不阻塞下一回合输入

### 六、P1 主题与导航样式

- [ ] 设置页轮换三主题 light / dark / atomOneDark → 全窗重绘、无旧主题残色
- [ ] 底色基准取色 → **内容页/标题栏透色区**：light `rgb(249,249,249)`、dark `rgb(40,40,40)`、atomOneDark `rgb(40,44,52)`；**导航列与会话侧栏**自 s17.7 起三主题一律取 FluentUI 原生导航带色（`3rdparty/FluentUI/stylesheet/<theme>/FluVNavigationView.qss:3`）：light `rgb(243,243,243)`、dark `rgb(32,32,32)`、atomOneDark `rgb(33,37,43)` ⇒ 导航带恒比内容页**暗一档**，靠色差分层而非线做分界
- [ ] 导航顶边 border-top → **三主题自 s17.7 起一律不再画这条线**（原 dark `70,70,70` / atomOneDark `72,78,88` 与 light 的 `210,210,210` 同批移除），分界交给上一条的色阶。描边基准 = 各主题 FluentUI 原生控件描边色：light `rgb(229,229,229)`、dark `rgb(69,69,69)`、atomOneDark `rgb(71,74,79)`，卡片描边 / 侧栏顶边 / `#jumpBottomBtn` / `SendMsgButton` 四处同源。**dark 刻意不取示例页卡片边框 `55,55,55`**——它与 `#jumpBottomBtn:hover` 的合成实色（40+15=55）同值，会让描边在 hover 时消失；`69,69,69` 与旧值 `70,70,70` 仅差 1，观感不变而保住四档层次 40/55/65/69
- [ ] 内容页圆角 → **左上角 8px、其余角直角**（三主题一致，s17.7 用户裁决；8px 取 FluentUI 卡片规格 `FluVCard/FluHCard: border-radius: 8px`）。注意缺口透出的是父级窗口本体色（light 249 / dark 40 / aod 40,44,52），不是导航带色
- [ ] 滚动条槽色随主题（`LiteHarnessScrollBarAlign.qss` 经 `qproperty-trunkBackgroundColor`）→ 三主题 trunk 一律归一到**导航带色**（light 243 / dark 32,32,32 / aod 33,37,43）：FluentUI 原生槽分别是 249 / 44,44,44 / 40,44,52，都比新导航带亮，不覆盖会在导航带里露出一条亮槽
- [ ] 标题栏底色 → StandardTitleBar 为 paintEvent 手绘不吃 QSS，靠置背景透明透出窗口本体色；light 前景黑、其余白，高度 32px
- [ ] 覆盖机制读码警示 → 覆盖 FluentUI 取色只准 `appendOwnSheetOverride` 往**控件自身样式表**幂等追加（marker `/*lh-nav-align*/` 截旧防增长）；若出现窗口级复合选择器写法即回归（像素实证无效）；`ThemeAware::bind` extraRefresh 的「首刷同步 + `singleShot(0)` 重放」双保险仍在（抵消 FluentUI 批处理重写）
- [ ] 会话侧栏 → 标题/模型/工作目录/上下文占用条/状态灯/待办清单/团队名册/页脚版本随回合实时刷新（全部由 `ChatSessionPage` 调 setter 推入，`SessionSidebar` 自身不订阅 `AgentLoop`）；「上下文」节是**条 + 条下数字行**（行内 K/M 紧凑读数、tooltip 千分位精确，同一条 `UsageTokens::formatCompact`）（数字原在输入框下方的 `ChatStatusStrip`，按用户裁决迁入面板；收起侧栏时条与数字一并不可见，这是明示取舍），「状态」三态灯整盏留在侧栏（s16.1 曾下移、随后按用户要求搬回）；收起后右上角**浮动展开钮**出现且随窗口 resize 对位（页面直接子件、手动几何 + `raise()`），展开钮与侧栏互斥不同时可见；显隐偏好落 config.json 键 `sidebarVisible`，重启后按上次状态恢复首帧（读值放在布局收尾，令首帧即按最终态钳宽）；三主题下 `SessionSidebar.qss` 与 `ChatStatusStrip.qss` 各自生效、无残色
- [ ] s17 P1 侧栏观测面三节 → 「当前目标」在 goal active 时显形于状态灯下方（条件行 + 拦阻 x/cap 计数 + 轮次/消耗汇总；goal 终局或 `/goal clear` 后整节消失）；`run_in_background` 起账即现「后台任务」节（●运行中/✓已完成/✗失败，收割离账后清空隐藏）；`schedule_cron` 登记即现「定时任务」节（到期待交付行 amber 高亮，cancel 即离账隐藏）；三节对单轮问答会话全程不可见（空态整节隐藏）；hover 行→tooltip 给全文（goal 条件/命令原文/prompt 全文+flags）；三主题下 failed 红点取色各自生效
- [ ] 状态条轮步行（`#ctxTurnStep`，s17.1 新增）→ 行形「第 N 轮 · 第 M 步」，N=**用户回合**数（`AgentLoop::userTurnCount()`，`run()` 入口自增、重启恢复按历史 `role=user` 条数播种）、M=该回合内**工具调用轮次**（`toolIterationCount()`=`m_toolIterations`，每回合归零）；**术语三处不同义必须核对**：本段「轮」=用户回合，而设置页「单轮最大调用次数」/引擎报错「工具调用轮次超过上限（%1 轮）」的「轮」=本段的「步」，侧栏 goal 节「轮次 %1」=goal 续跑计数——tooltip 首条【术语提醒】钉死三者，缺这句即回归；空账判据**与另两段不同**：全新会话（`turns<=0`）整段隐藏，而 `steps==0` 是合法读数照实显示「第 0 步」（本回合没调工具就答完），隐藏反而把「第 3 轮」读成「第 3 轮什么都没干」；到账时机不同批——本段随 `run()` 入口即显，消耗/速度等首次 usage，故首条读数常是轮步段（真实形态非故障）；步数上限由页面传入（视图不读 config.json），撞上限的报错文案与设置页数值须一致；三主题同色阶（次要信息，不与占用条抢焦点）
- [ ] 状态条速度行（`#ctxRate`）→ 本轮数字取单窗口速率、平均取 Σtokens ÷ ΣgenerationMs（**不是**各轮速率的算术平均——同一批窗口两口径差 5 倍，肉眼看不出但含义不同）；tooltip 必须写明分母是生成窗口（首个增量 → usage 末帧，不含首字延迟与工具执行时间）：分母不写，同一个数字会被读成「模型慢」而真相是「等了 8 秒才见第一个字」；空账（无有效窗口）整行隐藏——显示「0 tok/s」会被读成「模型慢」，与「还没拿到能定窗的数据」含义相反；纯 tool_calls 回合（delta 只有 tool_calls 字段，不触发 delta 信号）无增量起点 → 窗口作废、不进账；三主题下 `#ctxConsumption, #ctxRate` 同色阶（次要信息，不与占用条抢焦点）
- [ ] 输入框下方状态条（`ChatStatusStrip`）→ 与输入框**同栏宽**（作为 `m_inputSection` 末位成员由 `applyColumnWidth` 钳制，自身不设 `maximumWidth`，否则与消息列/输入框脱栏）；侧栏收起时轮步/消耗/速度三段文字仍可见（这正是它留在输入组下方的目的；占用条与占用数字按用户裁决同归侧栏，收起时一并不可见是明示代价）；**状态灯不在本组件**——三态灯单源在侧栏「状态」节，两处同时亮会在同一时刻给出两个读数；空账纪律**分段判据**——消耗/速度无回读数据即隐藏，轮步段看「开过回合没有」（`turns<=0` 隐藏）而 `steps==0` 照实显示，**三段全空时整条隐藏**（`applyEmptiness`，输入框下不留空行）；三主题下 `ChatStatusStrip.qss` 各自生效、无残色，且 `#ctxTrack`/`#ctxFill`/`#ctxNumbers`/`#status*` 只在 `SessionSidebar.qss` 定义、`#ctxTurnStep`/`#ctxConsumption`/`#ctxRate`/`#stripRow` 只在 `ChatStatusStrip.qss` 定义（同一 objectName 两处定义会打架）
- [ ] （**s17.8** 会话活动热力图）新会话页 → 53 列 × 7 行（371 天滚动窗口，恒满矩形、末格 = 今天）网格卡片显示在英雄标题下、输入栏上；数据来自跨会话合并（LiteHarness 缓存 → `NewChatPage::setActivityData` push），本页面从不直读磁盘也从不订阅 `AgentLoop`。启动一次扫全部 `<会话根>/.activity/daily.json`，回合结束增量刷新该会话（`activitySessionChanged` → 现读文件 → 合并 push），删除会话剔缓存并 push。档位五档（0/<5k/<20k/<50k/<100k/≥100k）硬编码在 `ActivityLedger::levelForTokens`；hover 单元格 → tooltip 显 prompt/completion/cached/reasoning/calls 五字段 + **覆盖边界免责**（「仅主循环与队友回合，不含压缩/记忆/workflow」，缺这句即回归）；三主题色板各自生效（light 40,167,69 / dark 52,216,113 / aod 152,195,121，档位 alpha `{0,77,140,191,230,255}`）；`ThemeAware::bind` 切主题重绘；日期键严格 ISO 口径（`2026/11/07` 类斜杠写法必拒——Qt `QDate::fromString(..., Qt::ISODate)` 宽松接受，判据必须自己拆到字符层，`tst_activityledger` 钉住）；无效 usage 快照（`prompt<=0`）不进账；日条目全零即 remove（防空日期污染视图）；**底部月份标签恰 12 个、月号互异、相邻不挤压**（`HeatmapGrid::monthLabels` 按月号去重、首列补标让位给同号 1 号锚点——371 天窗口首尾同号跨年，不去重必现两个相同月份；保留补标会与相邻月标签挤在一起，s18 两阶段修；`tst_heatmapgrid` 钉住）
- [ ] token 紧凑读数（`UsageTokens::formatCompact` 单源）→ 状态条消耗段 / 侧栏占用数字行 / goal 消耗汇总 / 热力图 tooltip 四处同档：999 原样、1000→1K、999949→999.9K、999950→1M（进位升档而非 999.9K 越界）、1e6→1M、整十位省 .0（2000→2K）、行内读数不超 8 字符；**tooltip 仍给 QLocale::c() 千分位精确值**——紧凑是读数、精确是账目，两处都换成紧凑或都换成全量都算回归（全量必被 225px 侧栏省略号截断）；0 与「无数据隐藏」的判据不变，格式化只在数字非负且有效时才发生

### 七、P2 i18n

- [ ] `config.json` 无 `language` 键 → UI 默认中文（中文为源语言，代码一律 `tr("中文")`）
- [ ] 设置页语言切「English」→ 弹「语言切换将在重启后生效。是否立即重启?」；允许 → `exit(931)` + `startDetached` 自重启，重启后**有且仅有一个**实例（main 对 rc==931 只透传，严禁二次拉起=双启缺陷回归点）
- [ ] 重启确认框点取消 → 不重启（close 复用退出守卫）；语言已落 `config.json`，下次启动生效
- [ ] 切换后常驻组件重译 → 导航三项 / 设置页 / 新对话页 / WorkDirPathBar / TodoCard 标题取新语言（`changeEvent(LanguageChange)`）；已渲染历史气泡滞留旧语言（已知接受偏差，勿判 bug）
- [ ] Qt 标准对话框（仅剩 QFileDialog 按钮）→ 恒为英文，qtbase 中文 qm 已按裁决摘除，属已知取舍勿误报漏译；确认框已全量 FluentUI 化（`FluMessageBox` 的「确定/取消」中文由 FluentUI 自带 `Controls.zh-CN.qm` 提供，不走 qtbase）
- [ ] 新增/修改 UI 串流程 → 仓库根跑 `scripts/update-i18n.ps1` 成对刷 `i18n/*.ts` 双文件后构建通过；**严禁**跑 `lite-harness_lupdate` 陷阱 target（会扫 FluentUI 灌入上千外部串）；发往 LLM 的 C 类串（system prompt、`(恢复：工具结果不可用)` 等落盘文本）保持 `QStringLiteral` 不被包 `tr()`
- [ ] 版本号单源 → 设置页「关于」版本 = CMake `project VERSION`（18.2）经 `LITE_VERSION` 宏拼 `s` 前缀运行时注入（显示 `s18.2`；s18 起主段为自有发布阶段、s18.1 起带第二段补丁位，lcc 参照阶段到 `s17` 为止；补丁位先例 `s13.1`/`s16.1`/`s17.1`/`s17.2` 与 `s17.4` 跳过裁决见 AGENTS），全仓无散落硬编码；侧栏页脚 `lite-harness s18.2` 不带多余 `v`
- [ ] **s17.12 自更新（发布包内验，开发态不适用）**：① 开发态（`build/bin` 旁无 `Qt6Core.dll`）进设置页 → 状态行须显示「当前是开发态构建…自更新已禁用」，**不得**与「已是最新」共用文案，且不得发出任何网络请求；② 发布包内首次进设置页 → 自动校准一次（`showEvent` 触发，反复切页不重复打 API）；③ 有新版本 → 「下载并更新」出现，下载期**说明位给百分比数字、其左侧进度环同步推进**（分母未知时环走不定态空转、数字退成 MB，**绝不允许出现一个假的 100% 或空环**；环的 track 必须可见，0% 时要看得出「还剩多少」），校验/解包/换体期环收起且**无限动画已停**（只隐藏不停动画会白烧 CPU、下次显示相位随机），校验期显示「正在校验 SHA256」；④ 篡改包或截断下载 → 须报「校验和不匹配，已丢弃，未触碰安装目录」，安装树逐文件不变；⑤ 换体成功后确认框默认键须钉在「取消」（与退出守卫同款数据丢失防线），确认后新实例起来且旧 `*.update-backup` 由新实例在下次启动清掉；⑥ 换体中途失败 → 须自动回滚到原版本可正常启动（不是半新半死）；⑦ 重启后设置页版本数字须变为新版本，日志有 `[update] swapped to …` 与 `[update] cleaned N residue files …`

### 八、P2 记忆与子代理

- [ ] 会话自然结束后查 `.memory/` → MEMORY.md 索引 + slug.md 记录新增；强续跑 / 撞调用上限分支**不**触发沉淀；仅 persistent scope 记录且过三重去重
- [ ] 召回 LLM 选择失败 → 关键词打分兜底，兜底记录仍出现在 payload 尾部注入块（不落 history.json，勿回退为注入 system）
- [ ] 记忆整理阈值触发 → 卡片「已整理记忆：%1 → %2 条」；重写中途失败 → 快照回滚原记录完好（无半成品态）；库过大 skip/skipped 降级不崩
- [ ] `task` 子代理 → 全新上下文（不带主会话历史）、黑盒只回最终文本（中间过程不涌入主流气泡）
- [ ] 子代理轮次预算与主循环同源（`start()` 入口快照 `AgentConst::maxToolIterationsValue()` 进 `m_maxTurns`，原固定 `kMaxSubagentTurns = 50` 已删）→ 改「单轮最大调用次数」联动子代理，但单次运行中不随设置变动；预算耗尽以停跑文案收尾
- [ ] 子代理工具白名单 → 仅 read/write/edit/glob + bash 异步；todo_write / task / cron 族等主循环专属工具不可见；其权限询问经同一 permissionRequired 透明转发宿主（同时至多一方待决）
- [ ] （**s18 SubAgent 上下文压缩**）长任务子代理（多轮工具、大输出）→ 每请求主动成组裁剪（目标 4/5×上下文上限，`contextTokenBudget()` 现取；system[0]+任务 prompt[1] 恒保留；assistant(tool_calls) 连同其后连续 tool 结果整组删，不得留孤立 tool 致上游 400）；人为制造子代理上下文溢出错误 → 反应式压缩重试**恰一次**（目标 1/2×上限，旗标 `m_reactiveCompactUsed` 成功收响应复位；同回合再溢出直接以 `Error: subagent API call failed: …` 收尾，不无限重试）；内核判据由 `tst_contexttrim` 48 条直钉，本项验接线（触发时机/复位纪律/keepHead=2）

### 九、P2 后台任务与 cron

- [ ] bash 带 `run_in_background: true`（**严格布尔**，字符串 "true"/1 等不算）→ 台账 `bg_0001` 起自增、立即返回不阻塞回合；完成后结果以 `<task_notification>…</task_notification>` 注入（**s15**：`<summary>` 取前 200 字符、无截断标记——lcc §11-3 坑沿不修=登记）；**s15** PostToolUse 钩子于进程**完成时**以真实终态触发（QProcess::finished），派发时不触发；FailedToStart 不触发钩子=偏差登记
- [ ] （**s15** 第 4 事件源）空闲且有已完成后台任务（`hasPending()` 只查不清）→ ≤1s tick 自动开**空 prompt 回合**收割（UI 无空用户气泡、历史不 append 空 user、标题不误锁「新会话」；收割唯一在 `run()` 圈顶 `injectBackgroundResults`；优先序 team→cron→bg 映射 lcc inbox→cron→bg→stdin，1s 节拍沿用=偏差登记）
- [ ] 占位输出逐字 `[Background task %1 started] Result will arrive as a task_notification.`、起进程失败输出 `Error: Failed to start background task: …`；两者均经 `isToolFailure` 判红标（**s15**：旧 `[Background task start error]` 判据已删，`Error:` 前缀天然覆盖）
- [ ] cron 5 段表达式校验 → 越界段拒绝（分 0-59 / 时 0-23 / 日 1-31 / 月 1-12 / 周 0-6）；支持 `*`、`*/N`、`a,b,c`、`a-b`；逗号列表全不中判 false（勿回退成默认命中）；日与周双限定为 OR（Vixie 语义）
- [ ] schedule_cron 持久 → `<会话数据根>/scheduled_tasks.json` 原子写；重启后 pending 一次性任务照常重投（装载不剪枝）；id 形如 `cron_<hex8>`
- [ ] at-least-once 两段投递：回合成功 → recurring 清 pending 留表、one-shot 移除；回合失败/停止 → 回队，下一 1s tick 立即重试（不清 lastFired、不等整分钟）
- [ ] 同一分钟去重 → pollDueJobs 同分钟 marker 命中不重复触发；list_crons 输出每行 `%1: %2 -> %3 [%4, %5]`（prompt 截前 60 字符，空表回 `No cron jobs.`）
- [ ] 轮询读码 → 1s QTimer 由宿主 AgentLoop 驱动（零线程）；`stop()` 不杀 cron：调度器存续、交付暂停于 `m_running` 卫兵、空闲后续投；已知偏差：多会话页同 workDir 各持台账可能双触发（登记勿修）

### 十、P2 打包部署与 CI

- [ ] Release 全量构建 `cmake --build build --config Release` → 0 error（FluentUI 首编 >15 分钟，设足超时）
- [ ] 打包 `cpack --config build/CPackConfig.cmake -B build` → 出 `build/lite-harness-s<版本>-win64.zip`（约 54MB；**本地包与 CI 包条目数不同，核对时分清两者**：本地 cpack 实测 74 条目 = 10 目录 + 64 文件（bin 19 / plugins 24 / translations 31），56,361,719 字节；CI 发布资产 69 条目 = 10 目录 + 59 文件（bin 18 / plugins 11 / translations 30），55,858,237 字节——差异源于构建环境，非打包缺陷；自更新下载与校验走的是 **CI 那份**，其 API `size` 与 `digest` 均已本机实测吻合）；`--config` 必带，否则报 generator not specified；cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）
- [ ] 包内布局 → 顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + `qt.conf`（Prefix=..），根级 `plugins/` + `translations/`；无 FluentUI 泄漏物（`bin/Gallery.exe`、`include/`、`lib/`、`share/`、重复 Qt 运行时）——根级 `install(CODE)` 清理必跑在子目录规则之后
- [ ] 解压到干净机器冒烟 → `<顶层目录>/bin/lite-harness.exe` 可启动；配好 `config.json` 的 `apiBaseUrl` / `apiToken` 后可完整对话；本链默认携带 VC 运行库（windeployqt 未传 `--no-compiler-runtime`），故缺 Redist 起不来不再是预期边界
- [ ] push / PR → GitHub Actions `Windows-Qt6.9.0.yml` 触发干净环境全量 **Release** 构建 + `ctest` 单测 + cpack 打包，绿灯即验收；paths 白名单外（如本 docs 改动）不触发；`tests/**` 已在白名单内（改测试也触发验收）

- [ ] tag `v*` / `s*` 推送 → 同一 workflow 把 zip 上传为该 tag 的 GitHub Release 资产；**Release 标题与简介的单源 = 附注 tag 正文**（`git for-each-ref --format='%(contents)'` 读出后喂 `body`，标题统一 `lite-harness <tag>`）；轻量 tag 会被「Read tag annotation as release notes」步骤显式 throw、不产出空简介 Release；改存量已发布 tag 的简介只走 GitHub 网页/API，**勿重跑其 workflow**（会用短附注覆盖富正文）
- [ ] CI 零凭证读码 → workflow 内无任何 token/secret 硬编码或引用
- [ ] 旧就地 `deploy` 目标（windeployqt 自定义目标）已摘除，**勿恢复双轨**：CPack ZIP 是唯一部署/打包路径，不要再验 `--target deploy` 或往 `dist/` 拷产物

### 十一、Agent 团队（s13 移植：P0 唤醒收割 + P2 协议抽查）

- [ ] **P0** `spawn_teammate` 后 Lead **结束当前回合**（空闲）→ 队友投给 lead 邮箱的 `plan_approval_request` / `result` 等由 1s cron tick 的 `tryDeliverTeamEvents` **自动收割**并开新回合（Gate③ MAJOR-1 回归点：曾永沉 `.mailboxes/lead.jsonl` 团队死锁直至人类再发言）；同 tick 邮箱交付先于 cron（lcc 优先级口径）；Lead 忙时不插队（`m_running` 卫兵）；空批不开回合（MessageBus M8 unlink 失败=本 tick 跳过、下一 tick 重试）
- [ ] **P2** `request_plan`→`review_plan` 审批翻转：门只随队友侧 `submit_plan`/`applyPlanResponse` 翻转（Lead `runReviewPlan` 不触 planGates）；批准前队友 bash/write_file/edit_file 被拦、read/glob 恒放行；裁决以 `plan_approval_response` 投回队友邮箱
- [ ] **P2** `create_worktree` 十门逐门回判（任务非 pending / 已被他树绑定 / 名占用 / 分支已存在 / 路径已存在 / 非仓库根 / 注册表不可读…失败一律折叠 `Error:` / `Partial` 文本交还模型）；Lead claim 绑定任务后 `leadToolCwd` 取 worktree cwd——bash/read/write/edit/glob **围栏根随租约 cwd**；`removeWorktree` 刻意非工具（ToolNames.h 不可见，防模型误删）；分支 `wt/<name>` 永不随 remove 删除
- [ ] **P2** Lead 工具面恰 27 件闭环（静态核对 `createToolsDefinition` / handler 表 / toolSummary / ToolBlock 标题四处 +7；**s16** 另 +1 编排入口 workflow——Lead 独占、executeTool 特判不入 handler 表；ToolTagKind 零新增类别）；`submit_plan` 只出现在队友 schema（`teammateToolsDefinition` 共 10 件）；团队纪律静态段字节恒定 `QStringLiteral` 禁翻（`.ts` 不得出现 schema 英文描述串——P4 Lane C 实测 0 泄漏）
- [ ] **P2** **s17.2 临时目录引导的覆盖面**：主循环 system prompt 有 `Write temporary/test/scratch files under <会话根>/.temp`（`makeSystemPrompt` 的 head+tempDir+tail 拼接，目录名单源 `AgentConst::kTempDirName`）；子代理 `subSystemPrompt` 与队友 `TeammateRuntime::systemPrompt` **刻意不含**此句（保持与 lcc 文本对照、不动禁翻区既有串），改由 `task` / `spawn_teammate` 的 `prompt` 参数**属性级 description** 指示宿主把自己那条绝对路径转述下去（自指式、不嵌具体路径——工具表是 `static const QJsonArray cached` 禁运行期插值）；核对这两处 description 存在且为 `QStringLiteral` 禁翻，`.ts` 不得出现其英文串
- [ ] **P2** 队友回合环：`turnRequested`→宿主 deferred 发 ChatStream→`deliverTurnResult` 回填（零嵌套事件循环）；权限询问经同一 `permissionRequired` 透明转发；队友工具失败折叠为文本回喂不抛异常
- [ ] **P2** 重启即账销（D7）：五本账纯内存——重启后磁盘残留 in_progress 任务经 `releaseTeammateAssignment` 降级 pending、清空 owner，不自动复活队友
- [ ] **P2** 关停与退出清算：`request_shutdown` 握手→队友 `finished`→宿主收口；会话关闭/`~AgentLoop` 经 `settleTeamOnExit`（不留活 QTimer；**析构期禁 emit**——页面成员此刻已析构，到方槽即 UB，见 AGENTS.md「AgentLoopTeam.cpp」条），运行中关窗守卫照常先 `stop()`

---

<a id="ui"></a>
## 第四部分 · 交互 UI 设计逻辑（折叠式渐进披露）

### 1. 用户发送文本后

```
用户输入 → 提交 → Agent 开始处理（流内即时显示「处理中…」占位，发送钮切为停止形态）
```

- 输入框提交后，对话流中新增一轮交互
- 用户消息以**右对齐气泡**固定在对话流中（`MessageBubbleWidget` 按 role 分流：User 内容右对齐、
  Assistant 占满整行左对齐）；助手侧的一切进展都挂在**同一个气泡的时间线**上，而非另起浮层
- Agent 响应区域开始流式渲染；首个内容/思考/工具事件到达时占位即清除

### 2. 思考（Thinking）— 可折叠

```
[▶ 思考过程]              ← 终态默认折叠，节省视觉空间
─────────────────────────
| 内部推理、分析、决策链    |  ← 展开后可见
─────────────────────────
```

- **终态默认折叠**：思考过程对多数用户是噪音，流式收尾后自动折回一行
- **标题带时长**：进行中标题逐秒显示本轮已思考多久（`思考中 · 12 秒`，满 60 秒进 `1 分 5 秒`，不足 1 秒只显 `思考中`），收尾后定格为总耗时（`思考了 1 分 5 秒`）。计时复用圆点轮播的 400ms 定时器（`CollapsibleBlock::liveElapsedSeconds`，起表点在 `startLiveTimer`），不另起 1s 定时器；时长只进标题不进正文，故不触发正文测量/高度动画/自动滚动
- **进行态保持折叠 + 顶部预览行**：思考期间块不展开，头部显示「当前输出行」的实时片段（400ms 随圆点轮播刷新，取末尾窗口最后一个换行之后的部分，按可用宽度 ElideRight）—— 既是「正在思考」的进行时指示，也避免块张开后被读成「一直在输出」。收尾后预览清空、标题定格为总耗时
- **进行态不允许展开**：思考期间点头部无反应（只看预览行），结束后才可展开看全文 —— 避免用户在半成品上做展开/折叠决策，也省掉流式期间的内容测量（折叠态 `measureContent` 直接返回）
- **可展开**：终态后用户主动点开，按完整限高（150px）查看内部推理
- 设计意图：**按需透明**，不强迫用户看也不完全隐藏

### 3. 工具执行（Tool Calls）— 可折叠卡片

```
[🔧 Read  README.md                    ✓]  ← 折叠态：工具名 + 参数摘要 + 状态字形
──────────────────────────────────────────
| 输出结果:                              |
|   1 | # Title                         |
|   2 | content...                      |
──────────────────────────────────────────
[▼ 收起]
```

设计逻辑：

| 层级 | 展示内容 | 目的 |
|------|---------|------|
| **折叠态** | 工具名 + 关键参数 + 状态字形（✓ 成功 / ✕ 失败 / ■ 已停止） | 让用户知道"做了什么、成没成"，不占空间 |
| **展开态** | 完整输出（输入参数以头部摘要与悬停 tooltip 呈现） | 让用户审查"结果是什么" |
| **进行中态** | 事前折叠卡：工具名 + 类别色标签 + 圆点轮播「执行中」 | 反馈"正在执行"，减少不确定感；收口时就地转为终态卡 |

### 4. 整体编排逻辑

```
用户消息
  │
  ├─ [▶ Thinking]              ← 折叠组 1：思考
  │
  ├─ [🔧 Tool Call 1 ✓]        ← 折叠组 2：工具调用（可多个）
  ├─ [🔧 Tool Call 2 ✕]
  ├─ [🔧 Tool Call 3 ●执行中]   ← 进行中（串行队列当前项）
  │
  └─ Agent 最终回复              ← 始终展开，流式渲染
```

### 5. 核心设计原则

| 原则 | 实现方式 |
|------|---------|
| **渐进式披露** | 默认只显示摘要，细节按需展开 |
| **状态可感知** | 每个工具调用有明确的进行中/成功/失败状态 |
| **信噪比控制** | 思考过程和工具细节默认折叠，最终回复始终可见 |
| **串行队列可视化** | 工具调用按串行队列逐个执行（全仓零线程架构约束）：执行中的工具显示事前 live 卡，已完成的卡各自保留终态，同屏可见进度轨迹 |
| **流式反馈** | 文本回复逐字渲染，减少等待焦虑 |
| **可审查性** | 所有折叠内容都可展开，用户保有完全知情权 |

### 6. 关键交互模式总结

- **折叠 ≠ 隐藏**：信息存在但收纳，用户随时可展开审查
- **多个独立折叠组**：每个工具调用是独立的可折叠单元，互不影响
- **状态驱动 UI**：pending → running → success/error，每个状态有对应的视觉表现
- **层级清晰**：思考（内部）→ 工具（过程）→ 回复（结果），三层分离

### 7. 权限审批卡（PermissionCard）— 内联、拒绝优先

```
[需要权限确认]                        ← 标题行
[bash] 请求执行：rm -f build/...      ← 工具名标签 + 动作词条 + 参数摘要
[风险原因：疑似破坏性命令]  [拒绝] [允许]   ← 拒绝在左且持默认焦点
──────────────────────────────────
[✓ 已允许 bash 请求执行：rm -f …]      ← 裁决后主体收起，只留一行留痕
```

- **安全默认**：拒绝按钮持默认焦点（回车即拒绝）。焦点设置延到 `QTimer::singleShot(0)`——卡片刚构造时尚未入布局可见，此刻 `setFocus()` 会失效。
- **裁决即收起**：`resolve()` 置 `m_resolved` 幂等门闩 → 两按钮禁用 → 发 `userResolved(bool)` → 主体隐藏、改显单行留痕（`✓ 已允许` / `✕ 已拒绝`，图标 `outcome` 动态属性喂 QSS 着色）。留痕行按当前块宽**中部省略**，tooltip 保全文。
- **外部收口不发信号**：用户 stop 导致后端自动拒绝时走同一留痕形态（`applyTrace(false)`），但**不**再发 `userResolved`——否则会二次驱动后端。
- **理由文案 EN→ZH 映射单源**：后端回传的英文 reason 经 `PermissionCard.cpp` 的映射表转中文（「Writing outside workspace」→「正在尝试访问工作区之外的路径」、「Potentially destructive command」→「疑似破坏性命令」）。**新增 reason 必须同步该表**，否则用户看到裸英文。
- **挂载位置**：有当前气泡则 `appendPermissionCard` 嵌进时间线；无气泡（回合外到达）则挂滚动区主布局兜底。子代理的权限询问经 `SubAgent::permissionRequired` 直连转发为宿主同一信号，故审批体验与主循环完全一致（同时至多一方待决，串行队列天然保证）。
- **可整盏关掉（s17.2）**：设置页「权限」→「工具执行权限审批」选「无需审批」，落 config.json 键 `permissionApproval`=false（单点取值 `AgentConst::permissionApprovalEnabled()`）。关闭后**本卡永不出场**——三道询问门（ASK 规则 / `mcp__` confirm / workflow confirm）在 `AgentLoopHooks.cpp` 的 permission 钩子里被整体跳过，异步回合的 `asyncApprovalDenied()` 也随之不可达（不需要审批就不存在「无人可问」）；侧栏「等待审批」灯同因无信号而恒不亮。**关的是「问不问」不是「许不许」**：bash 硬拒绝黑名单、workflow 的 deny 定值串、文件工具 `safePathIn` 越界拦截都在开关之外照常生效。生效时机=下一个工具调用（钩子每次现取，经 `AppSettings::value()` 读缓存零构造），与 `contextCharLimit` 同口径，不同于 `maxToolIterations` 的回合入口快照。

### 8. 待办卡（TodoCard）— 时点快照，无进行态

- 头部 `[任务清单] …… [done/total] [▼]`，与 ToolBlock 同款实色底与圆角；计数 tooltip「已完成 %1/%2」。
- 行状态用**文本字形**而非图标：`●` 进行中 / `✓` 完成 / `○` 待办，颜色由 QSS 行状态选择器控制。
- **快照语义**：`todo_write` 无状态，每次成功即以本次全量清单发 `todoUpdated`，UI 侧新建一张卡而非原地改表——历史里因此留下「计划演进」的每个时点，而非只有最终态。
- **回放重建同契约**（s17 缺陷修复）：回会话时 `replayHistory` 对成功的 `todo_write` 按同一口径重放——校验归一走 `AgentLoop::normalizeTodoSnapshot` 单源、快照卡照嵌、侧栏「任务清单」逐事件推入并停在末态；实时"延一拍嵌卡"是为让位自家工具块落位，回放中工具块同步落位在前，直接追加不再延帧。
- 它是 `CollapsibleBlock` 家族里**唯一没有进行态**的子类：`liveText()` 落到基类默认空串，轮播定时器对它不启动。
- 标题「任务清单」属常驻文案，`changeEvent(LanguageChange)` 里重取 `tr()`（i18n 第八轮）。

### 9. 会话侧栏（SessionSidebar）与输入框下方状态条（ChatStatusStrip）

- 与气泡时间线**互补**：气泡流是"发生过什么"（会滚走），侧栏是"现在什么状态"（常驻）——标题 / 模型 / 工作目录 / 上下文占用条+数字行 / 状态灯 / 待办清单 / 团队名册 / 当前目标 / 后台任务 / 定时任务 / 页脚版本（后三节 s17 P1 观测面）。
- **纯视图**：不订阅 `AgentLoop`，全部由 `ChatSessionPage` 调 setter 推入（`setSessionMeta`/`setWorkDir`/`setContextUsage`/`setRunning`/`setPermissionPending`/`setTodos`/`setTeammates`/`setGoalState`/`setBackgroundTasks`/`setCronJobs`）。P1 三节走「空态整节隐藏」名册先例（无关会话零扰动）；数据口为 `AgentLoop` 1s tick 签名边沿三广播（载荷即 `BackgroundTasksManager::ledgerSnapshot` / `CronSchedulerManager::ledgerSnapshot` / `goalStateSnapshot`），页面直连侧栏 setter 不回拉引擎；「当前目标」节刻意**不含运行时长**——elapsed 进快照会让 tick 签名秒秒变、静息期也每秒重绘。
- **轮步段（`#ctxTurnStep`，s17.1 新增）读的是「进行态定位」而非成本**：行形「第 N 轮 · 第 M 步」，N=本会话开过的**用户回合**数（`AgentLoop::userTurnCount()`，`run()` 入口自增、`loadSavedHistory` 按历史 `role=user` 条数播种），M=该回合内已发起的**工具调用轮次**（`AgentLoop::toolIterationCount()`，即引擎撞上限判定用的 `m_toolIterations`，每回合归零）。
  本仓「轮」字有**三处不同义**，是本段最大的坑：状态条的「轮」=用户回合、设置页「单轮最大调用次数」与引擎报错「工具调用轮次超过上限（%1 轮）」的「轮」=工具调用轮次（=本段的「步」）、侧栏 goal 节「轮次 %1」=goal 续跑计数。故 tooltip 首条必须是【术语提醒】把三者钉死，否则用户会拿本段的「轮」去对设置页的上限数。
  两条与另两段不同的判据：① **空账看「开过回合没有」**（`turns<=0` 整段隐藏），不像消耗/速度看「有没有回读数据」——`steps==0` 是合法读数（本回合一次工具没调就直接答完）照实显示，隐藏会让「第 3 轮」孤零零被读成「第 3 轮什么都没干」；② **到账时机不同批**，本段随 `run()` 入口即到账而消耗/速度等首次 usage，故整条最先出现的常是本段，这是「引擎刚开工、账还没回来」的真实形态而非故障。步数上限由页面传入（视图不读 config.json，与「不订阅 AgentLoop」是同一半纪律的两面）；已知偏差：历史被压缩改写后播种数偏小，刻意不为该读数动 `history.json` schema。
- 「轮步 / 消耗 / 速度」的**文字读数**常驻在输入框正下方的 `ChatStatusStrip`（轮步段 s17.1 新增）——侧栏可收起，而这三段必须常驻，故随输入框走；「上下文占用」的条与数字则同归侧栏「上下文」节（数字原住状态条首段，按用户裁决迁入面板，收起侧栏时随条一并不可见=明示取舍而非疏漏）。数据仍取内核单源口径（`estimatedContextTokens()` / `sessionUsageTotals()` / `outputRate()`），UI 不另算一套，算术仍在 `UsageTokens.h` / `StreamRate.h` 纯头。占用**条**（`#ctxTrack`/`#ctxFill`）与数字行（`#ctxNumbers`）并置在同一节：条是比例画法、数字是**紧凑读数**（`UsageTokens::formatCompact` 的 K/M），精确千分位值在同节 tooltip 里，同一时刻两处必须给同一个数，所以比例算术只在 `UsageTokens::usageRatio` 实现一次、一次计算喂两个视图。
- 状态灯三态合一：运行中 / 待决权限（审批卡挂起时点亮，提示"在等你"）/ 空闲，**整盏在侧栏「状态」节**（s16.1 曾随两节下移到状态条，随后按用户要求搬回）：灯是面板的身份信息而非计量读数，搬回后状态条不再复制一份，避免同一时刻两处亮灯给出两个读数；`setRunning`/`setPermissionPending` 只写缓存态，归并规则（waiting 优先于 running）在 `applyRunState` 单点实现。
- 收起后由右上角**浮动展开钮**恢复：该钮是页面直接子件、手动几何 + `raise()`，`resizeEvent` 与 `setSidebarVisible` 都重摆位（保证出现瞬间即对位）；偏好落 config.json 键 `sidebarVisible`，读值放在布局收尾令首帧即按最终态钳宽。

### 10. 子代理进度卡 — `task` 卡的 live 形态与滑窗

- `task` 工具执行时先挂 `startTaskLive()`（标题「子代理执行中」），子代理每完成一轮经 `subagentProgress` → `appendSubagentProgress(turnNo, toolName, summary)` 追加一行。
- **滑窗丢最旧**：行数上限 `AgentConst::kSubagentProgressMaxLines = 60`，超限丢头部并在日志顶部常驻一条「…更早 %1 条进度已省略」——提示行只有一条、永不重复（整体重组写法）。
- **建卡保持默认折叠**（用户裁决——与思考块/队友卡同纪律：live 期间不自动张开，免「块张开＝一直在输出」的误读）；要看逐条进度点击头部手动展开，收口时按惯例折回（`setToolExecution` 收口路径与 `finishTaskLiveAborted()` 各折一次）。
- 折叠态也可见进展：头部关键参数位同步为最新一行（tooltip 全文，按块宽中部省略）。
- **终态仍由 `toolOutputReady("task")` 唯一收口**：进度行只是过程留痕，绝不替代结果卡；取消/停止走 `finishTaskLiveAborted()` → `applyOutcome("stopped", ■)`（灰方块＝无结果终态，与常规卡同纪律）。

### 11. 记忆卡与压缩卡 — 复用同一 `toolOutputReady` 通道

- 两者都**不新增公共信号**：`CompactManager` / `MemoryManager` 各持一个 card sink，把内部事件（压缩档位达成、记忆 stored/consolidated）经宿主注入的四参 `toolOutputReady(toolName, summary, output, ok)` 出口发出，成败仍走 `isToolFailure` 单源判定。`toolName`：记忆恒 `"memory"`；压缩侧自 s18.2 起**由引擎给出、宿主原样透传**（不再写死单一名字）——全量压缩两路恒 `"compact"`，本地四段各用 `CompactStageNames` 的 UI 伪名，见下「本地四段轻量压缩卡」条。
- 好处：UI 侧只需接一条信号即可渲染，历史里也天然与工具卡同构（回放同口径）；代价：`memory` 卡有相位丢失风险，见下。
- **记忆 live 卡**：`memoryPhaseStarted` 挂出（`appendMemoryProgress`）、`memoryChainFinished` 收尾（`finishStreaming` + 释放 `m_memoryBubble`）。
- **压缩 live 卡（s18 补进行态）**：`CompactManager::phaseSink` 在摘要侧链**挂起之前**经宿主发
  `toolStarted("compact", 档位)`，走常规工具事前卡同一套形态（`startToolLive`：等宽 `compact` 标签 +
  「压缩中」轮播 + 档位摘要），终态 `toolOutputReady("compact")` 到达时同名就地收口带 ✓/✕，stop/error
  终局由 `finishStreaming` 兜底切「已停止」——**零新增公共信号、零新增 UI 方法**（此前压缩侧无对应物：
  记忆链有 live 卡而压缩链没有，摘要秒级到分钟级期间界面完全不动）。
- **压缩失败显形（s18 补）**：摘要侧链失败/超时交付空串时，两条全量压缩续延仍发一张以 `Error: ` 开头的
  终态卡——命中 `isToolFailure` 前缀判据 → `ok=false` → `toolFailText("compact")`=「压缩失败」+✕
  （该词条自 s08 就在词表里，但 output 恒以 `Transcript:` 开头，此前恒不可达＝死文案）。
  **红线不动**：本卡纯 UI 留痕，历史仍原样交付、绝不替换（丢历史事故修复）。
- **本地四段轻量压缩卡（s18.2：四张独立卡，不合并成一张）**：`toolResultBudget` / `snipCompact` /
  `microCompact` / `fitToolResults` 各自在**真的改写了会话**时经 `CompactManager::emitLocalCard` 发一张
  终态卡，`toolName` 取 `CompactStageNames.h` 的四个 UI 伪名（`compact_budget` / `compact_snip` /
  `compact_micro` / `compact_fit`；单源纯头，发射侧与 `ToolBlock` 标题表共用，杜绝散写字面量漂移），
  标题分别为「已卸载」/「已归档」/「已微压缩」/「已裁剪」——时间线上是**四张不同的卡**。
  - **发卡判据 = 本段改动计数 > 0**：budget 只数 `replacement != output` 的真改写；snip 走到归档赋值
    才算（门槛不足 / 切点重合 / 落盘失败三条早退都不发）；micro 数两条改写路径（含落盘失败降级写错误串
    那条，`saveOutput` 已记 WARN）；fit 只数「确实变短」分支。空转段不发卡，防每请求刷屏。
  - **计量零新增估算**（B3 纪律）：前后值复用各段**已有**的增量 `total`——budget 是 batch content-only
    口径、micro/fit 是整会话口径（卡片正文分别标 `batch` 与不标，口径差异沿用各段既有登记）；snip 用
    **条数**而非 token（该段原本零 token 估算，为发卡补两次全量估算即劣化）。
  - 与全量压缩卡的三点差别都是刻意的：**无 `Transcript:` 行**（本地段不产生全量转写快照，snip 的归档
    文件是会话级固定名追加、不是本次快照，写假路径会误导用户去点不存在的文件）；**无事前 live 卡**
    （纯本地同步、毫秒级，发「压缩中」只会闪一下）；**只加完成词条、不加失败词条**（output 恒不以
    `Error: ` 开头 → `ok` 恒 true，失败词条写了就是恒不可达的死文案，同上一条教训）。
  - 标签类别落 `other` 中性灰（同 `compact` / `memory`），不新增 kind → 三主题 QSS 零改动；伪名**不入**
    `ToolNames.h`（非模型工具名，入表会破坏「常量清单 ↔ 静态 schema 27 项恰好重合」断言），
    `tst_toolnames` ⑦ 组钉住四个字面量 + 与真工具名及 `memory` 零交集 + 映射恒 `other`。
  - **红线不动**：`prepareAsync` 五段顺序 / 阈值 / `done` 交付语义**零改动**（发卡在各段函数体内，
    不在管线编排层），卡片纯 UI 留痕、不碰历史替换语义。
  - 代价登记：长期超限会话每请求最多出 4 张本地卡（各段真干活才出），这是有意的可见性取舍，不是刷屏 bug。
- **M1 相位闸**：`toolOutputReady("memory")` 到达时若 `m_memoryBubble` 已空（清屏 / 双回合交叠丢相位），**静默丢弃该卡**——记忆数据早已落盘 `.memory/`，此卡纯 UI 留痕，兜底成独立气泡反而在清空后的视图里造孤儿。

### 12. 历史回放与实时链路同口径

- 回放三处调用点与实时链路**同参传法**：`startStreaming(tr("处理中…"))` 占位 → `appendHistoryThinkingText(reasoning)` → `appendToolExecution(...)`；回放是同步填满，占位无滞留窗口。
- 工具摘要与成败判定**共用单源**：回放走 `AgentLoopDetail::toolSummary` + `AgentLoop::isToolFailure`，与实时链路一字不差——靠 `ChatSessionPage` 直接 include 内核内部头 `AgentLoopInternal.h`（`friend` 授权的有意例外，见结构篇 §9 异常 2）。
- 任务清单**同样走查**（s17 缺陷修复，曾漏）：`todo_write` 成功调用在回放时经 `normalizeTodoSnapshot` 重建快照卡与侧栏终态——曾只重放气泡时间线，回会话后侧栏「任务清单」节空到模型新回合重发为止，即本条契约的反例。
- 压缩过的会话：回放源是 `AgentLoop::replayHistoryMessages()` 而非 `messages()`——它把压缩摘要消息沿 `<会话根>/.transcripts/transcript_*.jsonl` 转写链展开成被替换掉的前序消息（规则单源 `CompactTranscript.h`），展开内容**只进展示**、不进 `m_messages`。故「压缩前对话可见」与「模型上下文仍是压缩后」同时成立；展开失败（转写缺失 / 坏行 / 路径越界 / 环）一律保留摘要消息降级，绝不比不回放更少内容。
- 因此「重启后看到的」与「当时看到的」必然一致；任何只改实时侧的渲染改动都会在回放里露馅。

### 13. 折叠骨架的三条公共纪律（CollapsibleBlock）

| 纪律 | 口径 |
|---|---|
| 头部几何 | 固定高 `kHeaderHeight = 32`，手型光标；底色由 QSS 决定，几何公式与 `setContentHeight` 共用同一常量 |
| 展开动画 | 懒建 `contentHeight` 属性动画，300ms OutCubic，`finished` 复位 `m_animating`；`setContentHeight` 内同步向上遍历父链逐帧 resize |
| live 进行态 | 400ms 一个相位的圆点轮播（0..3 循环）；**进行时长复用同一定时器**（`liveElapsedSeconds()`，起表点在 `startLiveTimer`），不另起 1s 定时器；时长只进标题不进正文，故不触发正文测量 / 高度动画 / 自动滚动 |

基类构造**禁调虚函数**，子类须在构造尾再 `bind` 主题；无进行态的子类把 `liveText()` 落到基类默认空串即可。


---

这套设计的核心思路是：**让非技术用户看到简洁的对话流，同时让技术用户能深入每一步的细节**——通过折叠/展开机制同时服务两类需求。

---

<a id="async"></a>
## 第五部分 · 异步链现状契约（memory / compact 侧链）

> 原「异步化实施规格」已全量落地，其迁移前现状表、API 草案、分阶段计划与工作量估算**均已失效并删除**
> （正文行号锚定拆分前那份 2063 行的 `AgentLoop.cpp`，100% 不可回查）。本部分按**当前代码**重写，
> 只保留仍然成立的契约；历史决策与实现偏离登记见 §7。

### 1. 全异步不变量（禁回退阻塞）

- **全仓零嵌套事件循环**：`blockingRequest` / `blockingCreate` / `BlockingSession` / `BlockingGate` /
  `setBlockingTimeout` 及同步版记忆三方法、`applyCompactPipeline` 已全部删除，grep 无残留（唯一命中是
  `SubAgent.h` 里"无嵌套 QEventLoop"的说明注释）。
- **零线程原则**不破：轮询/异步一律 `QTimer` + `QProcess` 信号，不起线程。
- **禁发送唯一来源**是会话级 `AgentLoop::runningChanged(bool)`（多会话天然隔离），不再有全局网关。
- 红线：`run()` 内的 `setRunning(true)` 必须**同步置位**、先于任何异步与任何 `run()` 卫兵；写入一律经
  `setRunning`（同点发射 `runningChanged`，同值不发射，防漏发）。消费方**不得**在
  `runningChanged(false)` 的栈内启动新回合。

### 2. `QOpenAi::AsyncRequest` 契约

```cpp
static AsyncRequest *sendText(const QJsonObject &input, QObject *parent,
                              std::function<void(const QString &content, const QString &error)> done,
                              int totalTimeoutMs = 120000);
void cancel();   // 取消后 done 永不触发，对象 deleteLater 自清理
```

- 复用 `ChatStream` 的流式组装、只提取正文 `content`：白得重试 / idle 静默超时（**s15**：idle 转可重试——预算内走 `scheduleRetry` 重发、耗尽才终态且终态文案逐字节不变——lcc 超时不重试=偏差登记） / 配置缺失延迟 error /
  析构 abort reply 四件，无需另写一套。
- **`done` 恒恰好一次**：终态唯一出口 `fireDone`，`m_done` 首到门闩 → 停表 + 取消流 → 交付 → `deleteLater`。
  已终态后再 `cancel()` 经门闩天然无操作。
- **双超时防线**：`totalTimeoutMs`（默认 120000，沿用迁移前阻塞链总时限）是总量哨兵，专杀"idle 因持续
  收字节被重置而杀不死的慢而不断流"；`<=0` 表示只依赖 idle 超时。
- 失败一律折叠为 `error` 字符串交还调用方，不抛异常。

### 3. `MemoryManager` 三条异步链

| 方法 | 契约 |
|---|---|
| `loadMemoriesAsync(conversation, ctx, done)` | 召回：LLM 从目录挑选与最近请求相关的记录，失败回落关键词打分；`done` 恒收到可用文本（可为空） |
| `extractMemoriesAsync(conversation, ctx, done)` | 沉淀：`done(stored)`，0 = 跳过/失败；`scope==persistent` 门槛 + 临时标记 + 三重去重 |
| `consolidateMemoriesAsync(ctx, done)` | 整理：**无 conversation 参数**（整理链不消费对话）；阈值未达/失败恒 0 且不起 LLM；快照-删-写段跨 LLM 等待之后仍完整执行 |

- 三者均返回 `QOpenAi::AsyncRequest *`（宿主句柄记账/取消所需），本地短路路径同步交付后返回 `nullptr`。
- `ctx` 是生命周期锚：请求 parent 到 `ctx`，`ctx` 析构即链作废、`done` 永久静默。
- 拆分方式：prompt 构建段 / 结果处理段留作私有同步方法，LLM 那一跳走 `AsyncRequest`；写文件段与替换段
  仍是同步原子段（单线程文件视图一致）。

### 4. `CompactManager` 异步变体与唯一拆分点

- 公开三入口：`prepareAsync`（五级管线，仅触发全量压缩时挂起）、`compactHistoryAsync`（批尾主动压缩）、
  `reactiveCompactAsync`（溢出反应式）。**均返回 `AsyncRequest *`**，`done` 恒一次。
- **拆分点唯一**：`summarizeHistory` → `summarizeHistoryAsync`（私有），失败交付空串；两条全量压缩
  续延对空串一律**放弃本次替换、原样交付**（不补占位、历史绝不改写）——丢历史事故修复后替换旧
  `(empty summary)` 占位语义。前四段纯本地管线不动。
  **s18 起该分支发一张 `Error:` 前缀终态卡**（`emitFailureCard`，纯 UI 留痕、不触碰历史）：
  此前"不发卡"使「压缩失败」词条恒不可达。代价登记：摘要侧链持续失败时，超限会话每个请求
  各出一张失败卡（自动路径每请求判一次门槛）——这是有意的可见性取舍，不是刷屏 bug。
- `prepareAsync` 按值收 `conversation`、`done` 按值交付最终 conversation（挂起跨 await 后调用方栈引用
  有悬挂风险）；`changed` 判定为"最终态与入参不等价"。
- 入口门槛用宿主锚定的 `conversationTokens`，段内退出判定用管线自身对 conversation 的即时
  `estimateTokens`——两口径可能微偏，入口判超而各段都判达标时**不强推** compactHistory，靠反应式 400
  兜底，防摘要空转抖动。侧链从不增量改 `m_messages`，只在既有检查点整体替换 → tool_use/tool_result
  配对协议安全。
- **压缩后的历史与「回放用的历史」是两份**：`history.json` 只存压缩后的内容（压缩即替换 +
  `persistHistory` 落盘），压缩前全量只在本次压缩的转写快照里；重启时 `AgentLoop::replayHistoryMessages()`
  经纯头 `CompactTranscript.h` 沿摘要正文的 `Full transcript:` 行把转写读回来，交给
  `ChatSessionPage::replayHistory` 平铺在摘要之前——**只读**，不写回 `m_messages`。故「压缩前对话能否看到」
  与「模型上下文」彻底解耦：前者靠回放展开，后者恒为压缩后内容。

### 5. `AgentLoop` 侧链槽位与 `stop()` 收口顺序

**两个独立句柄槽**（均 `QPointer<QObject>`、parent 到 `this` 随析构自动作废）：

- `m_sideRequest`：**召回 + 三条压缩链共用**（`m_running` 期间同一时刻至多一条前链）。取消时
  `qobject_cast<QOpenAi::AsyncRequest*>` 而非 `static_cast`——同槽异构，转型空即无请求在途。
- `m_memoryRequest`：记忆沉淀链**分槽**，`stop()` 对它既不 cancel 也不清空。理由：前链（召回）是"本回合
  还没开始"的门槛，停则回合作废；记忆链是 `finished` 之后的 fire-and-forget 尾巴，与用户停止意图无关，
  cancel 落在写段之前只会白丢沉淀成果（尽力而为语义下无补偿路径）。
- 记忆链单槽队列：`m_memoryChainActive` + `m_memoryChainPending`（链在跑则置 pending 返回，至多补一次）；
  收口时清 active/句柄 → `emit memoryChainFinished` → 若 pending 且 `!m_running` 立即起下一条。
- **队友侧三容器**（s13；非 `AsyncRequest` 槽，但同属 `stop()` 收口面）：`m_teammateStreams`（队友
  回合 LLM 流）、`m_teammateProcesses`（队友 bash 子进程）、`m_teammateRuntimes`（队友状态机）。
  本轮修复前 `stop()` 整段漏触这三者——用户按停止后队友继续跑并写任务板、投邮箱，「停止」对用户
  不成立；收口位置见上表 ⑥。

**`stop()` 收口顺序（不可交换，`AgentLoop.cpp:399-529`——此处顺序本身是契约，故保留行号锚；上一登记的 320-447 已随历轮插入失效）**：

```
卫兵 !m_running → return
① cancelSubAgent()            级联取消，合成 "(cancelled)" 配对回填
①b cancelActiveWorkflow()（s16 第六路收口）  先清槽 → requestCancel（实文只置旗，终态下一拍折叠）→
    在途 runner 请求 cancel → 半批 "(stopped)" 直写历史保配对——全路径零 emit（析构同点复用，红线与
    settleTeamOnExit 一族）；串行队列下与子代理/待决权限     互斥，紧随 ① 空转安全
①c cancelActiveGoal()（s17 第七路收口）  先清单飞旗 → 弃挂起终局 → 在途评估器请求 cancel →
    清 handle；**全路径零 emit**（析构同点复用，红线与 cancelActiveWorkflow/settleTeamOnExit 一族）——
    goal 门不产生 tool_use，无半批配对问题；无 goal 时幂等空转
② 待决权限视为 deny            直写 "Permission denied"，不经 onToolFinished（不续跑、不发展示信号）
③ 半途工具批收口              flush m_toolResultsReady → 为 m_pendingToolCalls 每条合成 "(stopped)" → 清两队列
④ m_activeProcesses 逐个 kill  其 finished 因 m_running=false 不再继续
⑤ m_currentStream cancel + disconnect + deleteLater
⑥ 队友收口（s13 本轮新增，:388-418）  m_teammateStreams 逐个 cancel → m_teammateProcesses 逐个
   kill → m_teammateRuntimes 逐个 blockSignals(true)+cancel+deleteLater（**不可即时 delete**：
   stop() 可从页面按钮/关窗守卫栈到达，未必独立于 runtime 回调栈）→ 置 m_teamWakeSuppressed=true
   （门铃抑制，否则下一个 1s tick 只要 lead 邮箱有信就同栈拉起新回合，表现为「停了又自己跑起来」；
   下一次 run() 入口复位）。UI 终局不在此 emit teammateSettled：末尾 setRunning(false) 触发页面
   兜底扫，此刻名册已清空 → 活卡一律收口为 settled
⑦ m_sideRequest cast→cancel    压缩中停止 → 历史不被替换（P3 验证点）
⑧ m_cron.finalizeInFlightDelivery(false)   在途 cron 批回队，下个空闲 tick 重投
⑨ settleLeadLease()            用户停止同属终局，Lead 租约退租（幂等；只释放不注入）
⑩ setRunning(false) → persistHistory() → emit error("已停止。")
```

- ③ 的必要性：带 `tool_calls` 的 assistant 消息已入历史，不补齐则 tool 配对断裂，`persistHistory` 会把
  坏历史落盘、下一回合收到上游 400。①②③的次序是"各自负责在途调用的收口，③ 只兜已完成未回填 +
  未开始两类队列态"。
- **停止不杀 cron 运行时**：调度器存续，仅交付暂停于 `m_running` 卫兵（在此停掉调度会让 durable 任务
  跨会话永久停摆）。

### 6. UI 侧接线

- `runningChanged(true)` → `ChatMsgEdit::setTurnBusy` 把发送钮切成停止形态并禁输入（早于 `run()` 卫兵）；
  `runningChanged(false)` → 复位忙态与审批灯。
- `memoryPhaseStarted` / `memoryChainFinished` 挂出与收尾记忆 live 卡；`toolOutputReady("memory")` 受
  M1 相位闸约束（见交互篇 §11）。
- 记忆链尾巴期间 `runningChanged` 已为 false、输入不禁——链只写 `.memory/`，不碰会话历史。

### 7. 落地状态与实现偏离登记（历史）

- 四阶段全部落地：P1 召回链 `1256d51` / P2 沉淀整理链 + `runningChanged`、`memoryChainFinished` 接线
  `9563901` / P3 压缩三处续延化 `dc73367` / P4 阻塞族清理归零 `7e83012`，审查修复 `777a18c`。
- **实现优于原草案的三处修正**（原 API 草案正文已删，此处留结论以免有人照草案回退签名）：
  ① 记忆三方法草案为 `void`，实际均返回 `AsyncRequest *`；② `consolidateMemoriesAsync` 实际无
  `conversation` 参数；③ `prepareAsync` 草案原地引用，实际按值传入、按值交付。
- **s17 goal 终局门（本轮新增收口面）**：① 评估器为一次性 `AsyncRequest` 往返，NeedEval 时**挂起终局**
  等回填（lcc 同步阻塞 evaluate 不采=核心偏差①）；单飞旗标 `m_goalEvalInFlight` + 引擎代际守卫双卫兵，
  迟到/弃置回填按默认 Allow 静默丢弃零记账；② 账本纯内存不落盘，`restore` 备好但 v1 未接线（重启不复活
  goal=当前契约）；③ achieved/failed/limit/error 终局收口走 goalNotice + triggerStopHooks 镜像（与 lcc
  四形返 None→钩子链继续 summary 照跑一致），仅 Block 短路钩子链续跑；④ req==null 兑底经
  `onEvaluatorError("Error: evaluator request failed to start")` 合流记账（宿主自拟串，"Error:" 前缀承
  isToolFailure 判据惯例）；⑤ `stopHookBlockCap` 为 GoalManager 构造期快照，改 settings 需新会话生效。
- 审查结论 0 Critical / 0 High / 1 Medium / 5 Low：M1 记忆结果卡相位闸（已落地，见交互篇 §11）；
  **L1 登记为已知限制**——双回合交叠的窄窗口里 pending 链不重发 `memoryPhaseStarted`、旧链 `finished`
  抢先定稿新气泡；修它需给信号加世代参数，收益不配成本。
- **源码注释里的「设计文档 §x.y」是历史出处标记**（约 20 余处，散在 `AgentLoop*` / `MemoryManager*` /
  `CompactManager*` / `ChatSessionPage*` / `ChatMsgEdit.h` / `QOpenAi*`）：它们指向已删除的原规格正文，
  **不可回查、也不必逐处改写**——前面的「异步化 P1/P2/P3/P4」阶段名才是有效线索。现行契约按本部分
  对应小节读：原 §2.x（现状与目标时序）→ 本部分 §1；原 §3.1（AsyncRequest）→ §2；原 §3.2（记忆三链）
  → §3；原 §3.3（压缩）→ §4；原 §3.4 / §6-7（阶段计划与取消语义）→ §5；原 §3.5a/§3.5b（UI 接线）→ §6。
