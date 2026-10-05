# 手工冒烟回归清单

> 沉淀历轮验收场景为可重复执行的手工回归清单。自动化测试仅覆盖 `tests/`（当前 `LineEnding.h` 行尾口径，`ctest --test-dir build -C Release --output-on-failure` 跑），无 lint；其余仍靠构建+运行+目测验收。

> **用法**：按改动面选组执行——P0 每次 src/ 改动必跑；P1 按受影响域跑；P2 里程碑/发版前抽查。
> **基线环境**：Qt 6.9.0 msvc2022_64 + MSVC 2022 + CMake，仅 Windows；模型服务凭 `settings.ini` 键 `apiBaseUrl` / `apiToken`（主窗口构造期 `QOpenAi::initFromSettings()` 读取），模型下拉取 `modelOptions` / `defaultModel`（内置首项 `qwen3.8-flash`）。
> **配置存储**：exe 同目录 `settings.ini`（`AppSettings::ini()` 单源，QSettings IniFormat）——注册表方案已随用户裁决废弃，禁再引入默认构造 `QSettings`。下文 `S` 指上下文上限字符数（settings.ini `contextCharLimit`）；`T` 指派生 token 预算（`contextTokenBudget()` ≈ S/4，触发与 UI 均此口径）；`T′` 指 T 扣除 system+tools+注入 overhead 后的会话体预算。

## 一、P0 启动与构建

- [ ] 子模块前置 `git submodule update --init 3rdparty/FluentUI` → checkout 成功，configure 不再缺 `FluentUI::Controls`（`3rdparty/lcc`/`sqlite` 从不入构建图，无需 init）
- [ ] 干净 configure `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64` → 零报错，GLOB 拾取全部 `src/*.cpp|.h` 与 `stylesheet/**/*.qss`（新增源文件重 configure 自动拾取、不改 CMakeLists.txt）
- [ ] Debug 构建 `cmake --build build --config Debug --target lite-harness` → 0 error；src/ 各编译单元 `/W4` 0 告警（FluentUI 第三方豁免）
- [ ] 重链前先结束所有运行中的 `build/bin/lite-harness.exe`（含用户自开实例）→ 链接成功；若见 LNK1168 属已知坑（Debug/Release 共用 `build/bin/` 互相覆盖），非代码缺陷
- [ ] 双击 `build/bin/lite-harness.exe` → 主窗口存活 ≥10 秒无崩溃，标题栏（32px 手绘）与导航列渲染正常
- [ ] `settings.ini` 未配 `apiBaseUrl` / `apiToken` 启动 → 窗口正常显示、构造期 qWarning 提示未配置，仅在发起对话时以回合内错误文案体现（不闪退）

## 二、P0 会话生命周期

- [ ] 新建对话页发起 → 导航「会话」下新增子项并进入对应会话页；标题为空显示「新会话」，超 12 字符截断加「...」
- [ ] 检查 `<进程当前目录>/.lite-harness/index.json` → 条目含 dataId/title/model/workDir/时间戳；QSaveFile 原子写（异常退出不留半文件）。注意：索引根固定为进程当前目录，`workDir` 只是条目字段
- [ ] 会话内发若干消息 → `<会话数据根>/.lite-harness/sessions/<dataId>/history.json` 存在且与可见消息一致（dataId 为 8 位 hex）
- [ ] 重启程序 → 会话按 createdMs 升序恢复、历史气泡回放完整；恢复不自动切换当前页（仍停在新对话页）；workDir 失效时静默回退进程当前目录
- [ ] 右键会话导航子项 → 仅弹「重命名 / 删除会话」菜单；单击子项不因基类 itemClicked 误翻页（eventFilter 吞 press/release）
- [ ] 重命名：输入新名确认 → 导航与索引标题同步、刷新 lastActiveMs；取消/空/未变 → 保持原值
- [ ] 删除：弹「此操作不可撤销」确认 → 确认后页面销毁、索引条目移除、`sessions/<id>/` 整目录递归清理；删的若是当前页则切回新对话页；运行中会话先 `stop()` 再删；数据清理失败仅 qWarning、UI 状态照常收清
- [ ] 有会话运行中点窗口关闭 → 弹「任务仍在运行，退出将丢失未完成回合。确定退出吗？」且默认焦点「否」；选「否」→ 不退出、任务继续；选「是」→ 逐页 `stop()` 后退出

