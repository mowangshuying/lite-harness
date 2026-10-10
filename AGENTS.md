# AGENTS.md — lite-harness

Qt6 桌面 AI 编码代理 harness：内置 LLM 工具主循环、会话、记忆、定时任务的单可执行文件。

## 构建

- **工具链:** Qt 6.9.0 + MSVC 2022 + CMake 3.20+，C++17，仅 Windows
- **Prefix:** `CMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64`
- **构建目录:** `build/`（VS 解决方案 `build/lite-harness.sln`）
- **输出路径:** `build/bin/lite-harness.exe`（CMake `project VERSION 13`；展示/运行时/打包名统一带 `s` 前缀 = `s13`，与阶段 tag 同名）
- **编译选项:** MSVC `/W4 /utf-8`（无 `/WX`）；仅链 Qt6 Widgets/Svg/Network + FluentUI::Controls/Utils（find_package 另需 LinguistTools 组件供翻译生成）
- **单元测试:** `tests/` + ctest，见下独立节「测试（tests/ + ctest）」；**无 lint 配置**；**CI:**
 GitHub Actions `.github/workflows/Windows-Qt6.9.0.yml`（main 分支 push/PR 触发于源码 paths 清单，干净环境全量 **Release** 构建 + ctest 单测 + cpack 出 zip 即验收；首跑实测约 19.5 分钟量级。
构建**必须全目标**（勿加 `--target lite-harness`）：FluentUI 子项目 install 规则 configure 期即注册进全树安装清单，单目标构建缺 gallery.exe/cmark.exe 会让 cpack `file(INSTALL)` 硬错误中止（s12.2 两条 run 实证），全量编出后由根级清理剥除。`v*`/`s*` tag 推送触发同一流水线并在末尾经 svenstaro/upload-release-action 把 zip 上传为该 tag 的 GitHub Release——官方语义：paths 过滤不拦 tag；`branches` 与 `tags` 必须显式同写，只写 tags 会静默丢掉分支验收流）
- **手工回归:** 历轮验收场景沉淀为冒烟清单 `docs/doc.md` 第三部分「手工冒烟回归清单」（锚点 `#checklist`；P0/P1/P2 按改动面选组，提交前跑对应组）

- **首次构建前置:** `git submodule update --init 3rdparty/FluentUI`（必需）。`3rdparty/lcc` 仅为移植规格参考、从不参与构建，init 可选；`3rdparty/sqlite_orm` 已从 .gitmodules 与索引 gitlink 清账移除（全仓零引用）。`3rdparty/sqlite`（vendored sqlite3）目录残留但同样从不进构建图。
- **打包 (CPack ZIP，唯一部署/打包路径):** `LITE_PACKAGE` 默认 ON。先构建出 exe，再 `cpack --config build/CPackConfig.cmake -B build`（`--config` 必带，否则报 generator not specified；cpack.exe 与 VS 自带 cmake 同目录）→ `build/lite-harness-s13-win64.zip`（约 54MB/75 条目：顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + qt.conf，根级 `plugins/` + `translations/`）。机制与坑（均实证）：① `qt_generate_deploy_app_script` 生成的 windeployqt 命令固定 `--dir . --libdir bin`（多配置 Windows 下 QtDeploySupport 默认），故 exe 必须 `RUNTIME DESTINATION bin`，包内平铺布局不可行；windeployqt 自动写 `bin/qt.conf`（Prefix=..）令根级 plugins/translations 生效；此路默认携带 VC 运行库（windeployqt 无 `--no-compiler-runtime`）。② FluentUI 子项目在同一 staging 树注册了自己的 install 规则（include/lib/share、`bin/` 下 Gallery.exe 及重复 Qt 运行时）——根级 `install(CODE)` 整删垃圾目录 + 逐个删 `bin/` 内非 lite-harness.exe；CMake 子目录规则先于父目录规则执行，父级清理必跑最后。③ FluentUI 内部泄漏过一次 `include(CPack)`，根尾部后发 `include(CPack)` 覆盖生成 `build/CPackConfig.cmake` 才生效（FILE_NAME=lite-harness… 实证）；勿删根级 include 顺序。④ cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）；多配置 staging 默认按 Release 执行子规则。原就地 `deploy`（windeployqt 自定义目标）已随本链落地摘除，勿恢复双轨。CI 已接入本打包链：Windows-Qt6.9.0.yml 按 Release 构建后 `cpack --config build/CPackConfig.cmake -B build`，`v*`/`s*` tag 推送时 zip 自动上传为该 tag 的 GitHub Release。
- **链接坑:** Debug/Release 共用 `build/bin/` 输出目录互相覆盖；运行中的 lite-harness.exe（含用户自己开的实例）占文件导致 LNK1168，重链前先结束占用进程。FluentUI 的 Release 全量首编很慢（>15 分钟），设足超时。**主程序全量重编同样超单轮时限**（拉取上游大批提交后必遇）：`run_in_background` 也受 120s 硬顶，后台全量重编会被杀（实证：日志停在 moc 阶段、无 error，非编译失败）。正解是**同一构建命令跨轮续跑**——MSBuild 以 `.obj` 留存进度，重复调用两三轮即收敛（实测第三次 `exit=0`、`/W4` 零告警），勿为此拆目标或降并行。
- **源文件收集:** `file(GLOB CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` + `GLOB_RECURSE "stylesheet/*.qss"`（经 `qt_add_resources` 打包为 `:/stylesheet/`）。**新增源文件/QSS 无需改 CMakeLists.txt**，重新 configure 即自动拾取。

