# AGENTS.md — lite-harness

Qt6 桌面 AI 编码代理 harness：内置 LLM 工具主循环、会话、记忆、定时任务的单可执行文件。

## 构建

- **工具链:** Qt 6.9.0 + MSVC 2022 + CMake 3.20+，C++17，仅 Windows
- **Prefix:** `CMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64`
- **构建目录:** `build/`（VS 解决方案 `build/lite-harness.sln`）
- **输出路径:** `build/bin/lite-harness.exe`（CMake `project VERSION 16.1`；展示/运行时/打包名统一带 `s` 前缀 = `s16.1`，与阶段 tag 同名）
- **编译选项:** MSVC `/W4 /utf-8`（无 `/WX`）；仅链 Qt6 Widgets/Svg/Network + FluentUI::Controls/Utils（find_package 另需 LinguistTools 组件供翻译生成）
- **单元测试:** `tests/` + ctest，见下独立节「测试（tests/ + ctest）」；**无 lint 配置**；**CI:**
 GitHub Actions `.github/workflows/Windows-Qt6.9.0.yml`（main 分支 push/PR 触发于源码 paths 清单，干净环境全量 **Release** 构建 + ctest 单测 + cpack 出 zip 即验收；首跑实测约 19.5 分钟量级。
构建**必须全目标**（勿加 `--target lite-harness`）：FluentUI 子项目 install 规则 configure 期即注册进全树安装清单，单目标构建缺 gallery.exe/cmark.exe 会让 cpack `file(INSTALL)` 硬错误中止（s12.2 两条 run 实证），全量编出后由根级清理剥除。`v*`/`s*` tag 推送触发同一流水线并在末尾经 svenstaro/upload-release-action 把 zip 上传为该 tag 的 GitHub Release——官方语义：paths 过滤不拦 tag；`branches` 与 `tags` 必须显式同写，只写 tags 会静默丢掉分支验收流）
- **手工回归:** 历轮验收场景沉淀为冒烟清单 `docs/doc.md` 第三部分「手工冒烟回归清单」（锚点 `#checklist`；P0/P1/P2 按改动面选组，提交前跑对应组）

- **首次构建前置:** `git submodule update --init 3rdparty/FluentUI`（必需）。`3rdparty/lcc` 仅为移植规格参考、从不参与构建，init 可选；`3rdparty/sqlite_orm` 已从 .gitmodules 与索引 gitlink 清账移除（全仓零引用）；`3rdparty/sqlite`（vendored sqlite3，约 11MB）与 `cmake/FindSQLite3.cmake` 已同批 `git rm`——自摘除子构建起全仓零引用，属纯死码，勿再 vendored 回来。
- **打包 (CPack ZIP，唯一部署/打包路径):** `LITE_PACKAGE` 默认 ON。先构建出 exe，再 `cpack --config build/CPackConfig.cmake -B build`（`--config` 必带，否则报 generator not specified；cpack.exe 与 VS 自带 cmake 同目录）→ `build/lite-harness-s16.1-win64.zip`（约 54MB/75 条目：顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + qt.conf，根级 `plugins/` + `translations/`）。机制与坑（均实证）：① `qt_generate_deploy_app_script` 生成的 windeployqt 命令固定 `--dir . --libdir bin`（多配置 Windows 下 QtDeploySupport 默认），故 exe 必须 `RUNTIME DESTINATION bin`，包内平铺布局不可行；windeployqt 自动写 `bin/qt.conf`（Prefix=..）令根级 plugins/translations 生效；此路默认携带 VC 运行库（windeployqt 无 `--no-compiler-runtime`）。② FluentUI 子项目在同一 staging 树注册了自己的 install 规则（include/lib/share、`bin/` 下 Gallery.exe 及重复 Qt 运行时）——根级 `install(CODE)` 整删垃圾目录 + 逐个删 `bin/` 内非 lite-harness.exe；CMake 子目录规则先于父目录规则执行，父级清理必跑最后。③ FluentUI 内部泄漏过一次 `include(CPack)`，根尾部后发 `include(CPack)` 覆盖生成 `build/CPackConfig.cmake` 才生效（FILE_NAME=lite-harness… 实证）；勿删根级 include 顺序。④ cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）；多配置 staging 默认按 Release 执行子规则。原就地 `deploy`（windeployqt 自定义目标）已随本链落地摘除，勿恢复双轨。CI 已接入本打包链：Windows-Qt6.9.0.yml 按 Release 构建后 `cpack --config build/CPackConfig.cmake -B build`，`v*`/`s*` tag 推送时 zip 自动上传为该 tag 的 GitHub Release。
- **链接坑:** Debug/Release 共用 `build/bin/` 输出目录互相覆盖；运行中的 lite-harness.exe（含用户自己开的实例）占文件导致 LNK1168，重链前先结束占用进程。FluentUI 的 Release 全量首编很慢（>15 分钟），设足超时。**主程序全量重编同样超单轮时限**（拉取上游大批提交后必遇）：`run_in_background` 也受 120s 硬顶，后台全量重编会被杀（实证：日志停在 moc 阶段、无 error，非编译失败）。正解是**同一构建命令跨轮续跑**——MSBuild 以 `.obj` 留存进度，重复调用两三轮即收敛（实测第三次 `exit=0`、`/W4` 零告警），勿为此拆目标或降并行。
- **源文件收集:** `file(GLOB CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` + `GLOB_RECURSE "stylesheet/*.qss"`（经 `qt_add_resources` 打包为 `:/stylesheet/`）。**新增源文件/QSS 无需改 CMakeLists.txt**，重新 configure 即自动拾取。

## 测试（tests/ + ctest）

- **机制:** `tests/` 下纯函数单测；根 `CMakeLists.txt` 的 `LITE_TESTS` 默认 ON → `enable_testing()` +
  `file(GLOB tests/*.cpp CONFIGURE_DEPENDS)` + 单一 `add_test(NAME lite-harness-tests)`。产物
  `build/tests/lite-harness-tests.exe`（**输出目录与 `bin/` 分开**——CPack 的 staging 与根级
  `install(CODE)` 清理都围绕 `bin/` 展开，测试 exe 不进去，杜绝被误打包或被清理规则误删）；测试目标
  只链 `Qt6::Core`，`/W4 /utf-8` 与主目标同口径。