## 三、P0 Agent 主循环与权限门

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
- [ ] 任意工具失败（read_file 不存在的文件等）→ 仅折叠为 tool_result 字符串交还 LLM，全程不抛异常、不弹窗
- [ ] 子代理（`task`）内 bash 失败同样可判定 → 非零退出码回喂带 `Error: command exited with status N:` 前缀（与主循环前台/后台共用 `formatBashResult`）；PowerShell 起不来由 `errorOccurred` 显式收口回 `Error: bash 启动失败…`；不得把失败洗白成裸输出或 `(no output)`（曾如此）；取消后仍静默丢弃输出、`onToolFinished` 恒一次
- [ ] `edit_file` 行尾两级匹配 → CRLF 文件上用**从 `read_file` 输出抄来的 LF 多行** `old_string` 能命中（曾必然报 text not found）；写回后全文件无裸 LF（不产出混合行尾）；LF 文件保持 LF 不被转成 CRLF；单行替换与「只替换第一处」语义不退化；混合行尾文件走 LF 归一化回退、写回按主导行尾归一
- [ ] `edit_file` 编码防线 → 非 UTF-8 文件（如 GBK 源文件）返回可判定错误拒绝编辑，**不得**写回把非法字节永久替换为 U+FFFD；带 UTF-8 BOM 的文件既能正常编辑（`fromUtf8` 会吃 BOM，守卫须按去 BOM 后的正文比较，否则误拒）、写回后 BOM 原样保留（不被静默删除）
- [ ] `edit_file` 三道防线 → ① 空 `old_string` 返回可判定错误拒绝编辑（**不得**把 `new_string` 静默前插到文件开头）；② 超过 `kEditFileMaxBytes`（5MB）的文件拒绝编辑并回体量错误（**不得**整文件读入冻结主线程，也不得部分读写毁文件）；③ `old_string` 在文件中命中多处 → 返回「命中 N 处，拒绝编辑（请补充上下文使其唯一）」，**不得**静默替换第一处（有意偏离 lcc `str.replace(old,new,1)` 语义）
- [ ] 单元测试 → `ctest --test-dir build -C Release --output-on-failure` 全绿（产物 `build/tests/lite-harness-tests.exe`，需 Qt bin 在 PATH 供 `Qt6Core.dll`）；测试 exe **不进** CPack 包（输出目录与 `bin/` 分开，staging 只取 `bin/`）
- [ ] `write_file` 行尾保真 → 覆盖已存在的 CRLF 文件时沿用 CRLF
（模型给的 content 天然是 LF，直写会翻转整文件行尾、产出全文件 diff 噪声）；新建文件按 content 原样落盘不臆造行尾；主导行尾只读开头 `kEndingProbeBytes`（64KB）窗口判定，已知边界：窗口内无换行的超长单行文件判为 LF

## 四、P1 设置与数据路径

