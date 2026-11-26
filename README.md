# Lite-Harness

用 Qt6 (C++) 编写的桌面 AI 编码代理 harness —— 单可执行文件，内置 LLM 工具主循环、会话管理、记忆与定时任务。

## 功能

- **多会话**：新建 / 重命名 / 删除 / 重启恢复；全局索引在配置文件 `config.json` 的 `app/sessions` 键里（与宿主其它键、FluentUI 键同一个文件），每会话 history 单独落 `sessions/<id>/history.json`（`<config>` = `<exe 目录>/../config`，全部运行时数据同此一锚）
- **工具主循环（27 工具）**：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / 任务图 6 工具（create/update/list/get/claim/complete_task）/ cron 3 工具（schedule_cron / list_crons / cancel_cron）/ Agent 团队 7 工具（spawn_teammate / list_teammates / send_message / request_shutdown / request_plan / review_plan / create_worktree）/ MCP 入口（connect_mcp）/ 编排入口（workflow，s16）
- **子代理**：`task` 工具派发独立上下文子代理，黑盒回传结果
- **权限门**：内联审批卡（拒绝为默认焦点），bash 危险命令硬拒绝表 + 工作区外写入 ASK 规则；异步回合（cron / 团队唤醒 / 后台唤醒）fail-closed——不弹卡，直接定值拒绝
- **流式渲染**：打字机气泡 + 思考块 / 工具执行块折叠动画 + Todo 进度卡
- **会话侧栏**：标题 / 模型 / 工作目录 / 上下文占用条 / 状态灯 / 待办清单；可收起，偏好持久化后由浮动钮恢复
- **记忆**：会话结束自动沉淀、下轮召回注入上下文（以 `<agent_context>` 注入块随请求尾部下发，system 全程字节恒定）、超阈值整理（Markdown 存储）
- **项目指令**：自动加载仓库根的 `AGENTS.md`（没有则回落 `<config>/AGENTS.md`，两级互斥只取一份；两者都没有则不产生该段），同样走 `<agent_context>` 注入块尾部下发；正文超 16000 字符只下发前缀并在段标题标注「first N of M characters shown」，会话中途编辑下一回合即生效
- **上下文压缩**：五段管线 + 溢出反应式压缩，阈值按 token 预算判定（以 `usage.prompt_tokens` 锚定估算占用），转录与超大工具输出落盘。**压缩过程在会话流里可见**：本地四段各出一张轻量卡（「已卸载」/「已归档」/「已微压缩」/「已裁剪」），四张互不合并、只在该段真改动了内容时才发（空转段不发卡，避免每请求刷屏）；全量压缩另有「压缩中」实时卡、「已压缩」完成卡与「压缩失败」卡
- **cron 定时任务**：5 段表达式、JSON 持久账本、at-least-once 投递
- **后台 bash**：`run_in_background` 异步执行，结果以 `<task_notification>`（`<summary>` 摘要）注入后续回合；完成后 ≤1s 级 tick 自动开空回合收割（第 4 事件源）
- **错误恢复**：429/5xx/idle 超时指数退避，连续 529 过载可换 `fallbackModelId` 兜底模型；max_tokens 两拍（升档重打 + 续打提示）；流错误以 `[Error]` 降级入历史（UI 报错照旧可见）
- **工作流编排（s16）**：`workflow` 工具启动宿主预注册编排流程（当前唯一条目 `review-changes` 审查工作流）；六原语 agent/parallel/pipeline/phase/log/workflow 零线程异步状态机，journal 落盘、`resume_from_run_id` 续跑未变步骤直接命中缓存；宿主单飞卫兵 + 异步延迟回填；策略三态门 deny 硬拒 / confirm 弹卡（异步回合 fail-closed）/ allow 放行
- **三主题**：light / dark / atomOneDark，QSS 资源打包、运行时热切换
- **中英文界面**：中文源文本 + Qt Linguist 英文译文（`i18n/`），设置页切换、重启生效；译文内嵌单 exe，FluentUI 官方中文随库资源
- **模型选择**：下拉切换（生效清单与缺省项由 `config.json` 键 `app/providers` / `app/modelOptions` / `app/defaultModel` 决定；**s18.5 起未配置即空清单**——内置默认项 `kBuiltinModelOptions` 已删，不再回退，由用户自行添加），API 经设置页「模型管理」弹窗配置（服务商选择/新增/删除、服务地址与 API Key、每模型思考/强度/显示/上下文窗口偏好）