- **本地跑:** `cmake --build build --config Release --target lite-harness-tests`，再设
  `$env:LITE_TEST_TMPROOT`（**值必须以 `tmp-test` 结尾**，`ScopedTempRoot` 的构造/析构双守卫之一）后跑
  `ctest --test-dir build -C Release --output-on-failure`（需 Qt bin 在 PATH 供 `Qt6Core.dll`；CI 由
  install-qt-action 注入）。变量缺失时四个依赖夹具的套件整组 SKIP 且**不计失败**——见下条门禁。
- **CI 位置:** 构建之后、打包之前——单测失败即中止流程，坏产物不进 zip。workflow 除注入
  `LITE_TEST_TMPROOT` 外还注入 `LITE_TEST_MIN_PASS`（断言下界，`tests/main.cpp` 判定）：SKIP 通道本身
  不产生断言也不报失败，早期 CI 未注入 TMPROOT 时 4/5 套件静默跳过仍判通过（假绿），下界门禁即为此设。
  套件另受 `CMakeLists.txt` 的 `set_tests_properties(... TIMEOUT 600)` 保护，挂死不会拖满 workflow 上限。
- **新增套件:** 写 `tests/tst_<模块>.cpp` 暴露 `int tst_<模块>()`（内部用 `TestHarness::check` 断言，
  返回本套件失败数），在 `tests/main.cpp` 加一行调用；GLOB 自动纳入，**无需改 CMakeLists**。测试直接
  `#include` 生产头（`src/` 已入搜索路径），测的是真实编译产物而非算法副本。
- **骨架取舍:** 刻意不引 `Qt6::Test`/moc——可单测对象全是纯函数，无信号槽、无数据驱动表需求，少一个
  组件依赖与一份 DLL 负担，本地 cl 与 CI 都能直接起来（完整理由见 `tests/TestHarness.h` 顶部）。
  `check` 失败不中断，一次跑完看全貌；计数用 C++17 inline 函数内 static，跨 TU 唯一实例、无需定义文件。
- **覆盖现状:** **十四个套件**——`tst_lineending`（`LineEnding.h` 行尾口径）/ `tst_messagebus`（邮箱三重
  路径门 + M8 删除失败）/ `tst_taskstore_lease`（租约六门与双回合释放）/ `tst_worktree`（工作树十门、
  名册与大小写盘符）/ `tst_agentteams`（11/8/4 协议门与队友回合环）/ `tst_mcpmanager`（MCP 命名净化、
  拼名上限、装配碰撞与快照原子性门）/ `tst_agentrecovery`（s15 契约串逐字 + 分类器判据矩阵 + 恢复常量门，
  零夹具）/ `tst_backgroundtasks`（s15 id 序列 / 空命令拒绝 / 严格布尔 / hasPending 只查不清 / collect
   模板逐字 + 200 截断与 FIFO，零夹具）/ `tst_workflowmanager`（s16 身份/哈希/journal/schema/抢占/快照
   纯函数门，155 断言 = 恒跑组 118 + 夹具门控文件组 37）/ `tst_workflowruntime`（s16 limits 恒跑组 +
   端到端 10 分组含 review-changes 全环 / resume 全 cached / 脏缓存拒用 / budget / parallel 屏障 /
   pipeline 无段间屏障 / 嵌套两级门 / 抢占 / 取消，约 87）/ `tst_compacttokens`（计量单源纯头
   `CompactTokens.h`：可加性 / 项与位置无关 / 替换恒等式 / 替换序列逐步对表 / 确定性 / 折算口径锚点，
   19 静态点·实跑 48 条，零夹具）/ `tst_settingscache`（设置读缓存纯头 `SettingsCache.h`：身份三元组
   穷举 / 缺失文件态 / 200 次热读只装载一次 / 四类外部手改（等长·变长·新增·删除）立即生效 / 缓存与
   现取逐键一致（含类型与大小写变体）/ 裸逗号串 QStringList 形态不被抹平 / 写侧落盘与失效成对，
   实跑 75 条 = 恒跑组 18 + 夹具门控组 57）/ `tst_usagetokens`（用量计量纯头 `UsageTokens.h`：有效性门 /
   只增累加 / `hitRatio` 钳界与 `uncachedPrompt` 钳非负 / 嵌套 details 缺失即 0，49 条，零夹具）/
   `tst_streamrate`（输出速度纯头 `StreamRate.h`：空账不除零 / 无效窗口不进分母也不改写 last /
   定窗判据（生成窗口不含 TTFT）/ **Σtokens ÷ ΣgenerationMs 而非各轮速率的算术平均**（同一批窗口两口径
   差 5 倍）/ 纯 tool_calls 回合无增量起点即窗口作废 / 聚合与到达顺序无关而 last 顺序敏感 /
   last 归属（`beginTurn` 只降级归属不清账，本轮无法定窗时不冒充本轮），30 条，零夹具），
   全量实跑 1188 条断言（本轮本机实跑，含 1 条 junction 夹具 skip；上一轮登记的 1095 已过期——
   `tst_usagetokens` 当时未列进本清单）。被测生产
   源需显式列进 `CMakeLists.txt` 的测试目标清单（当前八个 TU：TaskStore / MessageBus / WorktreeManager /
   AgentTeamsManager / McpManager / BackgroundTasksManager / WorkflowManager / WorkflowRuntime），
   **仅允许 QtCore-only 的引擎入表**。header-only 纯头（`LineEnding.h` / `AgentRecovery.h` /
   `CompactTokens.h` / `SettingsCache.h` / `UsageTokens.h` / `StreamRate.h`）**不占此清单**——测试直接 include 生产头即可，零 CMake 改动；带非 QtCore 依赖的
   TU（如 `CompactManager.cpp` 依赖 `QOpenAi::AsyncRequest`）**不得**为凑覆盖率塞进测试目标，
   要直测它先把纯函数抽成头。
   **仍不可证的部分（勿高估覆盖面）:** L4 主循环（`AgentLoop` 家族 15 TU、`SubAgent`、`TeammateRuntime`）
  与网络层 `QOpenAi` **零覆盖**——阻塞点在 `QOpenAi` 的函数局部 static 单例无注入点，需先引入 transport
  接口才能测。这些改动的验证手段仍是「编译期等价 + 冒烟运行 + `docs/doc.md` 清单篇」。s15 宿主接线
  （两拍续打、异步锁存与门、fallback 粘滞、bg 唤醒开回合）同属不可证区——直测仅覆盖纯头与引擎判据
  （`tst_agentrecovery` / `tst_backgroundtasks`），恢复逻辑主体仍靠编译期等价 + 冒烟。s16 宿主接线
  （单飞卫兵、runner 桥、延迟回填、stop 收口）与真 LLM 编排环同属本区——直测仅以 mock runner 与
  同步测试形态覆盖两引擎判据。
  下一批最小切口（均已无 GUI 依赖，零改动或极小搬移即可入表）：`CronSchedulerManager` 的
  `validateCron`/`cronMatches`（已是 static）/ `BashRunner::dangerWarning`（安全关键却 0 直测）/
  `AgentPathGuard` 三个 inline / `SessionStore` 六个 static / `AgentConst::estimateTokens` /
  `AgentLoopDetail::toolSummary`/`parseToolCall`/`AgentLoop::isToolFailure`（困在带 moc 的 TU，
  需仿 `LineEnding.h` 抽纯头；s15 后失败文案族本身已有直测——`tst_agentrecovery` 契约串逐字、
  `tst_backgroundtasks` collect 模板与 "Error:" 前缀文案，函数本体仍 0 直测）/ SKILL.md frontmatter 解析。