- [ ] 设置页「默认工作目录」：选新目录 → `settings.ini` `defaultWorkDir` 更新；对话框取消 → 保持原值；清除 → 展示「未设置（使用进程当前目录）」
- [ ] 设置页「上下文上限（字符）」改为 300000 → 落盘即下一回合压缩管线现取生效（无需重启）；卡片展示千分位（QLocale::c() 固定，不随界面语言变），数值行带派生提示「≈N token」（= 字符上限/4，仅展示、键语义仍是字符）；侧栏占用同为 ≈token 口径（contextCharLimit/4 派生预算，非字符）
- [ ] 上下文上限输入 9999 / 5000001 / 非数字 → 「无效数值」弹窗不落盘；输入带千分位的 `200,000` → 剥逗号解析成功；合法界 10000~5000000、默认 200000
- [ ] 键位巡检 → `language` / `defaultWorkDir` / `sidebarVisible` / `contextCharLimit` / `maxToolIterations` / `maxRetries` / `apiBaseUrl` / `apiToken` / `modelOptions` / `defaultModel` 均落于 exe 同目录 `settings.ini`（手工编辑重启即生效，注册表路径不再被读写）
- [ ] 模型清单可配置：`settings.ini` 写 `modelOptions=qwen-a,qwen-b`（裸逗号串、不带引号）→ 下拉恰列两项且顺序一致；手改首项为 `qwen3.8-flash,qwen3.8-max` 读侧解析成功（`iniTextValue()` 归一化列表串，回归点：曾误判「未配置」静默回退内置清单）；清单为空/全空白 → 回退内置两项；条数超 32 截断
- [ ] `defaultModel` 指向清单内模型 → 生效；指向清单外或留空 → 回落清单首项（防配置指向不存在的模型）
- [ ] 设置页改「模型服务」端点/密钥 → 写 ini 后立即 `QOpenAi::setUrl()` / `setToken()`，下一回合请求即用新值（无需重启）；密钥卡默认掩码、点眼睛切明文且焦点不跳字
- [ ] 会话数据目录拼法目测+读码（`AgentConstants.h`）→ `.task` / `.temp` / `.transcripts` / `.task_outputs/tool-results` / `.memory` 零偏差（拼法涉数据兼容，改名即回归）；任务图每任务一个 `.task/task_<hex8>.json`

## 五、P1 异步链与压缩（禁回退阻塞的验证点）

- [ ] 新回合首条消息 → 记忆召回在开聊前**异步**注入（等待期 UI 不冻结、无嵌套事件循环），注入请求 payload 尾部独立消息（`<agent_context>` user，不落 history.json），system 全会话字节恒定（前缀缓存锚点）；召回飞行中 cron 交付被 `m_running` 卫兵拒投不插队
- [ ] 压缩三路径各触发一次（超限自动 / `compact` 工具 / 溢出反应式）→ 全程滚动气泡、切主题、切页均响应
- [ ] 超大工具输出（token 估算 >0.6T′）→ 卸载落 `.task_outputs/tool-results/`，历史内仅保留约 2000 字符预览（预览仍字符域）
- [ ] 压缩完成后查盘 → 原转录落 `.transcripts/*.jsonl`；压缩后历史体量 ≤0.8S；派生阈值读码 CompactManager 等比 batch=4S / large=0.6S / summary=1.6S（勿写死绝对值）；snip 迟滞 60/50（>60 条**且**过 token 闸门才归档、归档后总量 ≈50 条，10 条迟滞带内不重复 snip）、归档只追加落固定名 `.transcripts/snip_archive.jsonl`（非全量重写）、会话内归档标记为恒定文本（无条数/无路径，缓存友好）
- [ ] 人为制造上下文超限错误 → 服务端 4xx 响应体原文并入 error 文本（错误气泡可见 `context_length_exceeded` 等，不再恒为 Unknown error）；溢出关键词表单源 `isContextOverflowError`；反应式压缩自动重试恰 1 次（预算每 run 归零、成功收响应即复位）后重发
- [ ] 人为制造 429 / 5xx → 指数退避重试（1s,2s,4s…，次数上限 = `settings.ini` 键 `maxRetries`，默认 2、校验界 0~5，越界/非整数回退默认）；重试耗尽的错误文本含服务端响应体解析结果（`error.message [+ code]`，非 JSON 体截 400 字节），不再恒为 Unknown error；重试从头清空累积状态（buffer/thinking/content/toolCalls/usage）
- [ ] 4xx（除 429）仍硬错误不重试 → `context_length_exceeded` 反应式压缩路径不受重试改动影响；可重试判定位于 4xx 分支**之前**（顺序颠倒会让 429 落进硬错误、退避链路成死代码，回归点）
- [ ] 会话自然结束后 → 记忆沉淀 fire-and-forget 挂回合尾巴，不阻塞下一回合输入

## 六、P1 主题与导航样式