## 测试（tests/ + ctest）

- **机制:** `tests/` 下纯函数单测；根 `CMakeLists.txt` 的 `LITE_TESTS` 默认 ON → `enable_testing()` +
  `file(GLOB tests/*.cpp CONFIGURE_DEPENDS)` + 单一 `add_test(NAME lite-harness-tests)`。产物
  `build/tests/lite-harness-tests.exe`（**输出目录与 `bin/` 分开**——CPack 的 staging 与根级
  `install(CODE)` 清理都围绕 `bin/` 展开，测试 exe 不进去，杜绝被误打包或被清理规则误删）；测试目标
  只链 `Qt6::Core`，`/W4 /utf-8` 与主目标同口径。
- **本地跑:** `cmake --build build --config Release --target lite-harness-tests`，再
  `ctest --test-dir build -C Release --output-on-failure`（需 Qt bin 在 PATH 供 `Qt6Core.dll`；CI 由
  install-qt-action 注入）。
- **CI 位置:** 构建之后、打包之前——单测失败即中止流程，坏产物不进 zip。
- **新增套件:** 写 `tests/tst_<模块>.cpp` 暴露 `int tst_<模块>()`（内部用 `TestHarness::check` 断言，
  返回本套件失败数），在 `tests/main.cpp` 加一行调用；GLOB 自动纳入，**无需改 CMakeLists**。测试直接
  `#include` 生产头（`src/` 已入搜索路径），测的是真实编译产物而非算法副本。
- **骨架取舍:** 刻意不引 `Qt6::Test`/moc——可单测对象全是纯函数，无信号槽、无数据驱动表需求，少一个
  组件依赖与一份 DLL 负担，本地 cl 与 CI 都能直接起来（完整理由见 `tests/TestHarness.h` 顶部）。
  `check` 失败不中断，一次跑完看全貌；计数用 C++17 inline 函数内 static，跨 TU 唯一实例、无需定义文件。
- **覆盖现状（勿高估）:** 目前**只有 `LineEnding.h` 一个头有测试**（`tests/tst_lineending.cpp`）。其余
  改动的验证手段仍是「编译期等价 + 冒烟运行 + `docs/doc.md` 清单篇」，大面积行为回归**不可证**。
  下一批最小切口（均已无 GUI 依赖）：`AgentLoopDetail::toolSummary` / `parseToolCall` /
  `AgentLoop::isToolFailure` / `BashRunner::dangerWarning` / cron 表达式匹配 /
  `AgentConst::estimateTokens` / SKILL.md frontmatter 解析。

## 架构（src/）

> 本节只给「模块是什么」的一句话定位；**分层依赖图、AgentLoop 家族 14 个 TU 的职责地图、
> `AgentLoopDetail` 内部工具归属、一条消息的完整数据流与回填总表、运行时目录布局、已知分层
> 异常与技术债、新增代码落位决策树**见 [docs/doc.md](docs/doc.md) 第二部分「模块结构总览」（锚点 `#architecture`）——改结构须同步它**。
> 本仓文档的权威等级、「该读哪篇」与同步纪律见同文件**第一部分（锚点 `#index`）**。

### Agent 核心链