- **断言数口径（提交信息勘误，不改历史只登记）:** 提交信息里的断言数**只写实跑数**，静态断言点数
  （`TestHarness::check` 调用点）须另标、不可混写。已发生的两条：`42d1134`（s16.1 测试补强）写的
  「全量实跑 1123 条」实为静态点数（当时实跑 1144、后实测 1184）；`04eed69`（输出速度）写的「全量实跑
  1144 条」是 972+49+123 的推算链、从未实跑过（实测 1184，本轮 1188）。今后凡引用断言数须来自当轮
  实跑输出，推算链不得当实跑引用——CI 注释同口径（其 `LITE_TEST_MIN_PASS=1066` 仍按 1188 留约 10% 余量，
  未随本轮改动）。
- **StreamRate 的两层口径（本轮措辞缺陷修复）:** 聚合口径与 last 归属是两件事。`Meter::beginTurn()`
  在 `AgentLoop::startRateWindow()` 每请求置位、**只降级归属不清账**（账只增不减）；本轮无增量起点
  （纯 tool_calls 回合）时 last 仍是上一个可定窗，UI 措辞必须说「上一可定窗」而非「本轮」。UI 文案走
  `tr()`，措辞分叉即新词条——跑 `scripts/update-i18n.ps1`（不是 CMake 的 `update_i18n` 陷阱目标）补 en
  译文，zh 镜像保持译文=源文。

## 架构（src/）

> 本节只给「模块是什么」的一句话定位；**分层依赖图、AgentLoop 家族 15 个 TU 的职责地图、
> `AgentLoopDetail` 内部工具归属、一条消息的完整数据流与回填总表、运行时目录布局、已知分层
> 异常与技术债、新增代码落位决策树**见 [docs/doc.md](docs/doc.md) 第二部分「模块结构总览」（锚点 `#architecture`）——改结构须同步它**。
> 本仓文档的权威等级、「该读哪篇」与同步纪律见同文件**第一部分（锚点 `#index`）**。

### Agent 核心链