## 运行时配置

用户配置单源为 **`<config>/config.json`**（`<config>` = `<exe 目录>/../config`，路径单源 `LitePaths::configFilePath()`；`AppSettings.h`，QSettings 自定义 Json 格式；弃用注册表）。**同一个文件也是 FluentUI 自己的配置落点**（主题 / 导航宽度 / 语言镜像）——两侧合流后各占一个子对象互不可见：宿主键一律挂在 `app/` 组下（下表键名即 `app/<键>`，盘上形态 `{"app":{…}}`），FluentUI 自有键在 `config/`、`translation/` 组下。Json 后端**大小写敏感**，手改时键名须逐字写对（`apiToken` 写成 `apitoken` 读不到）。设置页可编辑大部分键，其余手改配置文件即可（非法值由各消费点校验后回退默认）——读侧带按文件身份 `(exists,mtime,size)` 的惰性重载缓存（`SettingsCache.h`），**手改在下一次取值即生效，不需要重启**；写侧落盘与缓存失效成对发生：

| 键 | 含义 | 默认 |
| --- | --- | --- |
| `apiBaseUrl` | OpenAI 兼容 API 基地址（http/https） | — |
| `apiToken` | API token（设置页值区脱敏显示；明文存本机配置文件） | — |
| `modelOptions` | 模型下拉清单，逗号分隔或数组（裸串与列表两种形态都能读回，归一化单源 `AgentConst::textValue()`） | 空（**s18.5 起未配置即空清单**：内置 `kBuiltinModelOptions` 已删、不再回退） |
| `defaultModel` | 缺省选中模型 | 清单首项；清单为空时返回空串（绝不 `first()` 越界） |
| `providers` | 多服务商表（数组 `[{id,name,baseUrl,token,models},…]`，判据单源 `ProviderSpec.h`）。**非空时摊平出的模型清单优先于 `modelOptions`**，旧三键 `apiBaseUrl`/`apiToken`/`modelOptions` 仍可读、下次保存时迁移 | 空 |
| `modelPrefs` | 每模型偏好（数组 `[{model,thinking,effort,visible,window},…]`，判据单源 `ModelPrefs.h`；思考开关/推理强度/是否显示/上下文窗口四字段按模型名逐条往返，旧对象形态可读并迁移） | 空 |
| `customModelNames` | 自定义模型名清单（弹窗「添加」与 `/models` 拉取归入、移除时同步剪除，与 `modelPrefs` 互不残留） | 空 |
| `defaultWorkDir` | 新建会话的默认工作目录 | — |
| `contextCharLimit` | 上下文窗口上限（**token**，校验界 10000~5000000；**声明值即全局 token 预算**，s17.9 起不再 ÷4 换算；键名沿用 `contextCharLimit` 是为兼容用户既有配置文件，语义已是 token 域） | 200000 |
| `maxToolIterations` | 单轮最大工具调用次数（校验界 10~1000；子代理预算同源） | 500 |
| `maxRetries` | 429/5xx 指数退避重试上限（校验界 0~5） | 2 |
| `permissionApproval` | 工具执行权限审批总开关（true=命中询问规则弹审批卡挂起队列；false=三道询问门一律放行、工具直接执行）。**撤的是「问不问」不是「许不许」**：bash 硬拒绝黑名单、workflow 的 deny 策略、文件工具的工作区越界拦截都不受开关影响。缺省/读不出的一律回退 true（fail-safe 多问一次）；设置页「权限」卡可改，**下一个工具调用即生效**（钩子现取），无需重启 | 需要审批 |
| `fallbackModelId` | 连续 529 过载后换用的兜底模型（空=禁用换模，仅退避；设置页无编辑卡，手改配置文件） | 空 |
| `language` | 界面语言（`zh-CN` / `en-US`，切换需重启） | `zh-CN` |
| `sidebarVisible` | 会话侧栏显隐偏好 | 显示 |
| `uiFontScale` | 界面字号倍率（整数百分比，校验界 80~200；**改后需重启**，与 `language` 同语义；设置页无编辑卡，手改配置文件）。生效三面：应用字体、本仓 QSS 的 `font-size`、以及代码里 `setPixelSize` 的组件（三面同读**启动快照**，故不会半生效）；刻意不改 FluentUI 自有 QSS，导航项等保留原字号 | 100 |
| `dpiRounding` | 高 DPI 缩放取整策略（`Round`/`Ceil`/`Floor`/`RoundPreferFloor`/`PassThrough`，大小写与首尾空白不敏感；缺失或非法回退 `PassThrough`）。**必须在 `QApplication` 构造前落定**，故读值走 `AppSettings::bootstrapValue(argv[0], …)`——这是「读一律 `value()`」的唯一例外 | `PassThrough` |
| `windowGeometry` | 主窗口几何（`saveGeometry()` 的 base64 串，编解码单源 `UiScale::encodeGeometry`/`decodeGeometry`）。写点在 `LiteHarness::closeEvent` 且仅退出被接受时覆盖，读点在构造函数末尾 `show()` 之前；串被手改坏则回退默认尺寸，绝不猜一个尺寸 | 无（首启用默认尺寸） |