- **AgentLoop** — LLM 主循环 + 25 工具分发（18 基础 + s13 团队 7；名单唯一来源 `ToolNames.h`）：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron / spawn_teammate / list_teammates / send_message / request_shutdown / request_plan / review_plan / create_worktree。权限门（bash 硬拒绝表 + ASK 规则）与生命周期钩子（UserPromptSubmit/PreToolUse/PostToolUse/Stop）。任务图 6、cron 3 与团队 7 工具仅主循环注册；`submit_plan` 为队友专属名（引擎匿名 ns 单源，刻意不入 `ToolNames.h`）。**上下文占用单源** `AgentLoop::estimatedContextTokens()`：有锚时取 `usage.prompt_tokens` + 锚后逐条增量（`AgentConst::estimateTokens`，字符→token 折算基准 `kCharsPerTokenBudget=4`），锚对应发送点历史条数与注入块 token 快照；历史被改写（压缩/回滚）或无 usage 则回落全量估算 `CompactManager::estimateTokens(m_messages) + tools schema + 注入块`。侧栏只读该值，别处不再另算一套。
- **提示词单源与注入块** — system prompt **静态化**：`makeSystemPrompt` 只由 workDir / 会话根 / 静态文字决定，全会话字节恒定（前缀缓存与 token 计量都靠它稳定）。每轮变化的数据（技能目录、记忆索引、召回记录）**严禁再写回 system**——改由 `makeContextInjection` 生成 `<agent_context>` 注入块随 payload 尾部下发（成员包装 `AgentLoop::buildContextInjection`，在召回 done 续延里快照进 `m_contextInjection`）。零技能 + 零记忆会话不产生注入块，也就不产生无信息量的 overhead。工具 function 定义 `createToolsDefinition` 与本段同属**禁翻区**（`QStringLiteral`，禁包 `tr()`）。
- **QOpenAi** — OpenAI 兼容客户端：`ChatStream` SSE 流式（thinkingDelta / textDelta / messageFinished / usageReceived / error）+ `AsyncRequest` 一次性异步文本请求（复用 ChatStream，done 恒一次/取消永久静默/总超时兜底；全仓零嵌套事件循环，阻塞族已随异步化 P4 删除）。运行时配置来自 settings.ini 键 `apiBaseUrl` / `apiToken`（`initFromSettings()` 于主窗口构造调用；设置页「模型服务」分组可编辑，写后即时生效），模型名 `MODEL_ID`（仍走环境变量），缺省 `AgentConst::defaultModel()`（settings.ini 键 `defaultModel`，未配置/非法则取生效清单首项）。429/5xx 走指数退避重试（1s,2s,4s…，上限 settings.ini 键 `maxRetries`，默认 2、校验界 0~5，`AgentConst::maxRetriesValue()` 单点取值，`initFromSettings()` 注入）；可重试判定**必须先于** 4xx 硬错误分支——429 落在 [400,500) 内，顺序颠倒会让退避链路永不可达（曾如此，429 直接终结回合）。**usage 回读**：主循环请求体带 `stream_options.include_usage`（`AsyncRequest` 侧不带、不武装宽限），`include_usage` 语义下末帧 `choices=[]` 且带 usage，故 **usage 捕获必须先于 choices 检查**；`finish_reason` 已见而 usage 未达时启 `kUsageGraceMs`（1500ms）**单次**宽限兜底收尾，重试 attempt 须清空 usage 捕获态从零重收。
- **TaskStore** — 任务图存储（lcc s10 移植）：每任务一个 `<会话根>/.task/task_<hex8>.json`，每次操作直读磁盘；6 个 run_* handler + 14 内核方法，失败一律折叠为工具输出字符串；**s13 租约扩展**：租约版 claim 六门 / complete 后**不退租**（同回合后续工具仍需租约 cwd）/ 双回合边界释放点（`releaseCompletedAssignment`/`releaseTeammateAssignment`）/ `Claimed ` 承重前缀（lcc 跨模块字符串契约）/ 租约台账纯内存重启作废（D7）；遗留非租约路径逐字保留保单代理零行为漂移。
- **SubAgent** — `task` 工具子代理（lcc s06）：全新上下文、黑盒只回最终文本、轮次预算与主循环同源可设置（`maxToolIterations`，start 入口快照）；仅开放 read/write/edit/glob + bash 异步。
- **BashRunner** — bash 执行单源（危险检测 / 截断 / 超时终态 / QProcess 启动），AgentLoop 前后端与 SubAgent 共用。
- **BackgroundTasksManager** — 后台 bash 任务台账（lcc s11）：`run_in_background` 严格布尔判定，宿主驱动 QProcess，结果以 `<task_notification>` 注入下一回合。
- **CronSchedulerManager** — cron 定时任务（lcc s12）：5 段表达式校验/匹配，`<会话根>/scheduled_tasks.json` 持久账本，QTimer 1s 轮询替代线程，at-least-once 两段投递。
- **CompactManager** — 上下文压缩（lcc s08）：五段管线 toolResultBudget→snipCompact→microCompact→fitToolResults→compactHistory + 溢出反应式压缩；转录落 `<会话根>/.transcripts/*.jsonl`，超大工具输出卸载到 `.task_outputs/tool-results`。主字符上限可设置（见「数据与路径」）；**阈值判定对象已迁 token 域**：会话体预算 `T' = AgentConst::contextTokenBudget() − overhead`（overhead = system + tools schema + 注入块，钳位 `[T/4, T]`），batch=`4×T'`、单条大结果 `0.6×T'`、压缩目标 `0.8×T'`——比例与原字符口径一致，派生表达式写死在消费点防漂移；唯 summary 输入裁剪（`1.6S`）与预览长度仍是字符域（内容级启发不随迁）。snip 走双门槛回滞：条数 > 60 **且** 会话体估算过 token 闸门才归档，归档后总量 ≈50（10 条迟滞带内不重复触发）；归档只追加落固定名 `.transcripts/snip_archive.jsonl`（非全量重写），会话内标记为恒定文本（无条数、无路径，缓存友好）。
- **MemoryManager** — 记忆（lcc s09）：`<会话根>/.memory/`（MEMORY.md 索引 + slug.md 记录）；沉淀（会话自然结束）、召回（LLM 选择 + 关键词兜底 → **上下文注入块**，见上「提示词单源与注入块」条；system 不随召回变动）、整理（阈值重写带快照回滚）。
- **Agent Teams 引擎群（lcc s13）** — 四个零 GUI 内核：`MessageBus`（`<会话根>/.mailboxes/<name>.jsonl` 破坏性读邮箱 = at-most-once；三重 fail-closed 路径门；M8 删除失败即空批+lastError）、`WorktreeManager`（git worktree 十门创建 / 五门移除、分支永留、remove 刻意非工具、落 `.worktrees/<name>` 分支 `wt/<name>`）、`AgentTeamsManager`（Lead 7 工具内核 + 五本账全内存重启作废 + 11/8/4 协议门，门翻转只在队友侧——Lead `runReviewPlan` 不触 planGates）、`TeammateRuntime`（QTimer 2s 心跳无头状态机，回合 LLM 由宿主 `turnRequested` deferred 驱动、`deliverTurnResult()` 回填，零线程零嵌套事件循环）。
- **AgentLoopTeam.cpp（s13 宿主接线 TU）** — 六注入装配（launcher/worktreeCreator/permissionCheck/hooksTrigger/toolAdapter×5）、`leadToolCwd` 租约感知 cwd 单点（围栏根随租约 cwd）、三注入边界 + `tryDeliverTeamEvents` 空闲唤醒收割（挂 cron tick 1s 节拍、先于 cron 交付、经 `scheduledUserMessage` 开回合——Gate③ MAJOR-1）、队友回合环与退出清算（`settleTeamOnExit`，析构无 emit）。