- **AgentLoop** — LLM 主循环 + 27 工具分发（18 基础 + s13 团队 7 + s14 MCP 入口 connect_mcp + s16 编排入口 workflow；名单唯一来源 `ToolNames.h`）：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron / spawn_teammate / list_teammates / send_message / request_shutdown / request_plan / review_plan / create_worktree / connect_mcp。权限门（bash 硬拒绝表 + ASK 规则；**s15 异步回合 fail-closed**：cron/团队唤醒/bg 唤醒三类事件源 emit 前置位 `m_pendingAsyncTurn`、`run()` 入口锁存 `m_asyncTurn`，ASK 门与 mcp__ 策略门在异步回合改返 `RecoveryRules::asyncApprovalDenied()` 定值拒串——不弹卡不挂起队列，deny 表与 granted 短路顺序不动；队友工具适配器 hook 窗口同栈临时置位）与生命周期钩子（UserPromptSubmit/PreToolUse/PostToolUse/Stop——bg 的 PostToolUse 时点已搬至完成，见 BackgroundTasksManager 条）。**s15 per-用户回合恢复态**（对照 lcc RecoveryState，`run()` 入口复位）：`m_recoveryCurrentModel`（fallback 换模回合内粘滞）/`m_maxTokensEscalated`/`m_recoveryCount`——max_tokens 两拍：finish_reason=length 第一拍升档 8000→16000 重打（半截不入账），第二拍 append 半截 + user continuationPrompt 续打 ≤2（`kMaxRecoveryRetries`），耗尽走既有成功终局不发 error（对齐 lcc）；流错误终局 persistHistory 前 append assistant `"[Error] "+msg` 入历史（仿 MCP assemble 发现期先例；emit error 保留=用户裁决偏差，lcc 静默 return）；反应式压缩旗标由 per-API-request 改 per-用户回合一次性（单源复位仅 `run()` 入口，成功路径复位已删，对照 lcc hasAttemptedReactiveCompact）。任务图 6、cron 3、团队 7、MCP 入口 connect_mcp 与编排入口 workflow 仅主循环注册（connect_mcp / workflow 为 Lead 独占，SubAgent 白名单与队友 schema 不含此名天然排除）；`submit_plan` 为队友专属名（引擎匿名 ns 单源，刻意不入 `ToolNames.h`）。**s16 workflow 宿主态**：值成员 `m_workflow`（`WorkflowManager` 引擎，storeDir 由 setWorkDir/setSessionDataId 两推点推 `<会话根>/.workflows`，目录首用自愈在引擎侧）；`workflow` 为 executeTool 第三异步特判（单飞卫兵→runner 桥→延迟回填，lcc 同步阻塞编排不采）；`stop()` 第六路 `cancelActiveWorkflow`（析构同点）全路径零 emit、半批 "(stopped)" 直写历史保配对——详见 AgentLoopWorkflow.cpp 条。**上下文占用单源** `AgentLoop::estimatedContextTokens()`：有锚时取 `usage.prompt_tokens` + 锚后逐条增量（`AgentConst::estimateTokens`，字符→token 折算基准 `kCharsPerTokenBudget=4`），锚对应发送点历史条数与注入块 token 快照；历史被改写（压缩/回滚）或无 usage 则回落全量估算 `CompactManager::estimateTokens(m_messages) + tools schema + 注入块`。侧栏只读该值，别处不再另算一套。
- **提示词单源与注入块** — system prompt **静态化**：`makeSystemPrompt` 只由 workDir / 会话根 / 静态文字决定，全会话字节恒定（前缀缓存与 token 计量都靠它稳定）。每轮变化的数据（技能目录、记忆索引、召回记录、s16 工作流注册清单）**严禁再写回 system**——改由 `makeContextInjection` 生成 `<agent_context>` 注入块随 payload 尾部下发（成员包装 `AgentLoop::buildContextInjection`，在召回 done 续延里快照进 `m_contextInjection`）。s16：system 尾 workflow 编排纪律段为 lcc prompt_workflow 逐字转译 + lite 自有尾三句（清单取名/单飞并发/被拒勿换名），纯静态字节恒定；动态注册清单走注入块第五段（`WorkflowManager::systemPromptNote()` 原文直贴，承 s14 mcpNote 纪律）。零技能 + 零记忆会话不产生注入块，也就不产生无信息量的 overhead。工具 function 定义 `createToolsDefinition` 与本段同属**禁翻区**（`QStringLiteral`，禁包 `tr()`）；workflow 信封单源 `WorkflowManager::toolInfo()`（静态表以原型对象取值，禁双写名称/描述/schema）。
- **QOpenAi** — OpenAI 兼容客户端：`ChatStream` SSE 流式（thinkingDelta / textDelta / messageFinished(fullMessage, finishReason)——s15 双参 BREAKING，全仓 4 连接点补参 / usageReceived / error）+ `AsyncRequest` 一次性异步文本请求（复用 ChatStream，done 恒一次/取消永久静默/总超时兜底；全仓零嵌套事件循环，阻塞族已随异步化 P4 删除）。运行时配置来自 settings.ini 键 `apiBaseUrl` / `apiToken`（`initFromSettings()` 于主窗口构造调用；设置页「模型服务」分组可编辑，写后即时生效），模型名 `MODEL_ID`（仍走环境变量），缺省 `AgentConst::defaultModel()`（settings.ini 键 `defaultModel`，未配置/非法则取生效清单首项）。429/5xx 走指数退避重试（1s,2s,4s…64s，上限 settings.ini 键 `maxRetries`，默认 2、校验界 0~5，`AgentConst::maxRetriesValue()` 单点取值，`initFromSettings()` 注入；**保持无抖动**——不采 lcc s15 的 500ms 基准+25% 抖动，登记偏差）；**s15 传输层扩展**：idle 超时（60s 无数据）转可重试、消耗同一 retriesLeft 预算（耗尽时终态文案逐字节不变；30min 总超时仍直接终态=偏差登记）；529 过载族判定（HTTP 529 或 `AgentRecovery.h` isOverloadedText 文本判据），连续达 2（`kMaxConsecutive529`）且已配置 settings.ini 键 `fallbackModelId`（`QOpenAi::fallbackModelId()`，`initFromSettings()` 注入，空=禁用换模只退避）→ 换请求体 model 重发（usedFallback 每流一次、有效帧清零计数、换模消耗统一重试预算）；`scheduleRetry` 重试前补清 pendingFrame/finishReason（修既有半帧残留潜在缺陷——曾被 429/5xx 路径全量消费 body 掩盖）；可重试判定**必须先于** 4xx 硬错误分支——429 落在 [400,500) 内，顺序颠倒会让退避链路永不可达（曾如此，429 直接终结回合）。**usage 回读**：主循环请求体带 `stream_options.include_usage`（`AsyncRequest` 侧不带、不武装宽限），`include_usage` 语义下末帧 `choices=[]` 且带 usage，故 **usage 捕获必须先于 choices 检查**；`finish_reason` 已见而 usage 未达时启 `kUsageGraceMs`（1500ms）**单次**宽限兜底收尾，重试 attempt 须清空 usage 捕获态从零重收。
- **TaskStore** — 任务图存储（lcc s10 移植）：每任务一个 `<会话根>/.task/task_<hex8>.json`，每次操作直读磁盘；6 个 run_* handler + 14 内核方法，失败一律折叠为工具输出字符串；**s13 租约扩展**：租约版 claim 六门 / complete 后**不退租**（同回合后续工具仍需租约 cwd）/ 双回合边界释放点（`releaseCompletedAssignment`/`releaseTeammateAssignment`）/ `Claimed ` 承重前缀（lcc 跨模块字符串契约）/ 租约台账纯内存重启作废（D7）；遗留非租约路径逐字保留保单代理零行为漂移。
- **SubAgent** — `task` 工具子代理（lcc s06）：全新上下文、黑盒只回最终文本、轮次预算与主循环同源可设置（`maxToolIterations`，start 入口快照）；仅开放 read/write/edit/glob + bash 异步。
- **BashRunner** — bash 执行单源（危险检测 / 截断 / 超时终态 / QProcess 启动），AgentLoop 前后端与 SubAgent 共用。
- **BackgroundTasksManager** — 后台 bash 任务台账（lcc s11）：`run_in_background` 严格布尔判定，宿主驱动 QProcess，结果以 `<task_notification>` 注入下一回合。**s15**：新增 `hasPending()`（只查不清，=`!m_ready.isEmpty()`）挂 AgentLoop 1s tick 作第 4 事件源（收割优先序 team→cron→bg，映射 lcc inbox→cron→bg→stdin；lcc 0.25s 轮询不采、沿用 1s=偏差登记），命中开空 prompt 回合唤醒收割（`scheduledUserMessage(QString(),QString())`，收割唯一在圈顶 `injectBackgroundResults`）；通知模板 `<result>`[:500] → `<summary>`[:200]（lcc §11-3「截断无标记」坑沿不修=登记）；成功占位逐字 `[Background task %1 started] Result will arrive as a task_notification.`；start 失败串 `Error: Failed to start background task: %1`（"Error:" 前缀覆盖判据，旧 `[Background task start error]` 判据已删）；PostToolUse 触发时点由派发时→完成时（QProcess::finished 回调以真实终态 output 触发、按值捕获 toolCall 零台账改动；bg 分支派发处就地返回防双触发；FailedToStart 不触发=偏差登记；lcc §8-⑨ 钩子拦截值前缀与 [hook error] 降级在 lite 无对应语义）。
- **CronSchedulerManager** — cron 定时任务（lcc s12）：5 段表达式校验/匹配，`<会话根>/scheduled_tasks.json` 持久账本，QTimer 1s 轮询替代线程，at-least-once 两段投递。
- **CompactManager** — 上下文压缩（lcc s08）：五段管线 toolResultBudget→snipCompact→microCompact→fitToolResults→compactHistory + 溢出反应式压缩；转录落 `<会话根>/.transcripts/*.jsonl`，超大工具输出卸载到 `.task_outputs/tool-results`。主字符上限可设置（见「数据与路径」）；**阈值判定对象已迁 token 域**：会话体预算 `T' = AgentConst::contextTokenBudget() − overhead`（overhead = system + tools schema + 注入块，钳位 `[T/4, T]`），batch=`4×T'`、单条大结果 `0.6×T'`、压缩目标 `0.8×T'`——比例与原字符口径一致，派生表达式写死在消费点防漂移；唯 summary 输入裁剪（`1.6S`）与预览长度仍是字符域（内容级启发不随迁）。snip 走双门槛回滞：条数 > 60 **且** 会话体估算过 token 闸门才归档，归档后总量 ≈50（10 条迟滞带内不重复触发）；归档只追加落固定名 `.transcripts/snip_archive.jsonl`（非全量重写），会话内标记为恒定文本（无条数、无路径，缓存友好）。**计量与循环成本**：会话体 token 估算单源已抽成纯头 `CompactTokens.h`（`messageTokens` 单条项 / `conversationTokens` 逐条求和），本类 `estimateTokens` 只是它的转发——宿主 `estimatedContextTokens` 与压缩管线共用同一实现，禁再另写一套全量序列化；`microCompact`/`fitToolResults` **不是**每轮全量重估，而是循环前置一次总量 + 每次替换 `total += 新项-旧项` 的增量维护（恒等性由 `CompactTokens.h` 不变量①保证，红线测试 `tst_compacttokens`），阈值语义与触发点未变。本 TU 依赖 `QOpenAi::AsyncRequest`，**不得**为凑覆盖率塞进测试目标，要直测其纯函数先抽头。
- **MemoryManager** — 记忆（lcc s09）：`<会话根>/.memory/`（MEMORY.md 索引 + slug.md 记录）；沉淀（会话自然结束）、召回（LLM 选择 + 关键词兜底 → **上下文注入块**，见上「提示词单源与注入块」条；system 不随召回变动）、整理（阈值重写带快照回滚）。
- **Agent Teams 引擎群（lcc s13）** — 四个零 GUI 内核：`MessageBus`（`<会话根>/.mailboxes/<name>.jsonl` 破坏性读邮箱 = at-most-once；三重 fail-closed 路径门；M8 删除失败即空批+lastError）、`WorktreeManager`（git worktree 十门创建 / 五门移除、分支永留、remove 刻意非工具、落 `.worktrees/<name>` 分支 `wt/<name>`）、`AgentTeamsManager`（Lead 7 工具内核 + 五本账全内存重启作废 + 11/8/4 协议门，门翻转只在队友侧——Lead `runReviewPlan` 不触 planGates）、`TeammateRuntime`（QTimer 2s 心跳无头状态机，回合 LLM 由宿主 `turnRequested` deferred 驱动、`deliverTurnResult()` 回填，零线程零嵌套事件循环）。
- **McpManager** — MCP 外部工具机制引擎（lcc s14 移植，非 QObject 纯引擎、值成员接线）：`connect_mcp` 入口 + 名字净化/拼名（`mcp__{server}__{tool}`，总长 ≤64）+ 每轮 `assemble` 快照原子替换（origins 溯源碰撞检测，发现期失败保上次快照）+ 宿主策略表 allow/confirm 缺省 fail-closed；动态 `mcp__` 工具不入 `ToolNames.h`/静态 schema，仅 `assemble` 追加进请求工具池。**当前注册表为空**（方案1：机制层 + 空注册表，不移植 lcc MCPClient/MOCK_SERVERS 模拟层；`connect_mcp` 对任意名回流 Unknown server 错误文本不崩溃），与 lcc 的 7 条偏差登记在 `src/McpManager.h` 头注释。Lead 独占（SubAgent 白名单不含 `connect_mcp`）。
- **WorkflowManager / WorkflowRuntime（lcc s16 42a555d 移植）** — workflow 编排引擎双件套：`WorkflowManager` 非 QObject 纯 QtCore 引擎、值成员接线（仿 McpManager）：注册表元数据（当前唯一条目 `review-changes`=忠实移植 lcc 演示工作流）/ 身份校验与语义键（createRunId / createTaskId / validateRunId / validateMeta / stableHash / agentKey）/ journal（`.workflows/<runId>.journal.jsonl` 逐行追加 + resume 逐行校验）/ SimpleJsonSchema 校验 + fillSchema / MockAgentRunner / parseRunnerJson / Budget 与 ExecutionLimits / AGENT_CAP=1000 / CONCURRENCY=8 / runId NewOnly 原子预留（撞名重试 ≤32）/ QSaveFile 原子快照 / last_run 账 / 进程内 run 抢占集合（同串 "workflow run {runId} is already active"；lcc 跨进程 fcntl 文件锁不采=偏差）；宿主 `setStoreDir` 推形推 `<会话根>/.workflows`（目录创建自愈在引擎 `ensureStoreDirs`，宿主不 mkdir）；偏差登记①-㉒在 `src/WorkflowManager.h` 头注释；工具信封单源成员 `toolInfo()`。`WorkflowRuntime` 一次性异步编排对象（**非 QObject**，槽用 shared_ptr）：六原语 agent / parallel / pipeline / phase / log / workflow 全事件驱动状态机（QTimer::singleShot(0) 续跑，零线程零嵌套事件循环）；HIT 脏缓存拒用 / MISS schema 唯一一次重试 / resume 四闸先于 journal 截断 / 信封三写终态收口；`kFocusedSystemPrompt()` / `buildRunnerPrompt()` / `kAgentMaxTokens=2000`；偏差 9-19 在头注释。
- **AgentLoopTeam.cpp（s13 宿主接线 TU）** — 六注入装配（launcher/worktreeCreator/permissionCheck/hooksTrigger/toolAdapter×5）、`leadToolCwd` 租约感知 cwd 单点（围栏根随租约 cwd）、三注入边界 + `tryDeliverTeamEvents` 空闲唤醒收割（挂 cron tick 1s 节拍、先于 cron 交付、经 `scheduledUserMessage` 开回合——Gate③ MAJOR-1）、队友回合环与退出清算（`settleTeamOnExit`）。**析构期红线：`settleTeamOnExit` 内禁止 emit 任何信号**——`AgentLoop` 恒为 `ChatSessionPage` 的子对象，Qt 析构时序是「派生类成员先析构 → `~QObject` 才断连并 `deleteChildren`」，此刻到方的 lambda 仍挂在连接上而它触碰的页面成员（如 `m_liveTeammateCards`）已成悬空对象，直连槽执行即 UB。运行期队友卡终局另有三路（`completed`/`exited`/Lead `runningChanged(false)` 兜底扫），均不经本函数。
- **AgentLoopWorkflow.cpp（s16 宿主接线 TU，181 行）** — `startWorkflowRun`：单飞卫兵（占用即回填 "Error: another workflow run is already active"）+ `WorkflowRuntime` 槽（**shared_ptr**，非 QObject 故 QPointer 不可用）与待决 toolCall 槽三件套；workflow 为 executeTool 第三异步特判——**lcc handler 内同步阻塞编排不采，零线程 GUI 红线下改异步延迟回填（仿 task 形=核心偏差）**；runner 桥 `QOpenAi::AsyncRequest::sendText`（focused system prompt + max_tokens 2000，复用 s15 传输恢复链；tokens 无 usage 回读，`AgentConst::estimateTokens` 字符折算=偏差；content 原样回、由 Lane B `agent()` 自 parse）；out 闭包双卫兵（QPointer self + 槽指针匹配，防迟到回填）。`cancelActiveWorkflow`（stop() 第六路收口 + 析构同点）：先清槽 → requestCancel（实文只置旗，终态折叠在下一拍）→ 在途 cancel → 半批以 "(stopped)" 直写历史保 tool_use 配对——**全路径零 emit**（析构窗口红线，与 settleTeamOnExit 一族）。权限门：Hooks workflow 三态门（deny → 定值 "Permission denied by host policy" 硬拒 / confirm → ASK 卡 "Workflow policy requires confirmation" 且异步回合复用 s15 `asyncApprovalDenied()` fail-closed / allow 放行；门位 mcp__ 门后，deny 表与 granted 短路顺序不动；ASK 批准重入仅派发一次）；PermissionCard 两译文同批补登（含 s14 遗留 MCP 词条）。