**环境变量**：`MODEL_ID` — 模型名（缺省用上表 `defaultModel`）。

## 构建

依赖：Windows + Visual Studio 2022（MSVC）+ Qt 6.9.0（`C:\Qt\6.9.0\msvc2022_64`）+ CMake 3.20+

```powershell
# 1. 拉取必需子模块（FluentUI）
git submodule update --init 3rdparty/FluentUI

# 2. 配置 + 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64
cmake --build build --config Release
```

输出：`build/bin/lite-harness.exe`（版本 s18.5）。源文件与 QSS 由 CMake `GLOB CONFIGURE_DEPENDS` 自动收集，新增文件重跑 configure 即可。

构建**必须全目标**（勿加 `--target lite-harness`）：FluentUI 子项目的 install 规则在 configure 期即注册进全树安装清单，单目标构建缺 `Gallery.exe`/`cmark.exe` 会让后续 cpack 硬错误中止。

### 单元测试（ctest）

纯函数单测，`LITE_TESTS` 默认 ON，产物落 `build/tests/`（与 `bin/` 分开，不进发布包）：

```powershell
cmake --build build --config Release --target lite-harness-tests
$env:LITE_TEST_TMPROOT = "$PWD\build\tmp-test"   # 必须以 tmp-test 结尾，否则带夹具门的套件走 SKIP
ctest --test-dir build -C Release --output-on-failure
```