### 会话层

- **SessionStore** — 纯静态工具：全局索引 `<workDir>/.lite-harness/index.json`（QSaveFile 原子写，条目 dataId/title/model/workDir/时间戳）。只管索引，history.json 归 AgentLoop。
- **SessionRegistry** — 导航 key ↔（page / dataId / 导航子项）反查表 + 右键监听映射；QPointer 观察、零所有权。

### 单源常量 / 纯头

- **AgentConstants.h** — 模型清单（settings.ini 键 `modelOptions` 逗号分隔、`defaultModel` 指定缺省项，未配置/非法回落内置 `kBuiltinModelOptions`；单点取值 `modelOptions()`/`defaultModel()`，**读值必须经本头内 `iniTextValue()`**——裸逗号串在 ini 是 QSettings 的列表语法，`value().toString()` 会得空串即「配置了却不显示」，列表形态要逐元素取原文再按逗号拆，严禁再裸用 `.toString()`）、kMaxTokens、bash 超时/错误文案、输出截断、上下文上限默认/校验界（kContextCharLimitDefault/Min/Max）与单点取值 `contextCharLimitValue()`、glob 上限与剪枝目录、数据目录名（`.task`/`.temp`/`.transcripts`/`.task_outputs/tool-results`/`.mailboxes`/`.worktrees` + 预留队友名 `kReservedTeammateNames`={lead,agent} 与团队节拍/等待界常量 — 拼法涉数据兼容，不可改；上下文上限数值则只是默认值语义，可被 settings.ini 覆盖，非硬约束）。
- **LayoutConstants.h** — 消息列宽/边距（NewChatPage/ChatSessionPage/ChatMsgEdit 同列对齐）。
- **NavItem.h** — 导航键常量（NavKey）+ NavItem（带 removeChildItem）。**ToolNames.h** — 25 工具名（18 基础 + 7 团队；`submit_plan` 队友专属名不入表，单源在引擎匿名 ns）。**ToolTagKind.h** — 工具→样式标签（write/run/search/read/plan/delegate/other），经动态属性喂给 QSS 选择器。
- **LineEnding.h** — 文本文件行尾归一单源（header-only 纯函数）：`dominant` 主导行尾判定（CRLF 数 ≥ 裸 LF 数且非零判 CRLF，平局偏 CRLF）/ `toLf` 匹配域归一 / `apply` 写回域还原（幂等，不产生 `\r\r\n`）/ `replaceOnce` 两级匹配单次替换。**纪律：匹配域归一到 LF、写回域按文件主导行尾还原**——`read_file` 以 `QTextStream` 逐行读再用 `'\n'` join，交还模型的永远是 LF 文本，而落盘文件在本仓（`core.autocrlf=true`）多为 CRLF；`edit_file` 若拿原始字节直接匹配，多行 `old_string` 必然失配（报 text not found），LF `new_string` 原样插入又会混入裸 LF。孤立 CR 不折叠（不误伤正文 CR 字面量）。`countOccurrences` 统计命中次数（不重叠计数，与两级匹配同口径：在哪个域命中就在哪个域计数），经 `replaceOnce` 的 `matchCount` 出参回传（与 `matched` 同为 nullptr 容错）。**`edit_file` 三道防线**（`runEditFileIn`）：① 空 `old_string` 拒绝——`indexOf("")` 恒返回 0，原语义会把 `new_string` 静默前插到文件开头（模型漏填参数即毁文件头）；② 体量上限 `kEditFileMaxBytes`（5MB）拒绝——整文件读入 + 多次整串拷贝（编码往返预检、行尾归一与还原）在零线程下会冻结主线程，`read_file` 已因此限量读，`edit_file` 不能反而无界；超限不尝试部分读写（部分写会毁文件）；③ **多处命中拒绝**（**有意偏离** lcc `str.replace(old,new,1)` 的静默替换第一处）——改哪一处取决于文件里恰好先出现哪个，模型无从判断，静默改错位置比失败更危险，回可判定错误要求补上下文使其唯一。