### 会话层

- **SessionStore** — 纯静态工具：全局索引 `<workDir>/.lite-harness/index.json`（QSaveFile 原子写，条目 dataId/title/model/workDir/时间戳）。只管索引，history.json 归 AgentLoop。
- **SessionRegistry** — 导航 key ↔（page / dataId / 导航子项）反查表 + 右键监听映射；QPointer 观察、零所有权。

### 单源常量 / 纯头

- **AgentConstants.h** — 模型清单（settings.ini 键 `modelOptions` 逗号分隔、`defaultModel` 指定缺省项，未配置/非法回落内置 `kBuiltinModelOptions`；单点取值 `modelOptions()`/`defaultModel()`，**读值必须经本头内 `iniTextValue()`**——裸逗号串在 ini 是 QSettings 的列表语法，`value().toString()` 会得空串即「配置了却不显示」，列表形态要逐元素取原文再按逗号拆，严禁再裸用 `.toString()`）、kMaxTokens 与 s15 恢复常量（kEscalatedMaxTokens=16000 / kMaxConsecutive529=2 / kMaxRecoveryRetries=2）、`fallbackModelIdValue()`（经 `iniTextValue()` 读 settings.ini 键 `fallbackModelId`，空=禁用换模只退避；刻意不做清单内校验——fallback 语义是给清单外服务兜底）、bash 超时/错误文案、输出截断、上下文上限默认/校验界（kContextCharLimitDefault/Min/Max）与单点取值 `contextCharLimitValue()`、glob 上限与剪枝目录、数据目录名（`.task`/`.temp`/`.transcripts`/`.task_outputs/tool-results`/`.mailboxes`/`.worktrees`/`.workflows`（s16 编排台账） + 预留队友名 `kReservedTeammateNames`={lead,agent} 与团队节拍/等待界常量 — 拼法涉数据兼容，不可改；上下文上限数值则只是默认值语义，可被 settings.ini 覆盖，非硬约束）。
- **LayoutConstants.h** — 消息列宽/边距（NewChatPage/ChatSessionPage/ChatMsgEdit 同列对齐）。
- **NavItem.h** — 导航键常量（NavKey）+ NavItem（带 removeChildItem）。**ToolNames.h** — 27 工具名（18 基础 + 7 团队 + 1 MCP 入口 connect_mcp + 1 编排入口 workflow（s16，Lead 独占）；`submit_plan` 队友专属名不入表，单源在引擎匿名 ns）；另含 `MCP_PREFIX` 前缀常量（供权限钩子识别 `mcp__` 命名域，非工具名；故表内常量计 28）。**ToolTagKind.h** — 工具→样式标签（write/run/search/read/plan/delegate/other），经动态属性喂给 QSS 选择器。
- **LineEnding.h** — 文本文件行尾归一单源（header-only 纯函数）：`dominant` 主导行尾判定（CRLF 数 ≥ 裸 LF 数且非零判 CRLF，平局偏 CRLF）/ `toLf` 匹配域归一 / `apply` 写回域还原（幂等，不产生 `\r\r\n`）/ `replaceOnce` 两级匹配单次替换。**纪律：匹配域归一到 LF、写回域按文件主导行尾还原**——`read_file` 以 `QTextStream` 逐行读再用 `'\n'` join，交还模型的永远是 LF 文本，而落盘文件在本仓（`core.autocrlf=true`）多为 CRLF；`edit_file` 若拿原始字节直接匹配，多行 `old_string` 必然失配（报 text not found），LF `new_string` 原样插入又会混入裸 LF。孤立 CR 不折叠（不误伤正文 CR 字面量）。`countOccurrences` 统计命中次数（不重叠计数，与两级匹配同口径：在哪个域命中就在哪个域计数），经 `replaceOnce` 的 `matchCount` 出参回传（与 `matched` 同为 nullptr 容错）。**`edit_file` 三道防线**（`runEditFileIn`）：① 空 `old_string` 拒绝——`indexOf("")` 恒返回 0，原语义会把 `new_string` 静默前插到文件开头（模型漏填参数即毁文件头）；② 体量上限 `kEditFileMaxBytes`（5MB）拒绝——整文件读入 + 多次整串拷贝（编码往返预检、行尾归一与还原）在零线程下会冻结主线程，`read_file` 已因此限量读，`edit_file` 不能反而无界；超限不尝试部分读写（部分写会毁文件）；③ **多处命中拒绝**（**有意偏离** lcc `str.replace(old,new,1)` 的静默替换第一处）——改哪一处取决于文件里恰好先出现哪个，模型无从判断，静默改错位置比失败更危险，回可判定错误要求补上下文使其唯一。
- **AgentRecovery.h** — s15 恢复契约串与文本判据纯头（`RecoveryRules` 命名空间，禁翻区）：逐字契约串 `continuationPrompt()`（"Continue from the previous response. Do not repeat completed work."）与 `asyncApprovalDenied()`（"Permission denied: interactive approval is unavailable during an asynchronous turn"）——跨模块承重措辞（两拍续打、异步定值拒、isToolFailure 前缀判定、测试逐字断言），改文案须同步 `tst_agentrecovery`、`isToolFailure` 的 `startsWith("Permission denied")` 判据与 TeammateRuntime 镜像（两处双写、注释互指）；`isRateLimitText()` / `isOverloadedText()` 鸭子文本判据（小写 contains：429/ratelimit/rate limit/rate_limit；overloaded/529）供 `QOpenAi` 可重试判定。
- **StreamRate.h** — token 输出速度（token/s）计算单源（header-only 纯头，仿 `UsageTokens.h` 先例）：`Window`（三时刻：t0 流建立 / t1 本轮首个增量 / t2 usage 末帧）+ `makeWindow`（判据）+ `tokensPerSecond` + `Meter`（会话级只增账）。**三条不可混的口径**：①分母是生成窗口 t1→t2、**不含 TTFT**——把「请求发出→usage」整窗口当分母会把慢首字读成慢模型，两种状态对体感作用相反，故 TTFT 单独记录、单独显示；②无增量起点即窗口作废——纯 tool_calls 回合的 delta 只有 tool_calls 字段、不触发 `textDelta`/`thinkingDelta`，宁可不显示也不把 TTFT 冒充生成速度；③聚合 = Σtokens ÷ ΣgenerationMs，**不是**各窗口速率的算术平均（否则 1 token 窗口与 2000 token 窗口等权，一条长回答被九条短回答稀释）。有效性判据与 `UsageTokens::fromUsage` 同源（`prompt<=0` 即整窗作废），无效窗口不进分母也不改写 `last`，空账 UI 整行隐藏。宿主只观测三个时刻（`AgentLoopRequest.cpp` 的 `startRateWindow`/`markFirstOutputDelta`/`closeRateWindow`：每请求重开表、流内重试不重开，退避计入 TTFT），折算与判据全在本头——这是它能进 ctest 的全部原因（`AgentLoop` 链 QOpenAi/FluentUI，按 QtCore-only 纪律进不了测试目标）。覆盖面比消耗账更窄：仅主循环请求（队友增量不经宿主信号面、压缩/记忆/workflow 走 `AsyncRequest` 无 usage 通道），UI tooltip 同步声明。

