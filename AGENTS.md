# AGENTS.md — lite-harness

Qt6 桌面 AI 编码代理 harness：内置 LLM 工具主循环、会话、记忆、定时任务的单可执行文件。

## 构建

- **工具链:** Qt 6.9.0 + MSVC 2022 + CMake 3.20+，C++17，仅 Windows
- **Prefix:** `CMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64`
- **构建目录:** `build/`（VS 解决方案 `build/lite-harness.sln`）
- **输出路径:** `build/bin/lite-harness.exe`（CMake VERSION 0.1.0）
- **编译选项:** MSVC `/W4 /utf-8`（无 `/WX`）；仅链 Qt6 Widgets/Svg/Network + FluentUI::Controls/Utils（find_package 另需 LinguistTools 组件供翻译生成）
- **无测试、无 lint 配置** — 通过构建和运行验证；**CI:** GitHub Actions `.github/workflows/Windows-Qt6.9.0.yml`（main 分支 push/PR 触发于源码 paths 清单，干净环境全量 **Release** 构建 + cpack 出 zip 即验收，不跑测试；首跑实测约 19.5 分钟量级。构建**必须全目标**（勿加 `--target lite-harness`）：FluentUI 子项目 install 规则 configure 期即注册进全树安装清单，单目标构建缺 gallery.exe/cmark.exe 会让 cpack `file(INSTALL)` 硬错误中止（s12.2 两条 run 实证），全量编出后由根级清理剥除。`v*`/`s*` tag 推送触发同一流水线并在末尾经 svenstaro/upload-release-action 把 zip 上传为该 tag 的 GitHub Release——官方语义：paths 过滤不拦 tag；`branches` 与 `tags` 必须显式同写，只写 tags 会静默丢掉分支验收流）
- **手工回归:** 历轮验收场景沉淀为冒烟清单 `docs/regression-checklist.md`（P0/P1/P2 按改动面选组，提交前跑对应组）
- **首次构建前置:** `git submodule update --init 3rdparty/FluentUI`（必需）。`3rdparty/lcc` 仅为移植规格参考、从不参与构建，init 可选；`3rdparty/sqlite_orm` 已从 .gitmodules 与索引 gitlink 清账移除（全仓零引用）。`3rdparty/sqlite`（vendored sqlite3）目录残留但同样从不进构建图。
- **打包 (CPack ZIP，唯一部署/打包路径):** `LITE_PACKAGE` 默认 ON。先构建出 exe，再 `cpack --config build/CPackConfig.cmake -B build`（`--config` 必带，否则报 generator not specified；cpack.exe 与 VS 自带 cmake 同目录）→ `build/lite-harness-0.1.0-win64.zip`（约 54MB/75 条目：顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + qt.conf，根级 `plugins/` + `translations/`）。机制与坑（均实证）：① `qt_generate_deploy_app_script` 生成的 windeployqt 命令固定 `--dir . --libdir bin`（多配置 Windows 下 QtDeploySupport 默认），故 exe 必须 `RUNTIME DESTINATION bin`，包内平铺布局不可行；windeployqt 自动写 `bin/qt.conf`（Prefix=..）令根级 plugins/translations 生效；此路默认携带 VC 运行库（windeployqt 无 `--no-compiler-runtime`）。② FluentUI 子项目在同一 staging 树注册了自己的 install 规则（include/lib/share、`bin/` 下 Gallery.exe 及重复 Qt 运行时）——根级 `install(CODE)` 整删垃圾目录 + 逐个删 `bin/` 内非 lite-harness.exe；CMake 子目录规则先于父目录规则执行，父级清理必跑最后。③ FluentUI 内部泄漏过一次 `include(CPack)`，根尾部后发 `include(CPack)` 覆盖生成 `build/CPackConfig.cmake` 才生效（FILE_NAME=lite-harness… 实证）；勿删根级 include 顺序。④ cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）；多配置 staging 默认按 Release 执行子规则。原就地 `deploy`（windeployqt 自定义目标）已随本链落地摘除，勿恢复双轨。CI 已接入本打包链：Windows-Qt6.9.0.yml 按 Release 构建后 `cpack --config build/CPackConfig.cmake -B build`，`v*`/`s*` tag 推送时 zip 自动上传为该 tag 的 GitHub Release。
- **链接坑:** Debug/Release 共用 `build/bin/` 输出目录互相覆盖；运行中的 lite-harness.exe（含用户自己开的实例）占文件导致 LNK1168，重链前先结束占用进程。FluentUI 的 Release 全量首编很慢（>15 分钟），设足超时。
- **源文件收集:** `file(GLOB CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")` + `GLOB_RECURSE "stylesheet/*.qss"`（经 `qt_add_resources` 打包为 `:/stylesheet/`）。**新增源文件/QSS 无需改 CMakeLists.txt**，重新 configure 即自动拾取。