需 Qt bin 在 PATH（供 `Qt6Core.dll`）。新增套件：写 `tests/tst_<模块>.cpp` 暴露 `int tst_<模块>()`，在 `tests/main.cpp` 加一行调用即可，**无需改 CMakeLists**（`file(GLOB test_src CONFIGURE_DEPENDS)` 自动收录）。当前覆盖三十八个套件（`LineEnding` 行尾口径 / `MessageBus` 邮箱门 / `TaskStore` 租约 / `WorktreeManager` 工作树 / `AgentTeamsManager` 团队协议 / `McpManager` 命名净化与装配门 / `AgentRecovery` s15 契约串与判据矩阵 / `BackgroundTasksManager` 台账与唤醒判据 / `WorkflowManager` s16 引擎纯函数与 journal/schema 门 / `WorkflowRuntime` s16 编排原语与 review-changes 端到端 / `CompactTokens` 会话 token 计量与增量恒等式 / `SettingsCache` 设置读缓存与文件身份失效判据 / `UsageTokens` token 消耗累计与锚定恒等式 / `StreamRate` 生成窗口、聚合口径与本轮归属 / `GoalManager` s17 goal 引擎六门解析与七决策记账判据 / `UiScale` 字号倍率与 DPI 策略名与几何编解码判据 / `CronManager` s12 cron 引擎报错族与 Vixie 双限定语义 / `LitePaths` 运行时路径锚点单源 / `PathGuard` 沙箱围栏多根判定 / `FluConfigStore` FluentUI 配置三段定位 / `FluConfigUtils` 配置门面注入与后端重建往返 / `ConfigMerge` 宿主与 FluentUI 配置合流不变量 / `SessionStore` 会话索引并入 `app/sessions` 的读写与迁移 / `UiUX` P1 UI 判据 / `ToolTagKind` 工具名→标签类别映射 / `ToolNames` 工具名常量表契约 / `NavKeys` 导航键与堆叠页键同源 / `LayoutConst` 布局常量自洽 / `AgentConst` 数值域与键名契约（含 s17.9 的四条预算 identity 断言）/ `CompactTranscript` 压缩转写回溯 / `ActivityLedger` s17.8 会话活动账本 / `HeatmapGrid` 热力图网格算术（371 满矩形、末格恒为 today、`cellOf`↔`dateOfCell` 互逆、窗口外返 {-1,-1}）/ `MemoryRules` 记忆判据（frontmatter 五条回落 / slug / 三重去重 / 写读往返）/ `ToolSchema` 工具 schema（27 项与 `ToolNames` 恰好重合 / `required` ⊆ `properties` / 数组与字符串专属关键字不串挂）/ `SessionKeys` 会话键算术（两族键不相交的真实理由是**长度**而非字母表）/ `ProjectInstructions` 项目指令两级候选（工程根在前、命中即止、超预算截断）/ `UpdateRules` 自更新版本判据（**数值段比较**，字典序在补丁位上恰好反向）/ `ContextTrim` 上下文成组裁剪（assistant 带 `tool_calls` 必须连同其后连续 tool 结果整组删，孤立删 tool 即上游 400）。**全量实跑 2605 条断言**（fail=0 skip=1，带 `LITE_TEST_TMPROOT` 夹具）/ **1594 条**（fail=0 skip=35，无夹具，与 `kSuiteFloors` 求和逐条自洽）；上一登记值 2251 为 s17.9 三批抽头批现值、历史保留。**静态断言点的口径与逐批历史链以 [AGENTS.md](AGENTS.md)「测试」节为单源**，本文不复述其数值以免两处漂移：口径 = 仓库净口径行首三合一（限定名 `TestHarness::check` 扣除 3 行 `#define CHECK` 续体 + 裸 `check(` + 宏 `CHECK(`），复算须大小写敏感、行首锚定并**逐文件求和交叉核对**（曾单遍管道计数得 179 与逐文件求和 183 不一致，误把它归因为「`git grep -E` 的 `\s` 不可靠」——该归因不可复现，`-E` 与 `-P` 实测同值，已删去）。

**本地跑必须先设 `LITE_TEST_TMPROOT`**（见上，值必须以 `tmp-test` 结尾）：十三个套件依赖 `ScopedTempRoot` 夹具（`MessageBus` / `TaskStore` 租约 / `Worktree` / `AgentTeams` / `SettingsCache` / `WorkflowManager` / `WorkflowRuntime` / `FluConfigStore` / `FluConfigUtils` / `ConfigMerge` / `SessionStore` / `UiUX` / `ActivityLedger`），变量缺失或未以 `tmp-test` 结尾时按各套件口径走 SKIP 通道（`MessageBus` / `Worktree` / `WorkflowRuntime` / `FluConfigStore` 等整组 SKIP；`WorkflowManager` 一类刻意「纯在前、文件在后」，只跳文件组、纯组断言照常计数），而 **SKIP 不计失败、进程仍返回 0**——`ctest` 看起来是绿的，实际只跑了非 SKIP 的组。CI 由 workflow 注入该变量，并额外用 `LITE_TEST_MIN_PASS` 断言下界把「跑到了多少断言」变成硬门禁。覆盖面与下一批切口见 [AGENTS.md](AGENTS.md)「测试（tests/ + ctest）」节。

### 手工回归