### UI 层

- **LiteHarness** — 主窗口（FluFrameLessWidget）：FluVNavigationView + FluStackedLayout，会话新建/恢复（按索引升序）/重命名/删除，关闭时运行守卫。
- **ChatSessionPage** — 每会话一页：AgentLoop + 滚动消息流 + 输入框 + 只读路径条；历史回放与就地刷新。
- **NewChatPage / SettingsPage / BasePage** — 发起页（进入时重读 settings.ini 默认目录）、设置页（settings.ini `defaultWorkDir`、`contextCharLimit`、`apiBaseUrl`/`apiToken` 模型服务卡）、页面基类。
- **MessageBubbleWidget** — 气泡流式渲染（打字机），首次工具/思考事件后重建为时间线。
- **CollapsibleBlock** — 折叠动画基类（32px 头部 + 300ms OutCubic contentHeight 动画），子类 ThinkingBlock / ToolBlock / TodoCard。基类构造禁调虚函数，子类构造尾再 bind 主题。
- **ThemeAware** — 「加载 QSS + 订阅 themeChanged + 重载」样板单源（约 12 处旧复制已收敛）。
- **SessionSidebar** — 会话右栏信息面板（标题/模型/工作目录/上下文占用条/状态灯/待办清单/团队名册/页脚版本）。**纯视图，不订阅 `AgentLoop`**：全部信息由 `ChatSessionPage` 调 setter 推入（`setSessionMeta`/`setWorkDir`/`setContextUsage`/`setRunning`/`setPermissionPending`/`setTodos`/`setTeammates`）。状态灯三态合一（空闲/运行中/等待审批，waiting 优先）留在本类——s16.1 曾随「上下文」节一起下移到状态条，随后按用户要求整盏搬回：灯是面板的身份信息，与「侧栏收起也必须常驻」的计量读数不同类，两处同时亮会在同一时刻给出两个读数。显隐单点 `ChatSessionPage::setSidebarVisible()`：偏好落 settings.ini 键 `sidebarVisible`（默认显示），收起后由右上角**浮动展开钮**恢复（页面直接子件、`resizeEvent` 手动摆位 + `raise()`，与侧栏互斥）。
- **ChatStatusStrip** — 聊天输入框正下方的常驻状态条（**一行三段文字**：上下文占用数字 + 消耗段 + 速度段），s16.1 从 SessionSidebar 拆出：侧栏可收起而这些计量读数必须常驻，故随输入框走。占用**条**（`#ctxTrack`/`#ctxFill`）与「运行状态」三态灯**不在**本组件，两者归侧栏——条与数字是同一读数的两种画法（比例算术单源 `UsageTokens::usageRatio`，两处共用不会给出两个数），而灯在侧栏再亮一份就是两个读数。同为**纯视图，不订阅 `AgentLoop`**，由 `ChatSessionPage` 调 setter 推入（`setContextUsage`/`setTokenUsage`/`setOutputRate`，**无** `setRunning`/`setPermissionPending`）；作为输入组末位成员由 `applyColumnWidth` 钳宽、自身不设 `maximumWidth`；算术仍在 `UsageTokens.h`/`StreamRate.h` 纯头，空账整段隐藏纪律不变，**三段全空时整条隐藏**（`applyEmptiness`，输入框下不留空行）。
- **ChatMsgEdit / SendMsgButton / PermissionCard / WorkDirPathBar / FluentInputDialog** — 输入区、圆形 SVG 发送钮、内联审批卡（拒绝默认焦点、理由 EN→ZH）、工作目录条、通用单行输入对话框。

