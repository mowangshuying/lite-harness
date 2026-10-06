# Lite-Harness

用 Qt6 (C++) 编写的桌面 AI 编码代理 harness —— 单可执行文件，内置 LLM 工具主循环、会话管理、记忆与定时任务。

## 功能

- **多会话**：新建 / 重命名 / 删除 / 重启恢复；全局索引 `<workDir>/.lite-harness/index.json` + 每会话 history 持久化
- **工具主循环（18 工具）**：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / 任务图 6 工具（create/update/list/get/claim/complete_task）/ cron 3 工具（schedule_cron / list_crons / cancel_cron）
- **子代理**：`task` 工具派发独立上下文子代理，黑盒回传结果
- **权限门**：内联审批卡（拒绝为默认焦点），bash 危险命令硬拒绝表 + 工作区外写入 ASK 规则
- **流式渲染**：打字机气泡 + 思考块 / 工具执行块折叠动画 + Todo 进度卡
- **会话侧栏**：标题 / 模型 / 工作目录 / 上下文占用条 / 状态灯 / 待办清单；可收起，偏好持久化后由浮动钮恢复
- **记忆**：会话结束自动沉淀、下轮召回注入上下文（以 `<agent_context>` 注入块随请求尾部下发，system 全程字节恒定）、超阈值整理（Markdown 存储）
- **上下文压缩**：五段管线 + 溢出反应式压缩，阈值按 token 预算判定（以 `usage.prompt_tokens` 锚定估算占用），转录与超大工具输出落盘
- **cron 定时任务**：5 段表达式、JSON 持久账本、at-least-once 投递
- **后台 bash**：`run_in_background` 异步执行，结果以通知注入后续回合
- **三主题**：light / dark / atomOneDark，QSS 资源打包、运行时热切换
- **中英文界面**：中文源文本 + Qt Linguist 英文译文（`i18n/`），设置页切换、重启生效；译文内嵌单 exe，FluentUI 官方中文随库资源
- **模型选择**：下拉切换（生效清单与缺省项由 `settings.ini` 键 `modelOptions` / `defaultModel` 决定，未配置或非法则回落内置 `kBuiltinModelOptions`），API 经设置页配置

## 运行时配置

用户配置单源为 **exe 同目录 `settings.ini`**（`AppSettings.h`，QSettings IniFormat；弃用注册表）。设置页可编辑大部分键，其余手改 ini 即可（非法值由各消费点校验后回退默认）：

| 键 | 含义 | 默认 |
| --- | --- | --- |
| `apiBaseUrl` | OpenAI 兼容 API 基地址（http/https） | — |
| `apiToken` | API token（设置页值区脱敏显示；明文存本机 ini） | — |
| `modelOptions` | 模型下拉清单，逗号分隔（裸串与列表两种形态都能读回） | 内置 `kBuiltinModelOptions` |
| `defaultModel` | 缺省选中模型 | 清单首项 |
| `defaultWorkDir` | 新建会话的默认工作目录 | — |
| `contextCharLimit` | 上下文压缩主上限（字符，校验界 10000~5000000；token 预算由此换算） | 200000 |
| `maxToolIterations` | 单轮最大工具调用次数（校验界 10~1000；子代理预算同源） | 500 |
| `maxRetries` | 429/5xx 指数退避重试上限（校验界 0~5） | 2 |
| `language` | 界面语言（`zh-CN` / `en-US`，切换需重启） | `zh-CN` |
| `sidebarVisible` | 会话侧栏显隐偏好 | 显示 |

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

输出：`build/bin/lite-harness.exe`（版本 s12.6）。源文件与 QSS 由 CMake `GLOB CONFIGURE_DEPENDS` 自动收集，新增文件重跑 configure 即可。

构建**必须全目标**（勿加 `--target lite-harness`）：FluentUI 子项目的 install 规则在 configure 期即注册进全树安装清单，单目标构建缺 `Gallery.exe`/`cmark.exe` 会让后续 cpack 硬错误中止。

### 单元测试（ctest）

纯函数单测，`LITE_TESTS` 默认 ON，产物落 `build/tests/`（与 `bin/` 分开，不进发布包）：

```powershell
cmake --build build --config Release --target lite-harness-tests
ctest --test-dir build -C Release --output-on-failure
```