### UI 层

- **LiteHarness** — 主窗口（FluFrameLessWidget）：FluVNavigationView + FluStackedLayout，会话新建/恢复（按索引升序）/重命名/删除，关闭时运行守卫。
- **ChatSessionPage** — 每会话一页：AgentLoop + 滚动消息流 + 输入框 + 只读路径条；历史回放与就地刷新。
- **NewChatPage / SettingsPage / BasePage** — 发起页（进入时重读 settings.ini 默认目录）、设置页（settings.ini `defaultWorkDir`、`contextCharLimit`、`apiBaseUrl`/`apiToken` 模型服务卡）、页面基类。
- **MessageBubbleWidget** — 气泡流式渲染（打字机），首次工具/思考事件后重建为时间线。
- **CollapsibleBlock** — 折叠动画基类（32px 头部 + 300ms OutCubic contentHeight 动画），子类 ThinkingBlock / ToolBlock / TodoCard。基类构造禁调虚函数，子类构造尾再 bind 主题。
- **ThemeAware** — 「加载 QSS + 订阅 themeChanged + 重载」样板单源（约 12 处旧复制已收敛）。
- **SessionSidebar** — 会话右栏信息面板（标题/模型/工作目录/上下文占用条/状态灯/待办清单/页脚版本）。**纯视图，不订阅 `AgentLoop`**：全部信息由 `ChatSessionPage` 调 setter 推入（`setSessionMeta`/`setWorkDir`/`setContextUsage`/`setRunning`/`setPermissionPending`/`setTodos`）。显隐单点 `ChatSessionPage::setSidebarVisible()`：偏好落 settings.ini 键 `sidebarVisible`（默认显示），收起后由右上角**浮动展开钮**恢复（页面直接子件、`resizeEvent` 手动摆位 + `raise()`，与侧栏互斥）。
- **ChatMsgEdit / SendMsgButton / PermissionCard / WorkDirPathBar / FluentInputDialog** — 输入区、圆形 SVG 发送钮、内联审批卡（拒绝默认焦点、理由 EN→ZH）、工作目录条、通用单行输入对话框。

## 数据与路径