- [ ] 设置页轮换三主题 light / dark / atomOneDark → 全窗重绘、无旧主题残色
- [ ] 底色基准取色 → 导航列/内容页/标题栏透色区：light `rgb(249,249,249)`、dark `rgb(40,40,40)`、atomOneDark `rgb(40,44,52)`
- [ ] 导航顶边 border-top 与内容页边框同色基准 → light `rgb(229,229,229)`、dark `rgb(56,56,56)`、atomOneDark `rgb(56,60,68)`；交界处无断色
- [ ] 内容页圆角 → 左上角直角、其余角 10px
- [ ] 滚动条槽色随主题（`LiteHarnessScrollBarAlign.qss` 经 `qproperty-trunkBackgroundColor`）→ 深色主题下 FluScrollBar 槽不再是 FluentUI 默认色
- [ ] 标题栏底色 → StandardTitleBar 为 paintEvent 手绘不吃 QSS，靠置背景透明透出窗口本体色；light 前景黑、其余白，高度 32px
- [ ] 覆盖机制读码警示 → 覆盖 FluentUI 取色只准 `appendOwnSheetOverride` 往**控件自身样式表**幂等追加（marker `/*lh-nav-align*/` 截旧防增长）；若出现窗口级复合选择器写法即回归（像素实证无效）；`ThemeAware::bind` extraRefresh 的「首刷同步 + `singleShot(0)` 重放」双保险仍在（抵消 FluentUI 批处理重写）

## 七、P2 i18n

- [ ] `settings.ini` 无 `language` 键 → UI 默认中文（中文为源语言，代码一律 `tr("中文")`）
- [ ] 设置页语言切「English」→ 弹「语言切换将在重启后生效。是否立即重启?」；允许 → `exit(931)` + `startDetached` 自重启，重启后**有且仅有一个**实例（main 对 rc==931 只透传，严禁二次拉起=双启缺陷回归点）
- [ ] 重启确认框点取消 → 不重启（close 复用退出守卫）；语言已落 `settings.ini`，下次启动生效
- [ ] 切换后常驻组件重译 → 导航三项 / 设置页 / 新对话页 / WorkDirPathBar / TodoCard 标题取新语言（`changeEvent(LanguageChange)`）；已渲染历史气泡滞留旧语言（已知接受偏差，勿判 bug）
- [ ] Qt 标准对话框（QFileDialog/QMessageBox 按钮）→ 恒为英文，qtbase 中文 qm 已按裁决摘除，属已知取舍勿误报漏译
- [ ] 新增/修改 UI 串流程 → 仓库根跑 `scripts/update-i18n.ps1` 成对刷 `i18n/*.ts` 双文件后构建通过；**严禁**跑 `lite-harness_lupdate` 陷阱 target（会扫 FluentUI 灌入上千外部串）；发往 LLM 的 C 类串（system prompt、`(恢复：工具结果不可用)` 等落盘文本）保持 `QStringLiteral` 不被包 `tr()`
- [ ] 版本号单源 → 设置页「关于」版本 = CMake `project VERSION`（12.5）经 `LITE_VERSION` 宏拼 `s` 前缀运行时注入（显示 `s12.5`，与阶段 tag 同名），全仓无散落硬编码；侧栏页脚 `lite-harness s12.5` 不带多余 `v`

## 八、P2 记忆与子代理

- [ ] 会话自然结束后查 `.memory/` → MEMORY.md 索引 + slug.md 记录新增；强续跑 / 撞调用上限分支**不**触发沉淀；仅 persistent scope 记录且过三重去重
- [ ] 召回 LLM 选择失败 → 关键词打分兜底，兜底记录仍出现在 payload 尾部注入块（不落 history.json，勿回退为注入 system）
- [ ] 记忆整理阈值触发 → 卡片「已整理记忆：%1 → %2 条」；重写中途失败 → 快照回滚原记录完好（无半成品态）；库过大 skip/skipped 降级不崩
- [ ] `task` 子代理 → 全新上下文（不带主会话历史）、黑盒只回最终文本（中间过程不涌入主流气泡）
- [ ] 子代理轮次预算与主循环同源（`start()` 入口快照 `AgentConst::maxToolIterationsValue()` 进 `m_maxTurns`，原固定 `kMaxSubagentTurns = 50` 已删）→ 改「单轮最大调用次数」联动子代理，但单次运行中不随设置变动；预算耗尽以停跑文案收尾
- [ ] 子代理工具白名单 → 仅 read/write/edit/glob + bash 异步；todo_write / task / cron 族等主循环专属工具不可见；其权限询问经同一 permissionRequired 透明转发宿主（同时至多一方待决）

