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
| [第二部分 · 模块结构总览](#architecture) | **结构与依赖边**：六层清单、AgentLoop 家族 13 个 TU 的职责地图、一条消息的完整数据流、运行时目录布局、单源纪律一览、已知分层异常、新增代码落位决策树 | 活文档（权威） | 增删 `src/` 模块、改各 TU 分工、改 `AgentLoopInternal.h` 符号归属必须同步 |
| [第三部分 · 手工冒烟回归清单](#checklist) | **历轮验收场景沉淀**：十组 P0/P1/P2 勾选项 | 活文档（发版必跑） | 每条对应一个已实现行为；实现变了要改条目，**只记现状、不记愿望** |
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
   （`s12.6`）当示例写死；升版本时随 `git grep` 逐处改，做法见 `AGENTS.md`「发布」节第一条。
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
                 PermissionCard · SessionSidebar · ChatMsgEdit · SendMsgButton
                 WorkDirPathBar · FluentInputDialog · NavItem
                        ↓
L4 Agent 编排    AgentLoop（1 类 × 13 TU）· SubAgent
                        ↓
L5 引擎/传输     QOpenAi · BashRunner · CompactManager · MemoryManager · TaskStore
                 CronSchedulerManager · BackgroundTasksManager · SessionStore
                        ↓
L6 单源与设施    AgentConstants.h · ToolNames.h · LayoutConstants.h · AppSettings.h
                 I18n · ThemeAware · ToolTagKind.h · AgentLoopInternal.h · LineEnding.h
```

**两条铁律（本仓最重要的两条边方向）**

1. **L5/L6 零 GUI 依赖**：引擎层不 include 任何 widget 头，`CompactManager`/`MemoryManager`/
   `TaskStore`/`CronSchedulerManager` 甚至不是 QObject 或不发业务信号，宿主信息一律经**构造注入
   的 sink/回调**传入（例：`AgentLoop.cpp:40-43` 把 `sessionDataRoot()` 注成四个引擎的 workDir sink
   的 `[this]{ return sessionDataRoot(); }` 惰性 lambda）。
2. **不反向依赖**：引擎绝不 include `AgentLoop.h`；页面绝不 include 引擎内部头（唯一例外见 §9）。

---

### 2. 分层清单

#### L1 应用壳与启动

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `App.cpp` | `main()`：装 translator、显示主窗、退出码 931 只透传（严禁二次拉起） | `I18n`、`LiteHarness` |
| `LiteHarness` | 主窗口（`FluFrameLessWidget`）：导航 + 堆叠装配、会话新建/恢复/重命名/删除、主题语言联动、覆盖 FluentUI 取色的 `appendOwnSheetOverride` | 全部页面、`SessionRegistry`、`SessionStore`、`QOpenAi`（配置初始化）、`NavItem`、`ThemeAware` |
| `SessionRegistry` | 导航 key ↔（页 / dataId / 导航子项）反查表 + 右键监听映射；只持 `QPointer`，零所有权 | `NavItem.h`（`NavKey` 单源）；`.cpp` 需完整 `ChatSessionPage` 类型（见 §9 异常 1） |

#### L2 页面

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `BasePage` | 导航页基类；主题契约：基类构造不首刷，派生页构造尾 `ThemeAware::bind` | `ThemeAware` |
| `NewChatPage` | 发起页（欢迎语 + 工作目录条 + 输入框），发 `newChatRequested(text)`；进入时重读 `settings.ini` 默认目录 | `BasePage`、`ChatMsgEdit`、`WorkDirPathBar`、`AppSettings`、`LayoutConstants` |
| `ChatSessionPage` | 每会话一页：布局 + **持有 `AgentLoop`** + `wireAgent()` 把全部后端信号接成 UI + 历史重放 + 侧栏接线 | `AgentLoop`、**`AgentLoopInternal.h`**（`ChatSessionPage.cpp:14`）、`MessageBubbleWidget`、`ToolBlock`、`PermissionCard`、`TodoCard`、`SessionSidebar`、`ChatMsgEdit`、`WorkDirPathBar` |
| `SettingsPage` | 主题/语言/默认工作目录/模型服务 `apiBaseUrl`+`apiToken`/模型清单 `modelOptions`（下拉选项，逗号分隔）/上下文上限/轮次上限 | `BasePage`、`QOpenAi`（直改运行时配置）、`AppSettings`、`AgentConstants`、`I18n`、`FluentInputDialog` |

#### L3 展示控件

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `MessageBubbleWidget` | 一条消息气泡：流式打字机 + 纵向时间线（思考块/工具卡/权限卡/记忆进度按到达顺序内嵌） | `ThinkingBlock`、`ToolBlock`、`LayoutConstants` |
| `CollapsibleBlock` | 折叠骨架基类：手动几何头部 + `contentHeight` 属性动画（300ms OutCubic）+ live 圆点轮播；构造禁调虚函数 | `ThemeAware` |
| `ThinkingBlock` / `ToolBlock` / `TodoCard` | 思考折叠块 / 工具执行卡（终态 + 3 种 live 形态）/ 任务清单时点快照卡 | `CollapsibleBlock`；`ToolBlock` 另用 `ToolTagKind`、`ToolNames`、`AgentConstants` |
| `PermissionCard` | 权限审批卡：待决主体 → 单行「已允许/已拒绝」留痕；发 `userResolved(bool)` | `ThemeAware`、`ToolTagKind` |
| `SessionSidebar` | 会话右栏信息面板（标题/模型/上下文占用/状态灯/任务清单）；**纯视图，不订阅 `AgentLoop`**，全部由 `ChatSessionPage` 调 setter 推入 | `ThemeAware`、`LayoutConstants` |
| `ChatMsgEdit` / `SendMsgButton` | 底部输入框（`sendMessage` / `stopRequested` / `modelChanged` + `setTurnBusy`）/ 圆形发送-停止两态钮 | `AgentConstants`、`LayoutConstants`、`ThemeAware` |
| `WorkDirPathBar` / `FluentInputDialog` / `NavItem` | 只读路径条（中间省略 + tooltip 全路径）/ Fluent 风格单行模态框 / 导航项最小派生（补 `removeChildItem`）+ `NavKey` 常量 | 仅 Qt / FluentUI |

#### L4 Agent 编排

| 模块 | 职责 | 直接依赖 |
|---|---|---|
| `AgentLoop` | 回合状态机：`run()` → 召回 → 压缩前导 → 流式请求 → 工具批串行推进 → 终局落盘与记忆沉淀；**值成员**持有并编排全部 L5 引擎 | `QOpenAi`、`CompactManager`、`MemoryManager`、`TaskStore`、`CronSchedulerManager`、`BackgroundTasksManager`、`SubAgent`、`BashRunner`、`ToolNames`、`AgentConstants` |
| `SubAgent` | `task` 子代理：独立 messages / ChatStream / 工具队列 / 权限槽的黑盒，唯一出口是完成汇总文本 | `AgentLoop`（`friend`）、`QOpenAi`、`AgentLoopInternal.h`、`BashRunner`、`ToolNames` |

#### L5 引擎 / 传输（零 GUI）

| 模块 | 职责 | 依赖 |
|---|---|---|
| `QOpenAi` | OpenAI 兼容 SSE 客户端：`ChatStream`（`thinkingDelta`/`textDelta`/`messageFinished`/`usageReceived`/`error`）+ `AsyncRequest`（一次性文本请求，`done` 恒恰好一次）+ 运行时 url/token/model 配置 + 429/5xx 指数退避重试（`maxRetries`，可重试判定先于 4xx 硬错误）。`usageReceived` 只在请求体带 `stream_options.include_usage` 时发射（末帧 `choices=[]` 且带 usage，故 usage 捕获先于 choices 检查；`finish_reason` 已到而 usage 未达时启 `kUsageGraceMs` 单次宽限） | `AppSettings`、`AgentConstants` |
| `BashRunner` | bash 执行段单源：`dangerWarning` / `truncateOutput` / `finalizeOutput` / `start`（`powershell.exe -NoProfile -NonInteractive`） | `AgentConstants` |
| `CompactManager` | 五级压缩管线 `toolResultBudget → snip → micro → fitToolResults → compactHistory` + 溢出反应式压缩；非 QObject，全靠回调注入 | `QOpenAi::AsyncRequest`、`AgentConstants` |
| `MemoryManager` | 持久记忆：召回 / 提取 / 合并三条异步链，落 `.memory/MEMORY.md` + `<slug>.md` | `QOpenAi`、`AgentConstants` |
| `TaskStore` | 任务图文件存储：一任务一 `.task/task_<hex8>.json`，每操作直读盘；6 个 `run_*` 纯文本进出 | `AgentConstants` |
| `CronSchedulerManager` | 定时任务台账：cron 校验/匹配、到期队列、`scheduled_tasks.json` 持久化、两段式 at-least-once 交付；`QTimer` 1s 轮询不建线程 | 仅 QtCore |
| `BackgroundTasksManager` | 后台 bash 任务台账：登记 / 存结果 / 渲染 `<task_notification>`，进程由宿主驱动 | `ToolNames` |
| `SessionStore` | 全局索引 `<workDir>/.lite-harness/index.json` 读写 + 条目 upsert（`QSaveFile` 原子写）；纯静态 header-only（无 `.cpp`，方法全在类内定义即隐式 inline） | 仅 QtCore |

#### L6 单源与设施

| 模块 | 单一事实源 |
|---|---|
| `AgentConstants.h` | 模型清单（settings.ini `modelOptions` 逗号分隔 + `defaultModel` 缺省项，未配置回落内置 `kBuiltinModelOptions`；读值经头内 `iniTextValue()` 兼容 QSettings 的 ini 列表语法，裸 `.toString()` 会得空串）、`kMaxTokens`、bash 超时与错误文案、输出截断、上下文上限默认/校验界、glob 上限、**中间目录名**（`.task`/`.temp`/`.transcripts`/`.memory`，`AgentConstants.h:257-262`，拼法涉数据兼容不可改）、**token 口径**（`estimateTokens` 字符→token 折算基准 `kCharsPerTokenBudget=4`、全局预算 `contextTokenBudget()`、上下文上限 `contextCharLimitValue()`） |
| `ToolNames.h` | 18 个工具名（bash/read_file/write_file/edit_file/glob/todo_write/task/load_skill/compact/create_task/update_task/list_tasks/get_task/claim_task/complete_task/schedule_cron/list_crons/cancel_cron） |
| `LayoutConstants.h` | 聊天栏宽 800 / 边距 35 / 气泡系数 0.75 / 侧栏宽 280 |
| `AppSettings.h` | 配置存储：一律 `applicationDirPath()/settings.ini`（弃用注册表），键清单见 `AppSettings.h:3-6`（`defaultWorkDir`/`sidebarVisible`/`language`/`contextCharLimit`/`maxToolIterations`/`apiBaseUrl`/`apiToken`/`modelOptions`/`defaultModel`/`maxRetries`） |
| `ToolTagKind.h` | 工具名 → 语义类别（read/search/plan/delegate/run/write/other）→ QSS `toolTagKind` 动态属性 |
| `I18n.h` | 中英文切换：源文中文 + Linguist + **重启生效**（`exit(931)` 自重启） |
| `ThemeAware.h` | 「加载 QSS + 订阅 themeChanged + 重载」样板单源 `bind(qss, widget, extraRefresh)` |
| `NavItem.h` | `NavKey` 导航/堆叠键单源 |
| `AgentLoopInternal.h` | `AgentLoopDetail` 跨 TU 内部工具**声明**单源（§4） |
| `LineEnding.h` | 文本文件行尾口径单源：`dominant`（主导行尾判定，平局偏 CRLF）/ `toLf`（匹配域归一，孤立 CR 不动）/ `apply`（写回域还原，幂等）/ `replaceOnce`（两级匹配单次替换）。`edit_file` 与 `write_file` 共用，杜绝「read_file 交还 LF 文本、edit_file 按原文字节匹配」导致的多行失配与混合行尾。`countOccurrences` 报命中次数（经 `replaceOnce` 的 `matchCount` 出参，与 `matched` 同为 nullptr 容错），供 `edit_file` 拒绝歧义编辑 |

---

### 3. AgentLoop 家族：单类多 TU 的职责地图

`AgentLoop.h` 是**唯一类声明**；13 个 `.cpp` 各承载一组成员方法定义（不是多个类）。这样既保住了
「一个状态机一个所有者」的语义，又让单文件回到可读体量。**新增方法请按 §10 的决策树落位，不要
往 `AgentLoop.cpp` 里堆。**

| TU | 行 | 负责什么 | 关键定义 |
|---|---|---|---|
| `AgentLoop.cpp` | 405 | 生命周期与会话身份：构造/析构、workDir / sessionDataId / sessionDataRoot / model、回合入口与终局、**token 计量单源**（usage 锚 + 增量外推） | `run()`、`stop()`、`sessionDataRoot()`（`:137-143`）、`adoptUsageAnchor()`、`estimatedContextTokens()` |
| `AgentLoopRequest.cpp` | 460 | 一次 LLM 请求回合：压缩前导 → 流式请求 → 工具批推进 → 记忆沉淀链 | `startChatRequest`、`doStartChatRequest`、`applyCompactPipelineAsync`、`continueWithToolResults`、`runNextTool`、`startMemoryChain` |
| `AgentLoopPrompt.cpp` | 436 | **静态** system prompt（只由 workDir / 会话根 / 静态文字决定，全会话字节恒定）+ 请求尾部 `<agent_context>` 注入块（技能目录 / 记忆索引 / 召回记录）+ 18 工具 function 定义（**禁翻区**，`QStringLiteral` 不包 `tr()`） | `makeSystemPrompt`、`rebuildSystemPromptMessage`、`makeContextInjection`、`AgentLoop::buildContextInjection`、`createToolsDefinition` |
| `AgentLoopTools.cpp` | 315 | 工具分发：`executeTool` 分流、handler 表、统一收口 `onToolFinished`、成败判定单源 | `executeTool`、`onToolFinished`、`isToolFailure`、`AgentLoopDetail` 工具段定义 |
| `AgentLoopFileTools.cpp` | 355 | 沙箱文件工具（同步本地 IO）：逃逸判定 + read/write/edit/glob，全静态，宿主与子代理各传各的 workDir；edit/write 的行尾与编码防线见 `LineEnding.h`；edit 另有三道防线（空 `old_string` 拒绝 / 体量上限 `kEditFileMaxBytes` 拒绝 / 多处命中歧义拒绝） |
 `safePathIn`、`runReadFileIn`、`runWriteFileIn`、`runEditFileIn`、`runGlobIn` |
| `AgentLoopBash.cpp` | 159 | bash 异步执行链与后台任务结果收割 | `executeBashAsync`、`injectBackgroundResults` |
| `AgentLoopPermission.cpp` | 142 | 权限门：硬拒绝黑名单、破坏性命令升级判定、ASK 规则、用户裁决续跑 | `checkDenyList`、`checkPermissionRules`、`resolvePermission`、`AgentLoopDetail::bashDenyList` 定义 |
| `AgentLoopHooks.cpp` | 163 | 生命周期钩子注册表：注册顺序即执行顺序，四个 trigger（UserPromptSubmit/PreToolUse/PostToolUse/Stop）**首个非空返回短路** | `registerBuiltinHooks`（内置：`context_inject` / `permission` / `log_before` / `log_after`）、四个 `trigger*Hooks` |
| `AgentLoopHistory.cpp` | 165 | `history.json` 落盘（含索引 `lastActiveMs` 续活）与磁盘恢复；条目登记归 `LiteHarness::createSession` | `persistHistory`、`loadSavedHistory` |
| `AgentLoopSkills.cpp` | 196 | 技能扫描与 `load_skill`；技能目录**始终跨会话共享** `<workDir>/.lite-harness/skills` | `scanSkills`、`skillsCatalog`、`runLoadSkill` |
| `AgentLoopTodo.cpp` | 119 | `todo_write`（无状态：只校验入参并渲染面板文本，成功即以本次快照发 `todoUpdated`） | `renderTodos`、`runTodoWrite` |
| `AgentLoopSubAgent.cpp` | 86 | `task` 子代理的启动与统一收口（子代理信号直连转发为主循环信号） | `startSubAgentTask`、`cancelSubAgent` |
| `AgentLoopCron.cpp` | 64 | 三个 cron handler 与空闲边界交付；台账与匹配在 `CronSchedulerManager`，本文件只做宿主侧接线 | `runScheduleCron`、`runCancelCron`、`runListCrons`、`tryDeliverCron` |

**AgentLoop 对外信号面**（`AgentLoop.h:91-138`，UI 只认这些）：`thinkingDelta`、`textDelta`、
`toolStarted`、`toolOutputReady(…, ok)`、`subagentProgress`、`permissionRequired`、`todoUpdated`、
`memoryPhaseStarted`、`memoryChainFinished`、`scheduledUserMessage`、`finished`、`runningChanged`、
`error`。另有一组**私有驱动信号**（`startChatRequest` / `doStartChatRequest` /
`continueWithToolResults` 等，`AgentLoop.h:150` 起）只用于把阻塞链拆成事件驱动续跑，不对外、
UI 不得连接。

---

### 4. AgentLoopInternal.h：内部工具声明单源

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
   `bashDenyList`），已删除，友元直接 include 本头取用（`AgentLoop.h:186-187` 记录了这次演进）。
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

② 回合入口 AgentLoop::run（AgentLoop.cpp:181）
setRunning(true)（同步红线，先于任何异步）→ 快照 m_maxToolIterations → triggerUserPromptSubmitHooks
→ user 消息入 m_messages → injectBackgroundResults() → m_memory.loadMemoriesAsync(...)
→ done 里快照注入块 m_contextInjection = buildContextInjection(readMemoryIndex(), recalled)
  （**不重建 system**——system 全会话字节恒定，规格修1）→ startChatRequest(messagesJson)

③ 请求链（AgentLoopRequest.cpp）
startChatRequest → applyCompactPipelineAsync（仅触发全量压缩时挂侧链，句柄落 m_sideRequest）
→ doStartChatRequest：组 body（model / messages / tools=createToolsDefinition() /
  enable_thinking / reasoning_effort / temperature / top_p / max_tokens /
  stream_options.include_usage=true）+ 尾部追加 <agent_context> 注入块
→ QOpenAi::chat().createStream(request, this)
   · ChatStream::thinkingDelta → 直连转发 AgentLoop::thinkingDelta
   · ChatStream::textDelta     → 直连转发 AgentLoop::textDelta
   · ChatStream::usageReceived → AgentLoop::adoptUsageAnchor（token 锚，见下「⑦ 上下文计量」）
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

⑦ 上下文计量（token 锚定，UI 占用条与压缩门槛同源）
ChatStream::usageReceived(usage) → AgentLoop::adoptUsageAnchor：prompt_tokens>0 才采纳，
  记 m_tokenAnchor=服务端真值、m_tokenAnchorCount=发送点历史条数、m_anchorInjectionTokens=
  发送点注入块估算，并清 m_historyRewrittenSinceAnchor
AgentLoop::estimatedContextTokens()：
  · 锚有效 → 锚真值 + 其后新增消息逐条 estimateTokens + 注入块变化量（当前 − 锚时）。
    锚路径**不再另计 overhead**——prompt_tokens 已含 system/tools/注入的发送点真值
  · 锚失效（首回合未回读 / 压缩改写历史 / 恢复 / 换 workDir / 换目录）→ 本地全量
    CompactManager::estimateTokens(m_messages)（含 system[0]）+ tools schema + 注入块
消费两处，同一个函数：ChatSessionPage::refreshContextUsage → SessionSidebar::setContextUsage；
AgentLoopRequest 压缩入口门槛 conversationTokens > T'（T'=contextTokenBudget()−overhead）
字符→token 折算单源 AgentConst::estimateTokens，基准 kCharsPerTokenBudget=4（见 AGENTS「数据与路径」）
```

**回填 UI 总表**（全部集中在 `ChatSessionPage::wireAgent()`，别处不接 AgentLoop 信号）

| AgentLoop 信号 | 连接行 | 落到 |
|---|---|---|
| `finished` | `:188` | 兜底 `addMessage`（无气泡时）+ 侧栏权限态复位 + `refreshContextUsage()` |
| `error` | `:205` | `dismissPendingPermission()` + `finishStreaming()` + 一条 `*Error:* %1` |
| `thinkingDelta` / `textDelta` | `:221` / `:228` | 当前气泡 `appendThinkingText` / `appendText` |
| `toolStarted` | `:265` | `appendToolStart`（事前 live 卡） |
| `toolOutputReady` | `:238` | `appendToolExecution`；无当前气泡则独立气泡兜底；`memory` 有 M1 静默闸（`!m_memoryBubble` 即丢弃）；**任何一次到达都 `singleShot(0)` 补刷一次侧栏占用条**（tool 结果此刻还压在 `m_toolResultsReady`，同栈批尾才回填历史） |
| `subagentProgress` | `:276` | `appendSubagentProgress` |
| `permissionRequired` | `:319` | `PermissionCard` + 侧栏状态灯 |
| `todoUpdated` | `:348` | `TodoCard` 快照 + 侧栏任务清单 |
| `memoryPhaseStarted` / `memoryChainFinished` | `:291` / `:306` | 记忆进度 live 卡的挂出与收尾 |
| `scheduledUserMessage` | `:378` | 以「定时任务用户消息」入历史并直接发起新一轮 |
| `runningChanged` | `:391` | 输入框忙态 `setTurnBusy` + 侧栏状态灯；终局兜底复位审批灯（`setRunning(true)` 起即禁输入，早于 `run()` 卫兵） |

---

### 6. 运行时数据目录布局

根是**会话工作目录**下的相对路径，不是用户主目录（`SessionStore::rootDirFor`，`SessionStore.h:22-25`）。

```
<workDir>/
└── .lite-harness/                        ← 全局根（SessionStore.h:25）
    ├── index.json                        ← 会话索引（SessionStore，QSaveFile 原子写）
    ├── skills/<name>/SKILL.md            ← 技能，始终跨会话共享（AgentLoopSkills.cpp:18）
    └── sessions/<dataId>/                ← 会话数据根 = AgentLoop::sessionDataRoot()
        ├── history.json                  ← 会话历史（AgentLoopHistory.cpp:35）
        ├── .memory/MEMORY.md + <slug>.md ← 记忆索引与记录（MemoryManager）
        ├── .task/task_<hex8>.json        ← 任务图，一任务一文件（TaskStore）
        ├── .transcripts/transcript_*.jsonl ← 压缩前完整转写（CompactManager）
        ├── .transcripts/snip_archive.jsonl ← snip 归档，**固定名只追加**（非全量重写；恒定标记文本，缓存友好）
        ├── .task_outputs/tool-results/   ← 超大工具输出卸载（AgentConst 名单源）
        ├── .temp/                        ← prompt 引导语指向的临时目录（AgentLoopPrompt.cpp:22-25）
        └── scheduled_tasks.json          ← cron 台账（CronSchedulerManager.cpp:554）
```

- **回退语义**：`sessionDataId` 为空时 `sessionDataRoot()` 返回 `<workDir>/.lite-harness`（
  `AgentLoop.cpp:137-143`），保证未注入 ID 的独立构造路径行为不变；空历史不落盘以免污染全局根
  （`AgentLoopHistory.cpp:22-24`）。
- 目录名一律取 `AgentConst::k*DirName`（`AgentConstants.h:257-262`），拼法涉既有数据兼容，**逐字符
  不可改**；`.lite-harness` 中间层只在 `sessionDataRoot()` 拼一次，各引擎只拼自己的叶子段。
- 用户配置在**另一处**：`<exe 目录>/settings.ini`（`AppSettings.h:19`，弃用注册表）。

---

### 7. 构建与打包（结构视角，细节见 AGENTS.md）

- 根 `CMakeLists.txt:45` 的 `file(GLOB src CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` 一次收走全部
  源文件——**新增 TU 不需要改任何 CMake 文件**（本次 AgentLoop 拆分新增 13 个文件即零 CMake 改动
  即证）。`add_subdirectory(src)` 已注释，不存在 src 级 CMakeLists。
- 第二个可执行目标 `lite-harness-tests`（`LITE_TESTS` 默认 ON，`CMakeLists.txt:116-135`）只链
  `Qt6::Core`，产物落 `build/tests/` 而非 `build/bin/`——CPack staging 与根级清理都围绕 `bin/`
  展开，测试 exe 既进不了包也不会被清理规则误删。
- 只有一个可执行目标 `lite-harness`，链 `FluentUI::Controls/Utils` + `Qt6::Network`；
  `3rdparty/FluentUI` 是子目录，`3rdparty/sqlite*` 与 `3rdparty/lcc` **不进构建图**。
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
| 工具成败判定（UI 显形与历史折叠同源） | `AgentLoop::isToolFailure`（`AgentLoopTools.cpp`） |
| bash 危险黑名单 | `AgentLoopDetail::bashDenyList`（`AgentLoopPermission.cpp`，三方共用） |
| 数据目录名 / 魔法数 / 模型清单 | `AgentConstants.h` |
| 布局尺寸（多页同列对齐） | `LayoutConstants.h` |
| 配置键与文件位置 | `AppSettings.h` |
| 工具配色类别 → QSS | `ToolTagKind.h` + `stylesheet/<theme>/*.qss` |
| 主题化样板 | `ThemeAware::bind`（禁止再复制「读 QSS + 订阅 themeChanged」三件套） |
| 版本号 | CMake `project VERSION`（数字段 12.6）→ `LITE_VERSION` 宏（拼 `s` 前缀 = 阶段 tag 名 s12.6）|
| 文本文件行尾口径（匹配域 LF / 写回域按主导行尾还原） | `LineEnding.h`（`read_file`/`write_file`/`edit_file` 共用，禁再各自裸字节匹配） |
| token 估算与预算 | `AgentConst::estimateTokens`（字符→token，基准 `kCharsPerTokenBudget`）+ `AgentConst::contextTokenBudget`（= `contextCharLimitValue() / 4`）；管线内即时估算另见 `CompactManager::estimateTokens` |
| 上下文占用（侧栏占用条与压缩门槛同源） | `AgentLoop::estimatedContextTokens()`（usage 锚 + 增量外推，无锚回落全量估算） |
| 工具名清单（18 个） | `ToolNames.h`（schema / handler 表 / 权限门 / 子代理白名单 / ToolBlock 标题 / 回归清单核对项全部引用它） |
| 导航/堆叠键 | `NavItem.h::NavKey` |

---

### 9. 已知分层异常与技术债（登记在案，勿默默踩）

1. **`SessionRegistry.cpp` 反向 include `ChatSessionPage.h`**：头文件里只前向声明，`.cpp` 需完整
   类型才 include，形成 L1→L2 的回边。可接受（无循环 include），但新增跨层引用前先看这里。
2. **`ChatSessionPage.cpp:14` 直接 include 内核非公开头 `AgentLoopInternal.h`**：靠
   `friend class ChatSessionPage`（`AgentLoop.h:146`）授权，目的是历史回放与实时链路共用同一
   `toolSummary` 口径。**这是有意为之的例外**，别据此开「UI 可以吃内核内部头」的先例。
3. **`SettingsPage` 直改全局运行时配置**（`QOpenAi::setUrl/setToken`）：设置页 → 引擎的直连边，
   生效语义见 `SettingsPage.h` 注释。
4. **仍偏大的文件**（下一批可读性优化的候选，按体量排序，实测行数）：`MemoryManager.cpp` 1116、
   `MessageBubbleWidget.cpp` 836、`QOpenAi.cpp` 767、`SettingsPage.cpp` 764、
   `CompactManager.cpp` 724、`ChatSessionPage.cpp` 717、`TaskStore.cpp` 684、
   `CronSchedulerManager.cpp` 631、`SessionSidebar.cpp` 487。
5. **自动化测试刚起步**：`tests/` + `enable_testing()`/`add_test` 已落地（`LITE_TESTS` 默认 ON，
   产物 `build/tests/lite-harness-tests.exe`，CI 跑 `ctest`），但当前只覆盖 `LineEnding.h` 一个纯函数头。
   其余改动的验证手段仍是「编译期等价 + 冒烟运行 + 移动代码逐字一致」，行为回归大面积**不可证**。
   下一批最小切口是已无 GUI 依赖的纯函数：`AgentLoopDetail::toolSummary` / `parseToolCall` /
   `BashRunner::dangerWarning` / `isToolFailure` / cron 表达式匹配 / token 估算 / frontmatter 解析——
   每个都是「加一个 `tests/tst_<模块>.cpp` + `tests/main.cpp` 里一行调用」，GLOB 收集无需改 CMakeLists。
   刻意不引 `Qt6::Test`/moc：测试对象全是纯函数，无信号槽与数据驱动表需求（取舍见 `tests/TestHarness.h`）。

---

### 10. 新增代码该落在哪（决策树）

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

---

<a id="checklist"></a>
## 第三部分 · 手工冒烟回归清单

> 沉淀历轮验收场景为可重复执行的手工回归清单。自动化测试仅覆盖 `tests/`（当前 `LineEnding.h` 行尾口径，`ctest --test-dir build -C Release --output-on-failure` 跑），无 lint；其余仍靠构建+运行+目测验收。

> **用法**：按改动面选组执行——P0 每次 src/ 改动必跑；P1 按受影响域跑；P2 里程碑/发版前抽查。
> **基线环境**：Qt 6.9.0 msvc2022_64 + MSVC 2022 + CMake，仅 Windows；模型服务凭 `settings.ini` 键 `apiBaseUrl` / `apiToken`（主窗口构造期 `QOpenAi::initFromSettings()` 读取），模型下拉取 `modelOptions` / `defaultModel`（内置首项 `qwen3.8-flash`）。
> **配置存储**：exe 同目录 `settings.ini`（`AppSettings::ini()` 单源，QSettings IniFormat）——注册表方案已随用户裁决废弃，禁再引入默认构造 `QSettings`。下文 `S` 指上下文上限字符数（settings.ini `contextCharLimit`）；`T` 指派生 token 预算（`contextTokenBudget()` ≈ S/4，触发与 UI 均此口径）；`T′` 指 T 扣除 system+tools+注入 overhead 后的会话体预算。

### 一、P0 启动与构建

- [ ] 子模块前置 `git submodule update --init 3rdparty/FluentUI` → checkout 成功，configure 不再缺 `FluentUI::Controls`（`3rdparty/lcc`/`sqlite` 从不入构建图，无需 init）
- [ ] 干净 configure `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64` → 零报错，GLOB 拾取全部 `src/*.cpp|.h` 与 `stylesheet/**/*.qss`（新增源文件重 configure 自动拾取、不改 CMakeLists.txt）
- [ ] Debug 构建 `cmake --build build --config Debug --target lite-harness` → 0 error；src/ 各编译单元 `/W4` 0 告警（FluentUI 第三方豁免）
- [ ] 重链前先结束所有运行中的 `build/bin/lite-harness.exe`（含用户自开实例）→ 链接成功；若见 LNK1168 属已知坑（Debug/Release 共用 `build/bin/` 互相覆盖），非代码缺陷
- [ ] 双击 `build/bin/lite-harness.exe` → 主窗口存活 ≥10 秒无崩溃，标题栏（32px 手绘）与导航列渲染正常
- [ ] `settings.ini` 未配 `apiBaseUrl` / `apiToken` 启动 → 窗口正常显示、构造期 qWarning 提示未配置，仅在发起对话时以回合内错误文案体现（不闪退）

### 二、P0 会话生命周期

- [ ] 新建对话页发起 → 导航「会话」下新增子项并进入对应会话页；标题为空显示「新会话」，超 12 字符截断加「...」
- [ ] 检查 `<进程当前目录>/.lite-harness/index.json` → 条目含 dataId/title/model/workDir/时间戳；QSaveFile 原子写（异常退出不留半文件）。注意：索引根固定为进程当前目录，`workDir` 只是条目字段
- [ ] 会话内发若干消息 → `<会话数据根>/.lite-harness/sessions/<dataId>/history.json` 存在且与可见消息一致（dataId 为 8 位 hex）
- [ ] 重启程序 → 会话按 createdMs 升序恢复、历史气泡回放完整；恢复不自动切换当前页（仍停在新对话页）；workDir 失效时静默回退进程当前目录
- [ ] 右键会话导航子项 → 仅弹「重命名 / 删除会话」菜单；单击子项不因基类 itemClicked 误翻页（eventFilter 吞 press/release）
- [ ] 重命名：输入新名确认 → 导航与索引标题同步、刷新 lastActiveMs；取消/空/未变 → 保持原值
- [ ] 删除：弹「此操作不可撤销」确认 → 确认后页面销毁、索引条目移除、`sessions/<id>/` 整目录递归清理；删的若是当前页则切回新对话页；运行中会话先 `stop()` 再删；数据清理失败仅 qWarning、UI 状态照常收清
- [ ] 有会话运行中点窗口关闭 → 弹「任务仍在运行，退出将丢失未完成回合。确定退出吗？」且默认焦点「否」；选「否」→ 不退出、任务继续；选「是」→ 逐页 `stop()` 后退出

### 三、P0 Agent 主循环与权限门

- [ ] 工具名单核对（读码 `ToolNames.h` + 运行时注册观察）→ 恰 18 工具：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron；任务图 6 + cron 3 仅主循环注册
- [ ] bash 执行普通命令（如 `echo`）→ 不触发审批卡直接执行
- [ ] bash 触发硬拒绝表（`rm -rf /`、`sudo`、`shutdown`、`reboot`、`mkfs`、`dd if=`、`> /dev/`；大小写不敏感 contains）→ 直接回填 `Blocked: … is on the deny list`，不弹询问
- [ ] bash 触发 ASK 规则（含 `rm ` 等破坏词「Potentially destructive command」）或 write_file/edit_file 越出工作目录（「Writing outside workspace」）→ 内联 PermissionCard 出现、默认焦点「拒绝」、理由中文与 EN→ZH 映射表一致（新增 reason 必须同步该表）
- [ ] 审批卡点「拒绝」→ 历史回填 `Permission denied`、LLM 续跑下一轮；点「允许」→ 执行且同工具不再二次询问（仅跳过询问规则，硬拒不受批准影响）
- [ ] bash 运行超 120 秒命令 → 回填 `Error: Timeout (120s)`（`kBashTimeoutMs` 单源），回合继续不卡死
- [ ] 设置页「单轮最大调用次数」：合法值（如 50）落盘 → 下一回合生效（`run()` 入口快照 `maxToolIterationsValue()`，回合中途不变）；输 0 / 1001 / 非数字 → 弹「请输入 10 ~ 1000 之间的整数。」不落盘
- [ ] `settings.ini` `maxToolIterations` 缺失或非法（含手工篡改越界）→ 回退默认 500
- [ ] 回合进行中点停止 → 依序：子代理先级联取消（合成 `(cancelled)` 配对回填）、在途工具批收口（已完成批 flush + 未完 pending 每条合成 `(stopped)`）、QProcess 杀、SSE 流断、错误气泡「已停止。」；随后立刻发下一条 → 上游不报 400（tool_use/tool_result 配对完整）
- [ ] 压缩请求（prepare/批尾/反应式共用 sideRequest）进行中按停止 → 历史不被摘要替换（取消后 done 永久静默）
- [ ] 技能（`load_skill`）→ 技能目录固定 `<workDir>/.lite-harness/skills/<name>/SKILL.md`，**始终跨会话共享**（不随 `sessions/<dataId>` 隔离）；`load_skill` 按名返回全文；技能**清单**（名 + 描述）进请求尾部注入块、正文不进 system 也不进注入块（D2 裁决：清单回填、正文按需加载）；无技能 + 无记忆时不产生空注入块
- [ ] 任意工具失败（read_file 不存在的文件等）→ 仅折叠为 tool_result 字符串交还 LLM，全程不抛异常、不弹窗
- [ ] 子代理（`task`）内 bash 失败同样可判定 → 非零退出码回喂带 `Error: command exited with status N:` 前缀（与主循环前台/后台共用 `formatBashResult`）；PowerShell 起不来由 `errorOccurred` 显式收口回 `Error: bash 启动失败…`；不得把失败洗白成裸输出或 `(no output)`（曾如此）；取消后仍静默丢弃输出、`onToolFinished` 恒一次
- [ ] `edit_file` 行尾两级匹配 → CRLF 文件上用**从 `read_file` 输出抄来的 LF 多行** `old_string` 能命中（曾必然报 text not found）；写回后全文件无裸 LF（不产出混合行尾）；LF 文件保持 LF 不被转成 CRLF；单行替换与「只替换第一处」语义不退化；混合行尾文件走 LF 归一化回退、写回按主导行尾归一
- [ ] `edit_file` 编码防线 → 非 UTF-8 文件（如 GBK 源文件）返回可判定错误拒绝编辑，**不得**写回把非法字节永久替换为 U+FFFD；带 UTF-8 BOM 的文件既能正常编辑（`fromUtf8` 会吃 BOM，守卫须按去 BOM 后的正文比较，否则误拒）、写回后 BOM 原样保留（不被静默删除）
- [ ] `edit_file` 三道防线 → ① 空 `old_string` 返回可判定错误拒绝编辑（**不得**把 `new_string` 静默前插到文件开头）；② 超过 `kEditFileMaxBytes`（5MB）的文件拒绝编辑并回体量错误（**不得**整文件读入冻结主线程，也不得部分读写毁文件）；③ `old_string` 在文件中命中多处 → 返回「命中 N 处，拒绝编辑（请补充上下文使其唯一）」，**不得**静默替换第一处（有意偏离 lcc `str.replace(old,new,1)` 语义）
- [ ] 单元测试 → `ctest --test-dir build -C Release --output-on-failure` 全绿（产物 `build/tests/lite-harness-tests.exe`，需 Qt bin 在 PATH 供 `Qt6Core.dll`）；测试 exe **不进** CPack 包（输出目录与 `bin/` 分开，staging 只取 `bin/`）
- [ ] `write_file` 行尾保真 → 覆盖已存在的 CRLF 文件时沿用 CRLF
（模型给的 content 天然是 LF，直写会翻转整文件行尾、产出全文件 diff 噪声）；新建文件按 content 原样落盘不臆造行尾；主导行尾只读开头 `kEndingProbeBytes`（64KB）窗口判定，已知边界：窗口内无换行的超长单行文件判为 LF
- [ ] 上下文占用计量（token 锚定）→ 端点回 `usage.prompt_tokens` 时侧栏占用条取锚值 + 锚后增量（**锚路径不另计 overhead**，prompt_tokens 已含 system/tools/注入真值）；端点不回 usage（部分兼容端点）→ 回落本地全量估算，占用条仍单调合理；压缩改写历史后锚作废（`m_historyRewrittenSinceAnchor`）→ 下一回合重新锚定，不得沿用旧锚
- [ ] 任务图 6 工具行为冒烟（不止静态名单核对）→ `create_task` 落 `<会话根>/.task/task_<hex8>.json` 一任务一文件、`list_tasks`/`get_task` 每次直读磁盘（外部改文件即时可见）、`update_task` 加依赖用返回的 task id、`claim_task` 仅对依赖已完成的 pending 成功、`complete_task` 后状态翻转且不得重复完成他人任务；任一失败一律折叠为工具输出字符串，不抛异常

### 四、P1 设置与数据路径

- [ ] 设置页「默认工作目录」：选新目录 → `settings.ini` `defaultWorkDir` 更新；对话框取消 → 保持原值；清除 → 展示「未设置（使用进程当前目录）」
- [ ] 设置页「上下文上限（字符）」改为 300000 → 落盘即下一回合压缩管线现取生效（无需重启）；卡片展示千分位（QLocale::c() 固定，不随界面语言变），数值行带派生提示「≈N token」（= 字符上限/4，仅展示、键语义仍是字符）；侧栏占用同为 ≈token 口径（contextCharLimit/4 派生预算，非字符）
- [ ] 上下文上限输入 9999 / 5000001 / 非数字 → 「无效数值」弹窗不落盘；输入带千分位的 `200,000` → 剥逗号解析成功；合法界 10000~5000000、默认 200000
- [ ] 键位巡检 → `language` / `defaultWorkDir` / `sidebarVisible` / `contextCharLimit` / `maxToolIterations` / `maxRetries` / `apiBaseUrl` / `apiToken` / `modelOptions` / `defaultModel` 均落于 exe 同目录 `settings.ini`（手工编辑重启即生效，注册表路径不再被读写）
- [ ] 模型清单可配置：`settings.ini` 写 `modelOptions=qwen-a,qwen-b`（裸逗号串、不带引号）→ 下拉恰列两项且顺序一致；手改首项为 `qwen3.8-flash,qwen3.8-max` 读侧解析成功（`iniTextValue()` 归一化列表串，回归点：曾误判「未配置」静默回退内置清单）；清单为空/全空白 → 回退内置两项；条数超 32 截断
- [ ] `defaultModel` 指向清单内模型 → 生效；指向清单外或留空 → 回落清单首项（防配置指向不存在的模型）
- [ ] 设置页改「模型服务」端点/密钥 → 写 ini 后立即 `QOpenAi::setUrl()` / `setToken()`，下一回合请求即用新值（无需重启）；密钥卡默认掩码、点眼睛切明文且焦点不跳字
- [ ] 会话数据目录拼法目测+读码（`AgentConstants.h`）→ `.task` / `.temp` / `.transcripts` / `.task_outputs/tool-results` / `.memory` 零偏差（拼法涉数据兼容，改名即回归）；任务图每任务一个 `.task/task_<hex8>.json`

### 五、P1 异步链与压缩（禁回退阻塞的验证点）

- [ ] 新回合首条消息 → 记忆召回在开聊前**异步**注入（等待期 UI 不冻结、无嵌套事件循环），注入请求 payload 尾部独立消息（`<agent_context>` user，不落 history.json），system 全会话字节恒定（前缀缓存锚点）；召回飞行中 cron 交付被 `m_running` 卫兵拒投不插队
- [ ] 压缩三路径各触发一次（超限自动 / `compact` 工具 / 溢出反应式）→ 全程滚动气泡、切主题、切页均响应
- [ ] 超大工具输出（token 估算 >0.6T′）→ 卸载落 `.task_outputs/tool-results/`，历史内仅保留约 2000 字符预览（预览仍字符域）
- [ ] 压缩完成后查盘 → 原转录落 `.transcripts/*.jsonl`；压缩后历史体量 ≤0.8×T′；派生阈值读码 CompactManager：batch=4×T′ / large=0.6×T′ / 压缩目标=0.8×T′（`T′ = contextTokenBudget() − overhead`，overhead = system + tools schema + 注入块，钳位 `[T/4, T]`；勿写死绝对值），**唯 summary 输入裁剪仍是字符域 1.6S**（内容级启发不随迁）；snip 迟滞 60/50（>60 条**且**过 token 闸门才归档、归档后总量 ≈50 条，10 条迟滞带内不重复 snip）、归档只追加落固定名 `.transcripts/snip_archive.jsonl`（非全量重写）、会话内归档标记为恒定文本（无条数/无路径，缓存友好）
- [ ] 人为制造上下文超限错误 → 服务端 4xx 响应体原文并入 error 文本（错误气泡可见 `context_length_exceeded` 等，不再恒为 Unknown error）；溢出关键词表单源 `isContextOverflowError`；反应式压缩自动重试恰 1 次（预算每 run 归零、成功收响应即复位）后重发
- [ ] 人为制造 429 / 5xx → 指数退避重试（1s,2s,4s…，次数上限 = `settings.ini` 键 `maxRetries`，默认 2、校验界 0~5，越界/非整数回退默认）；重试耗尽的错误文本含服务端响应体解析结果（`error.message [+ code]`，非 JSON 体截 400 字节），不再恒为 Unknown error；重试从头清空累积状态（buffer/thinking/content/toolCalls/usage）
- [ ] 4xx（除 429）仍硬错误不重试 → `context_length_exceeded` 反应式压缩路径不受重试改动影响；可重试判定位于 4xx 分支**之前**（顺序颠倒会让 429 落进硬错误、退避链路成死代码，回归点）
- [ ] 会话自然结束后 → 记忆沉淀 fire-and-forget 挂回合尾巴，不阻塞下一回合输入

### 六、P1 主题与导航样式

- [ ] 设置页轮换三主题 light / dark / atomOneDark → 全窗重绘、无旧主题残色
- [ ] 底色基准取色 → 导航列/内容页/标题栏透色区：light `rgb(249,249,249)`、dark `rgb(40,40,40)`、atomOneDark `rgb(40,44,52)`
- [ ] 导航顶边 border-top 与内容页边框同色基准 → light `rgb(229,229,229)`、dark `rgb(56,56,56)`、atomOneDark `rgb(56,60,68)`；交界处无断色
- [ ] 内容页圆角 → 左上角直角、其余角 10px
- [ ] 滚动条槽色随主题（`LiteHarnessScrollBarAlign.qss` 经 `qproperty-trunkBackgroundColor`）→ 深色主题下 FluScrollBar 槽不再是 FluentUI 默认色
- [ ] 标题栏底色 → StandardTitleBar 为 paintEvent 手绘不吃 QSS，靠置背景透明透出窗口本体色；light 前景黑、其余白，高度 32px
- [ ] 覆盖机制读码警示 → 覆盖 FluentUI 取色只准 `appendOwnSheetOverride` 往**控件自身样式表**幂等追加（marker `/*lh-nav-align*/` 截旧防增长）；若出现窗口级复合选择器写法即回归（像素实证无效）；`ThemeAware::bind` extraRefresh 的「首刷同步 + `singleShot(0)` 重放」双保险仍在（抵消 FluentUI 批处理重写）
- [ ] 会话侧栏 → 标题/模型/工作目录/上下文占用条/状态灯/待办清单六项随回合实时刷新（全部由 `ChatSessionPage` 调 setter 推入，`SessionSidebar` 自身不订阅 `AgentLoop`）；收起后右上角**浮动展开钮**出现且随窗口 resize 对位（页面直接子件、手动几何 + `raise()`），展开钮与侧栏互斥不同时可见；显隐偏好落 settings.ini 键 `sidebarVisible`，重启后按上次状态恢复首帧（读值放在布局收尾，令首帧即按最终态钳宽）；三主题下 `SessionSidebar.qss` 各自生效、无残色

### 七、P2 i18n

- [ ] `settings.ini` 无 `language` 键 → UI 默认中文（中文为源语言，代码一律 `tr("中文")`）
- [ ] 设置页语言切「English」→ 弹「语言切换将在重启后生效。是否立即重启?」；允许 → `exit(931)` + `startDetached` 自重启，重启后**有且仅有一个**实例（main 对 rc==931 只透传，严禁二次拉起=双启缺陷回归点）
- [ ] 重启确认框点取消 → 不重启（close 复用退出守卫）；语言已落 `settings.ini`，下次启动生效
- [ ] 切换后常驻组件重译 → 导航三项 / 设置页 / 新对话页 / WorkDirPathBar / TodoCard 标题取新语言（`changeEvent(LanguageChange)`）；已渲染历史气泡滞留旧语言（已知接受偏差，勿判 bug）
- [ ] Qt 标准对话框（QFileDialog/QMessageBox 按钮）→ 恒为英文，qtbase 中文 qm 已按裁决摘除，属已知取舍勿误报漏译
- [ ] 新增/修改 UI 串流程 → 仓库根跑 `scripts/update-i18n.ps1` 成对刷 `i18n/*.ts` 双文件后构建通过；**严禁**跑 `lite-harness_lupdate` 陷阱 target（会扫 FluentUI 灌入上千外部串）；发往 LLM 的 C 类串（system prompt、`(恢复：工具结果不可用)` 等落盘文本）保持 `QStringLiteral` 不被包 `tr()`
- [ ] 版本号单源 → 设置页「关于」版本 = CMake `project VERSION`（12.6）经 `LITE_VERSION` 宏拼 `s` 前缀运行时注入（显示 `s12.6`，与阶段 tag 同名），全仓无散落硬编码；侧栏页脚 `lite-harness s12.6` 不带多余 `v`

### 八、P2 记忆与子代理

- [ ] 会话自然结束后查 `.memory/` → MEMORY.md 索引 + slug.md 记录新增；强续跑 / 撞调用上限分支**不**触发沉淀；仅 persistent scope 记录且过三重去重
- [ ] 召回 LLM 选择失败 → 关键词打分兜底，兜底记录仍出现在 payload 尾部注入块（不落 history.json，勿回退为注入 system）
- [ ] 记忆整理阈值触发 → 卡片「已整理记忆：%1 → %2 条」；重写中途失败 → 快照回滚原记录完好（无半成品态）；库过大 skip/skipped 降级不崩
- [ ] `task` 子代理 → 全新上下文（不带主会话历史）、黑盒只回最终文本（中间过程不涌入主流气泡）
- [ ] 子代理轮次预算与主循环同源（`start()` 入口快照 `AgentConst::maxToolIterationsValue()` 进 `m_maxTurns`，原固定 `kMaxSubagentTurns = 50` 已删）→ 改「单轮最大调用次数」联动子代理，但单次运行中不随设置变动；预算耗尽以停跑文案收尾
- [ ] 子代理工具白名单 → 仅 read/write/edit/glob + bash 异步；todo_write / task / cron 族等主循环专属工具不可见；其权限询问经同一 permissionRequired 透明转发宿主（同时至多一方待决）

### 九、P2 后台任务与 cron

- [ ] bash 带 `run_in_background: true`（**严格布尔**，字符串 "true"/1 等不算）→ 台账 `bg_0001` 起自增、立即返回不阻塞回合；完成后结果以 `<task_notification>…</task_notification>` 注入下一回合（result 取前 500 字符、无截断标记）
- [ ] cron 5 段表达式校验 → 越界段拒绝（分 0-59 / 时 0-23 / 日 1-31 / 月 1-12 / 周 0-6）；支持 `*`、`*/N`、`a,b,c`、`a-b`；逗号列表全不中判 false（勿回退成默认命中）；日与周双限定为 OR（Vixie 语义）
- [ ] schedule_cron 持久 → `<会话数据根>/scheduled_tasks.json` 原子写；重启后 pending 一次性任务照常重投（装载不剪枝）；id 形如 `cron_<hex8>`
- [ ] at-least-once 两段投递：回合成功 → recurring 清 pending 留表、one-shot 移除；回合失败/停止 → 回队，下一 1s tick 立即重试（不清 lastFired、不等整分钟）
- [ ] 同一分钟去重 → pollDueJobs 同分钟 marker 命中不重复触发；list_crons 输出每行 `%1: %2 -> %3 [%4, %5]`（prompt 截前 60 字符，空表回 `No cron jobs.`）
- [ ] 轮询读码 → 1s QTimer 由宿主 AgentLoop 驱动（零线程）；`stop()` 不杀 cron：调度器存续、交付暂停于 `m_running` 卫兵、空闲后续投；已知偏差：多会话页同 workDir 各持台账可能双触发（登记勿修）

### 十、P2 打包部署与 CI

- [ ] Release 全量构建 `cmake --build build --config Release` → 0 error（FluentUI 首编 >15 分钟，设足超时）
- [ ] 打包 `cpack --config build/CPackConfig.cmake -B build` → 出 `build/lite-harness-s<版本>-win64.zip`（约 54MB/75 条目）；`--config` 必带，否则报 generator not specified；cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）
- [ ] 包内布局 → 顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + `qt.conf`（Prefix=..），根级 `plugins/` + `translations/`；无 FluentUI 泄漏物（`bin/Gallery.exe`、`include/`、`lib/`、`share/`、重复 Qt 运行时）——根级 `install(CODE)` 清理必跑在子目录规则之后
- [ ] 解压到干净机器冒烟 → `<顶层目录>/bin/lite-harness.exe` 可启动；配好 `settings.ini` 的 `apiBaseUrl` / `apiToken` 后可完整对话；本链默认携带 VC 运行库（windeployqt 未传 `--no-compiler-runtime`），故缺 Redist 起不来不再是预期边界
- [ ] push / PR → GitHub Actions `Windows-Qt6.9.0.yml` 触发干净环境全量 **Release** 构建 + `ctest` 单测 + cpack 打包，绿灯即验收；paths 白名单外（如本 docs 改动）不触发；`tests/**` 已在白名单内（改测试也触发验收）

- [ ] tag `v*` / `s*` 推送 → 同一 workflow 把 zip 上传为该 tag 的 GitHub Release 资产；**Release 标题与简介的单源 = 附注 tag 正文**（`git for-each-ref --format='%(contents)'` 读出后喂 `body`，标题统一 `lite-harness <tag>`）；轻量 tag 会被「Read tag annotation as release notes」步骤显式 throw、不产出空简介 Release；改存量已发布 tag 的简介只走 GitHub 网页/API，**勿重跑其 workflow**（会用短附注覆盖富正文）
- [ ] CI 零凭证读码 → workflow 内无任何 token/secret 硬编码或引用
- [ ] 旧就地 `deploy` 目标（windeployqt 自定义目标）已摘除，**勿恢复双轨**：CPack ZIP 是唯一部署/打包路径，不要再验 `--target deploy` 或往 `dist/` 拷产物

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
- **流式期间单行展开**：思考增量到达时自动展开为单行高度、钉底滚动，兼任"正在思考"的进行时指示；收口后自动折回
- **用户干预后不再自动改变**：用户手动展开/折叠过，即尊重其意愿
- **可展开**：需要透明度时（调试、信任建立），用户主动点开
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

### 8. 待办卡（TodoCard）— 时点快照，无进行态

- 头部 `[任务清单] …… [done/total] [▼]`，与 ToolBlock 同款实色底与圆角；计数 tooltip「已完成 %1/%2」。
- 行状态用**文本字形**而非图标：`●` 进行中 / `✓` 完成 / `○` 待办，颜色由 QSS 行状态选择器控制。
- **快照语义**：`todo_write` 无状态，每次成功即以本次全量清单发 `todoUpdated`，UI 侧新建一张卡而非原地改表——历史里因此留下「计划演进」的每个时点，而非只有最终态。
- 它是 `CollapsibleBlock` 家族里**唯一没有进行态**的子类：`liveText()` 落到基类默认空串，轮播定时器对它不启动。
- 标题「任务清单」属常驻文案，`changeEvent(LanguageChange)` 里重取 `tr()`（i18n 第八轮）。

### 9. 会话侧栏（SessionSidebar）— 回合级状态常驻可见

- 与气泡时间线**互补**：气泡流是"发生过什么"（会滚走），侧栏是"现在什么状态"（常驻）——标题 / 模型 / 工作目录 / 上下文占用条 / 状态灯 / 待办清单 / 页脚版本。
- **纯视图**：不订阅 `AgentLoop`，全部由 `ChatSessionPage` 调 setter 推入（`setSessionMeta`/`setWorkDir`/`setContextUsage`/`setRunning`/`setPermissionPending`/`setTodos`）。占用条数值取内核单源 `AgentLoop::estimatedContextTokens()`，UI 不另算一套。
- 状态灯三态合一：运行中 / 待决权限（审批卡挂起时点亮，提示"在等你"）/ 空闲。
- 收起后由右上角**浮动展开钮**恢复：该钮是页面直接子件、手动几何 + `raise()`，`resizeEvent` 与 `setSidebarVisible` 都重摆位（保证出现瞬间即对位）；偏好落 settings.ini 键 `sidebarVisible`，读值放在布局收尾令首帧即按最终态钳宽。

### 10. 子代理进度卡 — `task` 卡的 live 形态与滑窗

- `task` 工具执行时先挂 `startTaskLive()`（标题「子代理执行中」），子代理每完成一轮经 `subagentProgress` → `appendSubagentProgress(turnNo, toolName, summary)` 追加一行。
- **滑窗丢最旧**：行数上限 `AgentConst::kSubagentProgressMaxLines = 60`，超限丢头部并在日志顶部常驻一条「…更早 %1 条进度已省略」——提示行只有一条、永不重复（整体重组写法）。
- 折叠态也可见进展：头部关键参数位同步为最新一行（tooltip 全文，按块宽中部省略）。
- **终态仍由 `toolOutputReady("task")` 唯一收口**：进度行只是过程留痕，绝不替代结果卡；取消/停止走 `finishTaskLiveAborted()` → `applyOutcome("stopped", ■)`（灰方块＝无结果终态，与常规卡同纪律）。

### 11. 记忆卡与压缩卡 — 复用同一 `toolOutputReady` 通道

- 两者都**不新增公共信号**：`CompactManager` / `MemoryManager` 各持一个 card sink，把内部事件（压缩档位达成、记忆 stored/consolidated）经宿主注入的四参 `toolOutputReady(toolName, summary, output, ok)` 出口发出，`toolName` 固定 `"compact"` / `"memory"`，成败仍走 `isToolFailure` 单源判定。
- 好处：UI 侧只需接一条信号即可渲染，历史里也天然与工具卡同构（回放同口径）；代价：`memory` 卡有相位丢失风险，见下。
- **记忆 live 卡**：`memoryPhaseStarted` 挂出（`appendMemoryProgress`）、`memoryChainFinished` 收尾（`finishStreaming` + 释放 `m_memoryBubble`）。
- **M1 相位闸**：`toolOutputReady("memory")` 到达时若 `m_memoryBubble` 已空（清屏 / 双回合交叠丢相位），**静默丢弃该卡**——记忆数据早已落盘 `.memory/`，此卡纯 UI 留痕，兜底成独立气泡反而在清空后的视图里造孤儿。

### 12. 历史回放与实时链路同口径

- 回放三处调用点与实时链路**同参传法**：`startStreaming(tr("处理中…"))` 占位 → `appendHistoryThinkingText(reasoning)` → `appendToolExecution(...)`；回放是同步填满，占位无滞留窗口。
- 工具摘要与成败判定**共用单源**：回放走 `AgentLoopDetail::toolSummary` + `AgentLoop::isToolFailure`，与实时链路一字不差——靠 `ChatSessionPage` 直接 include 内核内部头 `AgentLoopInternal.h`（`friend` 授权的有意例外，见结构篇 §9 异常 2）。
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

- 复用 `ChatStream` 的流式组装、只提取正文 `content`：白得重试 / idle 静默超时 / 配置缺失延迟 error /
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
- **拆分点唯一**：`summarizeHistory` → `summarizeHistoryAsync`（私有），失败交付空串、内部补
  `(empty summary)` 占位——降级语义与迁移前逐字一致。前四段纯本地管线不动。
- `prepareAsync` 按值收 `conversation`、`done` 按值交付最终 conversation（挂起跨 await 后调用方栈引用
  有悬挂风险）；`changed` 判定为"最终态与入参不等价"。
- 入口门槛用宿主锚定的 `conversationTokens`，段内退出判定用管线自身对 conversation 的即时
  `estimateTokens`——两口径可能微偏，入口判超而各段都判达标时**不强推** compactHistory，靠反应式 400
  兜底，防摘要空转抖动。侧链从不增量改 `m_messages`，只在既有检查点整体替换 → tool_use/tool_result
  配对协议安全。

### 5. `AgentLoop` 侧链槽位与 `stop()` 收口顺序

**两个独立句柄槽**（均 `QPointer<QObject>`、parent 到 `this` 随析构自动作废）：

- `m_sideRequest`：**召回 + 三条压缩链共用**（`m_running` 期间同一时刻至多一条前链）。取消时
  `qobject_cast<QOpenAi::AsyncRequest*>` 而非 `static_cast`——同槽异构，转型空即无请求在途。
- `m_memoryRequest`：记忆沉淀链**分槽**，`stop()` 对它既不 cancel 也不清空。理由：前链（召回）是"本回合
  还没开始"的门槛，停则回合作废；记忆链是 `finished` 之后的 fire-and-forget 尾巴，与用户停止意图无关，
  cancel 落在写段之前只会白丢沉淀成果（尽力而为语义下无补偿路径）。
- 记忆链单槽队列：`m_memoryChainActive` + `m_memoryChainPending`（链在跑则置 pending 返回，至多补一次）；
  收口时清 active/句柄 → `emit memoryChainFinished` → 若 pending 且 `!m_running` 立即起下一条。

**`stop()` 收口顺序（不可交换，`AgentLoop.cpp:253-339`）**：

```
卫兵 !m_running → return
① cancelSubAgent()            级联取消，合成 "(cancelled)" 配对回填
② 待决权限视为 deny            直写 "Permission denied"，不经 onToolFinished（不续跑、不发展示信号）
③ 半途工具批收口              flush m_toolResultsReady → 为 m_pendingToolCalls 每条合成 "(stopped)" → 清两队列
④ m_activeProcesses 逐个 kill  其 finished 因 m_running=false 不再继续
⑤ m_currentStream cancel + disconnect + deleteLater
⑥ m_sideRequest cast→cancel    压缩中停止 → 历史不被替换（P3 验证点）
⑦ m_cron.finalizeInFlightDelivery(false)   在途 cron 批回队，下个空闲 tick 重投
⑧ setRunning(false) → persistHistory() → emit error("已停止。")
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
- 审查结论 0 Critical / 0 High / 1 Medium / 5 Low：M1 记忆结果卡相位闸（已落地，见交互篇 §11）；
  **L1 登记为已知限制**——双回合交叠的窄窗口里 pending 链不重发 `memoryPhaseStarted`、旧链 `finished`
  抢先定稿新气泡；修它需给信号加世代参数，收益不配成本。
- **源码注释里的「设计文档 §x.y」是历史出处标记**（约 20 余处，散在 `AgentLoop*` / `MemoryManager*` /
  `CompactManager*` / `ChatSessionPage*` / `ChatMsgEdit.h` / `QOpenAi*`）：它们指向已删除的原规格正文，
  **不可回查、也不必逐处改写**——前面的「异步化 P1/P2/P3/P4」阶段名才是有效线索。现行契约按本部分
  对应小节读：原 §2.x（现状与目标时序）→ 本部分 §1；原 §3.1（AsyncRequest）→ §2；原 §3.2（记忆三链）
  → §3；原 §3.3（压缩）→ §4；原 §3.4 / §6-7（阶段计划与取消语义）→ §5；原 §3.5a/§3.5b（UI 接线）→ §6。