- 所有会话数据落在 **`<workDir>/.lite-harness/`**（`SessionStore::rootDirFor`）——是会话工作目录下的相对根，**不是**用户主目录。
- 带 sessionDataId 时隔离到 `sessions/<id>/`（history.json、.task、.memory、.transcripts、scheduled_tasks.json、.mailboxes、.worktrees 等）；`skills/` 始终跨会话共享。
- 设置存储 = `AppSettings.h` 单源的 **exe 同目录 `settings.ini`**（QSettings IniFormat；键 `defaultWorkDir`/`contextCharLimit`/`maxToolIterations`/`language`/`sidebarVisible`/`apiBaseUrl`/`apiToken`/`modelOptions`/`defaultModel`/`maxRetries`；用户裁决弃用注册表）。`modelOptions` 手改写成裸逗号串时 QSettings 会解析成 QStringList（两种形态——手改裸串与设置页写单值——都要能读回，故读值走 `AgentConstants.h` 的 `iniTextValue()`，见上条）。
- 上下文压缩上限可设置（settings.ini 键 `contextCharLimit`，默认 200000 字符，校验界 10000~5000000；缺失/非法回退默认，单点取值 `AgentConst::contextCharLimitValue()`）。**它是字符口径的唯一入口**：全局 token 预算 `T = contextCharLimit / kCharsPerTokenBudget(4)` 经 `AgentConst::contextTokenBudget()` 现取现用，压缩阈值 batch/large/目标按 `4×T'`/`0.6×T'`/`0.8×T'` 缩放（`T' = T − overhead`，见「CompactManager」条），summary 输入裁剪仍按 `1.6S` 字符域。设置页写值后压缩管线下一回合即生效，无需重启。
- 单轮最大工具调用次数可设置（settings.ini 键 `maxToolIterations`，默认 500，校验界 10~1000；缺失/非法/越界回退默认，`AgentConst::maxToolIterationsValue()` 单点取值，设置页与主循环共用）；主循环回合入口快照，中途改设置不影响当前回合。SubAgent 轮次预算与之同源（`start()` 入口快照进 `m_maxTurns`，原固定 `kMaxSubagentTurns = 50` 已删）。
- **零线程原则:** 全仓库主线程事件驱动，轮询/异步一律 QTimer + QProcess 信号，不起线程。

## 主题 / QSS

- 三套主题 `light` / `dark` / `atomOneDark`；QSS 位于 `stylesheet/<theme>/<Widget>.qss`，打包为 `:/stylesheet/`（调试期回退 `../stylesheet/`，解析逻辑在 FluentUI 的 FluStyleSheetUtils）。
- 组件接主题：构造函数**末尾**调用 `ThemeAware::bind("X.qss", widget, extraRefresh)`；widget 作为连接 context 自动随析构断开；extraRefresh 处理 SVG 重着色等组件特有步骤。
- **新增带 QSS 的组件:** 放置 `src/*.cpp/.h` 与三主题目录下同名 `.qss`，构造尾 bind 即可 — GLOB 自动拾取，**不改 CMakeLists.txt**。
- **覆盖 FluentUI 自带取色** 不改第三方源码的做法：**窗口级复合选择器无效**（像素实证——FluentUI 各控件把主题 QSS `setStyleSheet` 挂自身，Qt 级联近表优先，特异性再高压不过）。正确机制是**往控件自身样式表幂等追加覆盖规则**：`LiteHarness.cpp` 的 `appendOwnSheetOverride`（marker 注释截旧再追加防增长）+ `applyNavAlignOverrides()`（经 FluStyleSheetUtils 同源路径读本仓每主题 `LiteHarnessNavAlign.qss` / `LiteHarnessScrollBarAlign.qss`，后者用 `qproperty-trunkBackgroundColor` 改 FluScrollBar 滚动条槽色——其 paintEvent 直读该属性）；接线在 `ThemeAware::bind` 的 extraRefresh：首刷同步一次保首帧 + `singleShot(0)` 重放一次（FluThemeUtils::setTheme 把 themeChanged 与代理批处理重写控件表打包进同一 queued lambda，须排其后抵消）。导航列/内容页底色对齐即此法（三主题基准：light 249,249,249 / dark 40,40,40 / atomOneDark 40,44,52）。标题栏 StandardTitleBar 为 paintEvent 手绘不吃 QSS，靠置其背景透明透出窗口本体色。

## 国际化（i18n）