## 九、P2 后台任务与 cron

- [ ] bash 带 `run_in_background: true`（**严格布尔**，字符串 "true"/1 等不算）→ 台账 `bg_0001` 起自增、立即返回不阻塞回合；完成后结果以 `<task_notification>…</task_notification>` 注入下一回合（result 取前 500 字符、无截断标记）
- [ ] cron 5 段表达式校验 → 越界段拒绝（分 0-59 / 时 0-23 / 日 1-31 / 月 1-12 / 周 0-6）；支持 `*`、`*/N`、`a,b,c`、`a-b`；逗号列表全不中判 false（勿回退成默认命中）；日与周双限定为 OR（Vixie 语义）
- [ ] schedule_cron 持久 → `<会话数据根>/scheduled_tasks.json` 原子写；重启后 pending 一次性任务照常重投（装载不剪枝）；id 形如 `cron_<hex8>`
- [ ] at-least-once 两段投递：回合成功 → recurring 清 pending 留表、one-shot 移除；回合失败/停止 → 回队，下一 1s tick 立即重试（不清 lastFired、不等整分钟）
- [ ] 同一分钟去重 → pollDueJobs 同分钟 marker 命中不重复触发；list_crons 输出每行 `%1: %2 -> %3 [%4, %5]`（prompt 截前 60 字符，空表回 `No cron jobs.`）
- [ ] 轮询读码 → 1s QTimer 由宿主 AgentLoop 驱动（零线程）；`stop()` 不杀 cron：调度器存续、交付暂停于 `m_running` 卫兵、空闲后续投；已知偏差：多会话页同 workDir 各持台账可能双触发（登记勿修）

## 十、P2 打包部署与 CI

- [ ] Release 全量构建 `cmake --build build --config Release` → 0 error（FluentUI 首编 >15 分钟，设足超时）
- [ ] 打包 `cpack --config build/CPackConfig.cmake -B build` → 出 `build/lite-harness-s<版本>-win64.zip`（约 54MB/75 条目）；`--config` 必带，否则报 generator not specified；cpack 不触发编译，staging 取 `build/bin/` 当前 exe（RUNTIME_OUTPUT 配置无关）
- [ ] 包内布局 → 顶层目录内 `bin/` = exe + Qt6 运行时 + VC 运行库 + `qt.conf`（Prefix=..），根级 `plugins/` + `translations/`；无 FluentUI 泄漏物（`bin/Gallery.exe`、`include/`、`lib/`、`share/`、重复 Qt 运行时）——根级 `install(CODE)` 清理必跑在子目录规则之后
- [ ] 解压到干净机器冒烟 → `<顶层目录>/bin/lite-harness.exe` 可启动；配好 `settings.ini` 的 `apiBaseUrl` / `apiToken` 后可完整对话；本链默认携带 VC 运行库（windeployqt 未传 `--no-compiler-runtime`），故缺 Redist 起不来不再是预期边界
- [ ] push / PR → GitHub Actions `Windows-Qt6.9.0.yml` 触发干净环境全量 **Release** 构建 + `ctest` 单测 + cpack 打包，绿灯即验收；paths 白名单外（如本 docs 改动）不触发；`tests/**` 已在白名单内（改测试也触发验收）

- [ ] tag `v*` / `s*` 推送 → 同一 workflow 把 zip 上传为该 tag 的 GitHub Release 资产
- [ ] CI 零凭证读码 → workflow 内无任何 token/secret 硬编码或引用
- [ ] 旧就地 `deploy` 目标（windeployqt 自定义目标）已摘除，**勿恢复双轨**：CPack ZIP 是唯一部署/打包路径，不要再验 `--target deploy` 或往 `dist/` 拷产物