历轮验收场景沉淀为冒烟清单 [`docs/doc.md` 第三部分](docs/doc.md#checklist)：按改动面选组（P0 必跑），提交前执行对应组。

### 打包发布 ZIP

先完成 Release 构建，再经 CPack 出可分发压缩包（自动部署 Qt 与 VC 运行时，无需安装 Qt）：

```powershell
cpack --config build/CPackConfig.cmake -B build
```

产物：`build/lite-harness-s18.5-win64.zip`（约 54MB），解压后运行 `lite-harness-s18.5-win64/bin/lite-harness.exe` 即可。

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `src/` | 全部 C++ 源码（扁平目录、无子目录，分层靠 include 纪律）：Agent 核心（AgentLoop 家族 17 个 TU / QOpenAi / TaskStore / CompactManager / MemoryManager / CronScheduler / MessageBus / WorktreeManager / AgentTeamsManager / TeammateRuntime / WorkflowManager / WorkflowRuntime / GoalManager（s17 引擎群）…）+ FluentUI 页面与控件 |
| `tests/` | ctest 纯函数单测（`TestHarness.h` 极简断言骨架 + `tst_<模块>.cpp`），产物 `build/tests/`，不进发布包 |
| `stylesheet/` | 三主题 QSS（light / dark / atomOneDark），经 qrc 打包 |
| `i18n/` | 翻译源 `lite-harness_en_US.ts`（中文源→英文译文）与 `lite-harness_zh_CN.ts`（同文镜像，供 Linguist 审计），构建期 lrelease 内嵌 |
| `scripts/` | 仓库维护脚本：`update-i18n.ps1`（成对刷新两份 `.ts`，勿跑 `lupdate` 目标） |
| `.github/workflows/` | CI `Windows-Qt6.9.0.yml`：干净环境全量 Release 构建 + ctest + cpack 验收；`v*`/`s*` tag 末尾把 zip 上传为该 tag 的 GitHub Release |
| `3rdparty/FluentUI` | UI 框架子模块（构建必需） |
| `3rdparty/lcc` | 移植规格参考仓库子模块（不参与构建） |
| `res/` | 应用图标（`LiteHarness.ico`）；Windows 资源脚本 `LiteHarness.rc` 在仓库根，不在本目录 |
| `docs/` | 单一文档 `doc.md`（五部分）：第一部分索引与同步纪律 / 第二部分结构总览 / 第三部分冒烟清单 / 第四部分交互设计原理 / 第五部分异步链现状契约 |

## 文档

- [AGENTS.md](AGENTS.md) — **规则与坑**：构建/测试/数据路径/主题/i18n/关键约定/发布（面向开发代理，改约定须同步）
文档已合并为**单文件** [docs/doc.md](docs/doc.md)（五部分，按权威等级排序）。各部分与用途：

- [第一部分 · 文档索引与同步纪律](docs/doc.md#index) — 各部分权威等级、「该读哪篇」速查表、冲突优先级与同步纪律
- [第二部分 · 模块结构总览](docs/doc.md#architecture) — **结构与依赖边**：六层清单、AgentLoop 家族 TU 职责地图、一条消息的完整数据流、运行时目录布局、单源纪律一览、新增代码落位决策树（活文档，改结构须同步）
- [第三部分 · 手工冒烟回归清单](docs/doc.md#checklist) — 十一组 P0/P1/P2，发版必跑（活文档）
- [第四部分 · 交互 UI 设计逻辑](docs/doc.md#ui) — 折叠式渐进披露的设计原理（活文档）
- [第五部分 · 异步链现状契约](docs/doc.md#async) — memory/compact 侧链的现行契约：全异步不变量、`AsyncRequest` 语义、`stop()` 收口顺序（活文档，改侧链契约须同步；原实施规格正文已删，历史决策与偏离登记见其 §7）

## 发布

版本单源 = CMake `project VERSION`（数字段 `18.5`）；展示名、zip 名、tag 名统一加 `s` 前缀（`s18.5`）自动同名。补丁位 `.13` 沿用 `s13.1`/`s16.1`/`s17.1`/`s17.2` 先例（`s17.13.1` 是阶段 s17 内第十三个发布版、带第三段修订位）；`s18` 起版本主段是 lite-harness 自有发布阶段（lcc 参照阶段到 `s17` 为止）；s18 为单段形态，s18.1 起启用第二段补丁位（阶段内第一个补丁发布版）。补丁位只表发布次序、不要求连号：`s17.4` 经用户裁决跳过且后续不再启用。升版本必须 `git grep` 同步文档里写死的示例版本号；打 tag 即公开发布、实质不可撤回；**附注 tag 的正文就是 GitHub Release 简介的单源**。完整规矩见 [AGENTS.md](AGENTS.md)「发布（tag 与 Release）」节。

## 致谢

- [FluentUI](https://github.com/mowangshuying/FluentUI) — Qt Fluent Design 控件库