- **中文为源语言:** UI 文案一律 `tr("中文")`；英文译文在 `i18n/lite-harness_en_US.ts`，另有 `i18n/lite-harness_zh_CN.ts` 同文镜像目录（译文=源文，供 Linguist 审计全量 UI 串清单、与 en 目录对称，装载它零行为差异）。构建经 `qt_add_translations` 跑 lrelease 生成 qm 并内嵌资源（运行时路径 `:/i18n/lite-harness_en_US.qm` / `:/i18n/lite-harness_zh_CN.qm`，与 CMake 两侧同源写死在 I18n.cpp）。Qt 标准对话框按钮为英文（qtbase 中文 qm 已按裁决摘除，不内嵌二进制）；FluentUI 自带控件中文由其静态库资源提供（`:/i18n/Controls.zh-CN.qm`）。
- **I18n.h/.cpp 单源:** `language()` 读 settings.ini 键 `language`（AppSettings 单源；缺省 `zh-CN`，与 defaultWorkDir 同源——**不用** FluConfigUtils 的 CWD 相对 config.ini）；`applyLanguage()` 全量装卸 translator，**必须在任何窗口构造前调用**（App.cpp，构造期 tr() 定稿）；`setLanguage()` 写 settings.ini 并同步 FluConfigUtils 取值；`requestRestart()` 走 gallery 惯例 `exit(931)` + `startDetached` 自重启——main() 对 rc==931 只透传，严禁二次拉起（双启缺陷）。
- **切换=重启生效:** FluentUI 控件无运行中重译能力（无 languageChanged 信号）。仅跨切换常驻的组件经 `changeEvent(QEvent::LanguageChange)` 重译（LiteHarness 导航三项 / SettingsPage / NewChatPage / WorkDirPathBar / TodoCard 标题）；弹出即重建的菜单/对话框/卡片天然取当次语言。已渲染历史气泡滞留旧语言（接受）。
- **新增/修改 UI 字符串:** 照常写中文 `tr()`；**勿跑 `lite-harness_lupdate` 目标**（实证会把 FluentUI 子工程源码扫入、灌入上千外部串），改手动 `lupdate -recursive src -no-obsolete -source-language zh_CN -target-language zh_CN -ts i18n/lite-harness_zh_CN.ts` 与 `-ts i18n/lite-harness_en_US.ts` 各刷一次，补英文译文再构建；漏译条目运行时回退中文源。成对手刷已固化为 `scripts/update-i18n.ps1`（仓库根执行即可，两条 `-ts` 命令的权威单源）。
- **禁翻区:** 发往 LLM 的 C 类串（system prompt、压缩摘要指令、`AgentLoop` 落盘进历史的合成文本如 `(恢复：工具结果不可用)`——已改 `QStringLiteral` 硬隔离，禁再包 `tr()`）不进翻译、不入 .ts。品牌名/版本号豁免口径：代码中 `tr("lite-harness")`/`tr("AtomOneDark")` 等品牌专名与版本标签**保留 tr 包裹**（版本号经 LITE_VERSION 宏注入串天然不在 ts），在 .ts 内以译文=源文恒等登记，保证 Linguist 审计面完整。
- **版本号单源:** CMake `project VERSION`（数字段，如 13）→ `LITE_VERSION` 宏拼 `s` 前缀（→ `s13`，与阶段 tag 同名）→ App.cpp `setApplicationVersion` → 设置页标签/侧栏页脚取运行时值，不许散落硬编码。`s` 前缀只出现在 CMakeLists 的宏注入与 `CPACK_PACKAGE_FILE_NAME` 两处，升版本只改 `project VERSION` 一行。

## 关键约定

- 导航键统一取自 `NavItem.h::NavKey`；页面注册 `m_sLayout->addWidget(key, page)`，导航项用同一键。
- 魔法数字进 `AgentConstants.h`（agent 参数）或 `LayoutConstants.h`（布局尺寸），不散落字面量。
- 工具侧失败折叠为输出字符串交还 LLM，不抛异常、不弹窗。s13 团队 7 工具与队友侧同口径（引擎内核失败一律折叠为文本，含 worktree `Error:`/`Partial` 折叠形）。
- lcc 移植规格权威：源码注释以 `lcc sXX + hash` 标注对应阶段（s03–s13），参照仓库 `3rdparty/lcc`（不入构建）。
- FluentUI 头文件 `<FluUtils.h>`、`<FluThemeUtils.h>` 等位于 `3rdparty/FluentUI/{controls,utils}`。

## Git 提交
采用中文日志

## 发布（tag 与 Release）

- **升版本：行为只改一行，文档必须 grep 同步。** 行为单源是 CMake
  `project(lite-harness VERSION <数字段>)`——`LITE_VERSION` 宏注入与 `CPACK_PACKAGE_FILE_NAME`
  都从 `${PROJECT_VERSION}` 派生，于是运行时展示版本、zip 名、tag 名三者自动同名（`s13`），
  代码里**不得**再硬编码版本号（口径详见「国际化」节末「版本号单源」条）。
  `project VERSION` 只收数字点分，故 `s` 前缀永远不进 CMake 版本字段。
  **但「只改一行」只对行为成立**：README / AGENTS / `docs/doc.md`（结构篇与清单篇）与若干
  源码注释把当前版本号当**示例**写死了（s12.6 这次实测 21 处、跨 7 文件），改完 `project VERSION`
  必须跑 `git grep -n "12\.6" -- . ':(exclude)3rdparty' ':(exclude)build'` 逐处同步，
  否则文档与现实矛盾、下一轮读文档的人无从判断哪个是当前版本。