## 架构（src/）

> 本节只给「模块是什么」的一句话定位；**分层依赖图、AgentLoop 家族 12 个 TU 的职责地图、
> `AgentLoopDetail` 内部工具归属、一条消息的完整数据流与回填总表、运行时目录布局、已知分层
> 异常与技术债、新增代码落位决策树**见 [docs/architecture.md](docs/architecture.md)——改结构须同步它。

### Agent 核心链

- **AgentLoop** — LLM 主循环 + 18 工具分发（名单唯一来源 `ToolNames.h`）：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / create_task / update_task / list_tasks / get_task / claim_task / complete_task / schedule_cron / list_crons / cancel_cron。权限门（bash 硬拒绝表 + ASK 规则）与生命周期钩子（UserPromptSubmit/PreToolUse/PostToolUse/Stop）。任务图 6 工具与 cron 3 工具仅主循环注册。
- **QOpenAi** — OpenAI 兼容客户端：`ChatStream` SSE 流式（thinkingDelta/textDelta/messageFinished）+ `AsyncRequest` 一次性异步文本请求（复用 ChatStream，done 恒一次/取消永久静默/总超时兜底；全仓零嵌套事件循环，阻塞族已随异步化 P4 删除）。运行时配置来自 settings.ini 键 `apiBaseUrl` / `apiToken`（`initFromSettings()` 于主窗口构造调用；设置页「模型服务」分组可编辑，写后即时生效），模型名 `MODEL_ID`（仍走环境变量），缺省 `AgentConst::defaultModel()`（settings.ini 键 `defaultModel`，未配置/非法则取生效清单首项）。
- **TaskStore** — 任务图存储（lcc s10 移植）：每任务一个 `<会话根>/.task/task_<hex8>.json`，每次操作直读磁盘；6 个 run_* handler + 14 内核方法，失败一律折叠为工具输出字符串。
- **SubAgent** — `task` 工具子代理（lcc s06）：全新上下文、黑盒只回最终文本、轮次预算与主循环同源可设置（`maxToolIterations`，start 入口快照）；仅开放 read/write/edit/glob + bash 异步。
- **BashRunner** — bash 执行单源（危险检测 / 截断 / 超时终态 / QProcess 启动），AgentLoop 前后端与 SubAgent 共用。
- **BackgroundTasksManager** — 后台 bash 任务台账（lcc s11）：`run_in_background` 严格布尔判定，宿主驱动 QProcess，结果以 `<task_notification>` 注入下一回合。
- **CronSchedulerManager** — cron 定时任务（lcc s12）：5 段表达式校验/匹配，`<会话根>/scheduled_tasks.json` 持久账本，QTimer 1s 轮询替代线程，at-least-once 两段投递。
- **CompactManager** — 上下文压缩（lcc s08）：五段管线 toolResultBudget→snipCompact→microCompact→fitToolResults→compactHistory + 溢出反应式压缩；转录落 `<会话根>/.transcripts/*.jsonl`，超大工具输出卸载到 `.task_outputs/tool-results`。主字符上限可设置（见「数据与路径」），其余阈值按 4S/0.6S/1.6S/0.8S 等比派生。
- **MemoryManager** — 记忆（lcc s09）：`<会话根>/.memory/`（MEMORY.md 索引 + slug.md 记录）；沉淀（会话自然结束）、召回（LLM 选择 + 关键词兜底 → system prompt 尾部）、整理（阈值重写带快照回滚）。

### 会话层

- **SessionStore** — 纯静态工具：全局索引 `<workDir>/.lite-harness/index.json`（QSaveFile 原子写，条目 dataId/title/model/workDir/时间戳）。只管索引，history.json 归 AgentLoop。
- **SessionRegistry** — 导航 key ↔（page / dataId / 导航子项）反查表 + 右键监听映射；QPointer 观察、零所有权。

### 单源常量 / 纯头