需 Qt bin 在 PATH（供 `Qt6Core.dll`）。新增套件：写 `tests/tst_<模块>.cpp` 暴露 `int tst_<模块>()`，在 `tests/main.cpp` 加一行调用即可，**无需改 CMakeLists**。当前只覆盖 `LineEnding.h`，覆盖面与下一批切口见 [AGENTS.md](AGENTS.md)「测试（tests/ + ctest）」节。

### 手工回归

历轮验收场景沉淀为冒烟清单 [`docs/doc.md` 第三部分](docs/doc.md#checklist)：按改动面选组（P0 必跑），提交前执行对应组。

### 打包发布 ZIP

先完成 Release 构建，再经 CPack 出可分发压缩包（自动部署 Qt 与 VC 运行时，无需安装 Qt）：

```powershell
cpack --config build/CPackConfig.cmake -B build
```

产物：`build/lite-harness-s12.6-win64.zip`（约 54MB），解压后运行 `lite-harness-s12.6-win64/bin/lite-harness.exe` 即可。

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `src/` | 全部 C++ 源码（扁平目录、无子目录，分层靠 include 纪律）：Agent 核心（AgentLoop 家族 13 个 TU / QOpenAi / TaskStore / CompactManager / MemoryManager / CronScheduler…）+ FluentUI 页面与控件 |
| `tests/` | ctest 纯函数单测（`TestHarness.h` 极简断言骨架 + `tst_<模块>.cpp`），产物 `build/tests/`，不进发布包 |
| `stylesheet/` | 三主题 QSS（light / dark / atomOneDark），经 qrc 打包 |
| `i18n/` | 翻译源 `lite-harness_en_US.ts`（中文源→英文译文）与 `lite-harness_zh_CN.ts`（同文镜像，供 Linguist 审计），构建期 lrelease 内嵌 |
| `scripts/` | 仓库维护脚本：`update-i18n.ps1`（成对刷新两份 `.ts`，勿跑 `lupdate` 目标） |
| `cmake/` | 辅助 Find 模块 |
| `.github/workflows/` | CI `Windows-Qt6.9.0.yml`：干净环境全量 Release 构建 + ctest + cpack 验收；`v*`/`s*` tag 末尾把 zip 上传为该 tag 的 GitHub Release |
| `3rdparty/FluentUI` | UI 框架子模块（构建必需） |
| `3rdparty/lcc` | 移植规格参考仓库子模块（不参与构建） |
| `res/` | 应用图标与 Windows 资源脚本 |
| `docs/` | 单一文档 `doc.md`（六篇合一）：第一部分索引与同步纪律 / 第二部分结构总览 / 第三部分冒烟清单 / 第四部分交互设计原理 / 第五部分异步链规格（存档）/ 第六部分早期草图（已过时） |

## 文档

- [AGENTS.md](AGENTS.md) — **规则与坑**：构建/测试/数据路径/主题/i18n/关键约定/发布（面向开发代理，改约定须同步）
文档已合并为**单文件** [docs/doc.md](docs/doc.md)（六篇合一，按权威等级排序）。各部分与用途：

- [第一部分 · 文档索引与同步纪律](docs/doc.md#index) — 各部分权威等级、「该读哪篇」速查表、冲突优先级与同步纪律
- [第二部分 · 模块结构总览](docs/doc.md#architecture) — **结构与依赖边**：六层清单、AgentLoop 家族 TU 职责地图、一条消息的完整数据流、运行时目录布局、单源纪律一览、新增代码落位决策树（活文档，改结构须同步）
- [第三部分 · 手工冒烟回归清单](docs/doc.md#checklist) — 十组 P0/P1/P2，发版必跑（活文档）
- [第四部分 · 交互 UI 设计逻辑](docs/doc.md#ui) — 折叠式渐进披露的设计原理（活文档）
- [第五部分 · 异步链实施规格](docs/doc.md#async) — memory/compact 阻塞链异步化，已落地按定稿存档（正文行号锚点已失效，只读 §0/§8）
- [第六部分 · 早期会话页草图](docs/doc.md#sketch) — **已过时**，仅作设计演化留痕

## 发布

版本单源 = CMake `project VERSION`（数字段 `12.6`）；展示名、zip 名、tag 名统一加 `s` 前缀（`s12.6`）自动同名。升版本必须 `git grep` 同步文档里写死的示例版本号；打 tag 即公开发布、实质不可撤回；**附注 tag 的正文就是 GitHub Release 简介的单源**。完整规矩见 [AGENTS.md](AGENTS.md)「发布（tag 与 Release）」节。

## 致谢

- [FluentUI](https://github.com/mowangshuying/FluentUI) — Qt Fluent Design 控件库