- **tag 命名:** `s<数字段>`，与版本号单源同名（历史：`s00` 前身即 `v0.1.0`）。CI 的 tag 过滤
  写的是 `v*`/`s*`，两者都会触发 Release 上传，但新 tag 一律用 `s` 前缀保持一致。
- **发布流程:** ① 改 `project VERSION` 一行 + `git grep` 同步文档示例版本号（见上条）→
  ② 本地全量 Release 构建（勿 `--target`，原因见

  「构建」节）+ `ctest` 全绿 + `cpack` 出包，核对 zip 名已带新版本号 → ③ 跑
  `docs/doc.md` 清单篇（第三部分）的 P0（+ 按改动面选 P1）→ ④ 提交（日志写明版本 bump）→
  ⑤ **`git tag -a`（必须附注）** 后 `git push origin s<版本>`。轻量 tag 会被 CI 的
  `Read tag annotation as release notes` 步骤显式 throw，不产出空简介 Release。

- **tag 推送即公开发布，实质不可撤回:** workflow 末尾 `svenstaro/upload-release-action` 对不存在
  的 Release 自动创建，且 `overwrite: true` 会覆盖同名资产。故 tag 只在 main 已绿、包已核对后打。
  打错的补救是 `git push origin :refs/tags/s<版本>` + `git tag -d s<版本>` 删除重打，但**已上传的
  Release 与资产需手动到 GitHub 清理**，不会随 tag 删除自动消失。
- **分支 push 不发布:** main push 只跑「构建 + ctest + cpack」验收，不上传任何资产。官方语义
  「Path filters are not evaluated for pushes of tags」——tag 流不受 paths 白名单拦截，故只改
  docs 的阶段也能正常发版。
- **Release 标题与简介的单源 = 附注 tag 正文:** CI 在上传前用
  `git for-each-ref --format='%(contents)' refs/tags/<tag>` 读出 tag 附注，喂给
  `upload-release-action` 的 `body`，标题统一 `lite-harness <tag>`。**写 tag 附注即等于写
  Release 简介**，不存在第二处要同步的地方。此前 s12.5/s12.6 的 Release 标题为 null、正文为空
  （该步骤当年没传 `release_name`/`body`），s12.2/s12.3 的简介则是人工在网页补的——四种来源
  方式并存必然不一致，故收拢为 tag 附注单源。
  - **简介格式:** 首行一句「本轮定位」概述 → `### 相对 <上一版> 的变更` 下按 `**分组名**` +
    `- 条目` 列要点 → 末尾 `### 产物` 写 zip 名与运行方式。**不写 H1**（标题已由 Release name
    承载，正文再写一遍是冗余）。
- **Release 只覆盖 s12.2 起:** CI 发布链自 s12.2 接入，s00–s12.1 没有 Release 属正常、不是漏发，
  勿为它们补建无资产的 Release。另注：`s12.4` 这个 tag **不存在**（s12.3 之后直接 s12.5，跳号）。
- **单源只对 s12.7 起的新 tag 成立，存量 4 个是例外，勿重跑其 workflow:**
  上条「附注即简介」是**向后**的规矩。已存在的 s12.2/s12.3/s12.5/s12.6 正文是**手工规范化**的
  （s12.3 正文本就与附注不同——附注 264 字扁平列表、正文 729 字带分组；s12.5/s12.6 正文为
  2026-10 一次性回填，附注仍是一行标题），二者**已经分叉**。由于 `New Release` 步骤会把
  `body` 写成 tag 附注，**重跑这四个 tag 的 workflow 会用短附注覆盖富正文**。
  要改这四个 Release 的文字，直接在 GitHub 网页或 API 改，不要重跑 tag 流水线；
  也不要为了让二者对齐而强推（force-push）这些已发布 tag——那会触发重新构建并覆盖已发布资产。

- **历史资产文件名不统一，且刻意不改:** s12.2/s12.3/s12.5 的 zip 名为
  `lite-harness-0.1.0-win64.zip`——当年 `project VERSION` 就是 `0.1.0`（版本号单源化在 s12.5..s12.6
  区间才落地）。**包内顶层目录同名**（已实测 zip 本地文件头，解压出的就是
  `lite-harness-0.1.0-win64/`），所以只改 GitHub 资产外层名会造成「下载名 s12.5、解压目录 0.1.0」
  的新不一致，比现状更糟；重打包又会改动已发布二进制。结论：历史包名保留不改，s12.6 起包名与
  tag 同名。