## 数据与路径

- 所有会话数据落在 **`<workDir>/.lite-harness/`**（`SessionStore::rootDirFor`）——是会话工作目录下的相对根，**不是**用户主目录。
- 带 sessionDataId 时隔离到 `sessions/<id>/`（history.json、.task、.memory、.transcripts、scheduled_tasks.json、.mailboxes、.worktrees、.workflows 等）；`skills/` 始终跨会话共享。**s16 `.workflows/` 产物**：`<runId>.json` 原子快照 / `<runId>.journal.jsonl` HIT 账本 / `<runId>.output.json` 信封 / `last_run.txt`；lcc 跨进程 `.lock` 文件锁不采、由进程内 runId 抢占集合替代（=偏差登记）。
- 设置存储 = `AppSettings.h` 单源的 **exe 同目录 `settings.ini`**（QSettings IniFormat；**读一律 `AppSettings::value()`、写一律 `AppSettings::setValue()`**——缓存与失效判据在 `SettingsCache.h` 的 `Reader`，按文件身份 `(exists,mtime,size)` 惰性重载，手改 ini 下一次取值即生效；原逃生口 `AppSettings::ini()` 已摘除，禁在消费点现构造 `QSettings`，理由见 `docs/doc.md` §9.10 第 10 条；键 `defaultWorkDir`/`contextCharLimit`/`maxToolIterations`/`language`/`sidebarVisible`/`apiBaseUrl`/`apiToken`/`modelOptions`/`defaultModel`/`maxRetries`/`fallbackModelId`（s15 新增；设置页无编辑卡=留观，手改 ini）；用户裁决弃用注册表）。`modelOptions` 手改写成裸逗号串时 QSettings 会解析成 QStringList（两种形态——手改裸串与设置页写单值——都要能读回，故读值走 `AgentConstants.h` 的 `iniTextValue()`，见上条）。
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
- **版本号单源:** CMake `project VERSION`（数字段，如 16.1）→ `LITE_VERSION` 宏拼 `s` 前缀（→ `s16.1`，与阶段 tag 同名）→ App.cpp `setApplicationVersion` → 设置页标签/侧栏页脚取运行时值，不许散落硬编码。`s` 前缀只出现在 CMakeLists 的宏注入与 `CPACK_PACKAGE_FILE_NAME` 两处，升版本只改 `project VERSION` 一行。