- **AgentConstants.h** — 模型清单（settings.ini 键 `modelOptions` 逗号分隔、`defaultModel` 指定缺省项，未配置/非法回落内置 `kBuiltinModelOptions`；单点取值 `modelOptions()`/`defaultModel()`，**读值必须经本头内 `iniTextValue()`**——裸逗号串在 ini 是 QSettings 的列表语法，`value().toString()` 会得空串即「配置了却不显示」，列表形态要逐元素取原文再按逗号拆，严禁再裸用 `.toString()`）、kMaxTokens、bash 超时/错误文案、输出截断、上下文上限默认/校验界（kContextCharLimitDefault/Min/Max）与单点取值 `contextCharLimitValue()`、glob 上限与剪枝目录、数据目录名（`.task`/`.temp`/`.transcripts`/`.task_outputs/tool-results` — 拼法涉数据兼容，不可改；上下文上限数值则只是默认值语义，可被 settings.ini 覆盖，非硬约束）。
- **LayoutConstants.h** — 消息列宽/边距（NewChatPage/ChatSessionPage/ChatMsgEdit 同列对齐）。
- **NavItem.h** — 导航键常量（NavKey）+ NavItem（带 removeChildItem）。**ToolNames.h** — 18 工具名。**ToolTagKind.h** — 工具→样式标签（write/run/search/read/plan/delegate/other），经动态属性喂给 QSS 选择器。

### UI 层

- **LiteHarness** — 主窗口（FluFrameLessWidget）：FluVNavigationView + FluStackedLayout，会话新建/恢复（按索引升序）/重命名/删除，关闭时运行守卫。
- **ChatSessionPage** — 每会话一页：AgentLoop + 滚动消息流 + 输入框 + 只读路径条；历史回放与就地刷新。
- **NewChatPage / SettingsPage / BasePage** — 发起页（进入时重读 settings.ini 默认目录）、设置页（settings.ini `defaultWorkDir`、`contextCharLimit`、`apiBaseUrl`/`apiToken` 模型服务卡）、页面基类。
- **MessageBubbleWidget** — 气泡流式渲染（打字机），首次工具/思考事件后重建为时间线。
- **CollapsibleBlock** — 折叠动画基类（32px 头部 + 300ms OutCubic contentHeight 动画），子类 ThinkingBlock / ToolBlock / TodoCard。基类构造禁调虚函数，子类构造尾再 bind 主题。
- **ThemeAware** — 「加载 QSS + 订阅 themeChanged + 重载」样板单源（约 12 处旧复制已收敛）。
- **ChatMsgEdit / SendMsgButton / PermissionCard / WorkDirPathBar / FluentInputDialog** — 输入区、圆形 SVG 发送钮、内联审批卡（拒绝默认焦点、理由 EN→ZH）、工作目录条、通用单行输入对话框。

## 数据与路径

- 所有会话数据落在 **`<workDir>/.lite-harness/`**（`SessionStore::rootDirFor`）——是会话工作目录下的相对根，**不是**用户主目录。
- 带 sessionDataId 时隔离到 `sessions/<id>/`（history.json、.task、.memory、.transcripts、scheduled_tasks.json 等）；`skills/` 始终跨会话共享。
- 设置存储 = `AppSettings.h` 单源的 **exe 同目录 `settings.ini`**（QSettings IniFormat；键 `defaultWorkDir`/`contextCharLimit`/`maxToolIterations`/`language`/`sidebarVisible`/`apiBaseUrl`/`apiToken`/`modelOptions`/`defaultModel`；用户裁决弃用注册表）。`modelOptions` 手改写成裸逗号串时 QSettings 会解析成 QStringList（两种形态——手改裸串与设置页写单值——都要能读回，故读值走 `AgentConstants.h` 的 `iniTextValue()`，见上条）。
- 上下文压缩上限可设置（settings.ini 键 `contextCharLimit`，默认 200000 字符，校验界 10000~5000000；缺失/非法回退默认），派生阈值随主上限等比缩放（batch=4S、large=0.6S、summary=1.6S、压缩目标=0.8S）；设置页写值后压缩管线下一回合即生效，无需重启。
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
- **版本号单源:** CMake `project VERSION` → `LITE_VERSION` 宏 → App.cpp `setApplicationVersion` → 设置页标签取运行时值，不许散落硬编码。

## 关键约定

- 导航键统一取自 `NavItem.h::NavKey`；页面注册 `m_sLayout->addWidget(key, page)`，导航项用同一键。
- 魔法数字进 `AgentConstants.h`（agent 参数）或 `LayoutConstants.h`（布局尺寸），不散落字面量。
- 工具侧失败折叠为输出字符串交还 LLM，不抛异常、不弹窗。
- lcc 移植规格权威：源码注释以 `lcc sXX + hash` 标注对应阶段（s03–s12），参照仓库 `3rdparty/lcc`（不入构建）。
- FluentUI 头文件 `<FluUtils.h>`、`<FluThemeUtils.h>` 等位于 `3rdparty/FluentUI/{controls,utils}`。

## Git 提交
采用中文日志