## 关键约定

- 导航键统一取自 `NavItem.h::NavKey`；页面注册 `m_sLayout->addWidget(key, page)`，导航项用同一键。
- 魔法数字进 `AgentConstants.h`（agent 参数）或 `LayoutConstants.h`（布局尺寸），不散落字面量。
- 工具侧失败折叠为输出字符串交还 LLM，不抛异常、不弹窗。s13 团队 7 工具与队友侧同口径（引擎内核失败一律折叠为文本，含 worktree `Error:`/`Partial` 折叠形）。
- lcc 移植规格权威：源码注释以 `lcc sXX + hash` 标注对应阶段（s03–s16），参照仓库 `3rdparty/lcc`（不入构建）。
- FluentUI 头文件 `<FluUtils.h>`、`<FluThemeUtils.h>` 等位于 `3rdparty/FluentUI/{controls,utils}`。
- **日志单源 = FluentUI Utils 的 `FluLogUtils`**（主程序是 GUI 子系统、无 stderr 宿主，不接管则调试输出全蒸发）：`App.cpp` 在 `QApplication` 构造后 `QDir().mkpath("../log")` + `FluLogUtils::__init()`，`exec()` 返回后 `FluLogUtils::__deinit()`（QFile 关闭才 flush，缺它丢末段；自重启走 `qApp->exit(931)` 非 `std::exit`，故该行必达）。**`mkpath` 是必需兜底不是锦上添花**——`__init` 内部路径写死 `../log/<时间戳>.log`（相对 CWD），而 `QFile::open` 不建目录：目录缺失 → open 失败 → 它连 message handler 都不装、静默全丢。**`myMessageOutput` 只写 `msg` 原文（`tWrite << msg << "\n"`）、不加任何前缀**——实证：裸 `qDebug` 行落盘是 `[i18n] loaded translator: :/i18n/…`（无时间无定位），FluentUI 自己的 `LOG_WARN` 行是 `[2026-10-11 08:13:29.701][debug][I18n.cpp][…][38] …`。故 **`LOG_*` 宏前缀不是冗余，而是文件里唯一的时间/定位来源**。调用点已全量迁宏（63 处、18 文件），统一经中转头 `src/LiteLog.h`（各文件在自身首个 `#include` 之后引它）：主程序侧落到 `FluLogUtils.h` 原生宏，测试侧由 CMake 给 `lite-harness-tests` 定义 `LITE_NO_FLU_LOG`，退化宏为 `qDebug().nospace().noquote()`（与上游同流修饰符、只差前缀；`TaskStore.cpp`/`AgentTeamsManager.cpp` 同进两目标，测试只链 `Qt6::Core`，如此测试 exe 不被拖进 Qt6::Gui 的 DLL 依赖，ctest 仍读其 stdout）。**写法 `LOG_XXX << …`（裸写，无括号、无 `.noquote()`）**——`noquote` 已收进上游 FluentUI 的宏定义（上游 `master` 提交 `e337f514`，基于 1.0.9 已 fast-forward 合入、临时分支已删；本仓子模块随之由 `1.0.8-54` 更新到该提交）。历史成因留此备查：当年宏尾是 `<< "] "`，在调用点补 `.noquote()` 必须写成 `(LOG_DEBUG).noquote()`（裸写的 `.` 绑到字符串字面量 → C2228），且 `Qt::noquote` manipulator 在 Qt 6.9 不存在（`qdebug.h` 只有成员函数 `noquote()` → C2039）。宏内仍保留 `nospace()`，**多参数项间空格要靠字面量显式携带**（已全仓补齐 14 处，如 `"QOpenAi [retry] " << n << " / " << max`）。级别映射 `qDebug→LOG_DEBUG`/`qInfo→LOG_INFO`/`qWarning→LOG_WARN`；**2 处 `qCritical` 保留原样**（`LOG_ERR` 底层是 `qDebug()`，迁过去级别降级、`QT_LOGGING_RULES` 按级别过滤失效）。已知取舍：每启动一个新文件、无轮转清理。

## Git 提交
采用中文日志

## 发布（tag 与 Release）

- **升版本：行为只改一行，文档必须 grep 同步。** 行为单源是 CMake
  `project(lite-harness VERSION <数字段>)`——`LITE_VERSION` 宏注入与 `CPACK_PACKAGE_FILE_NAME`
  都从 `${PROJECT_VERSION}` 派生，于是运行时展示版本、zip 名、tag 名三者自动同名（`s16.1`），
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




