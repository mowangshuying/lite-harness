# Lite-Harness

用 Qt6 (C++) 编写的桌面 AI 编码代理 harness —— 单可执行文件，内置 LLM 工具主循环、会话管理、记忆与定时任务。

## 功能

- **多会话**：新建 / 重命名 / 删除 / 重启恢复；全局索引 `<workDir>/.lite-harness/index.json` + 每会话 history 持久化
- **工具主循环（18 工具）**：bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact / 任务图 6 工具（create/update/list/get/claim/complete_task）/ cron 3 工具（schedule_cron / list_crons / cancel_cron）
- **子代理**：`task` 工具派发独立上下文子代理，黑盒回传结果
- **权限门**：内联审批卡（拒绝为默认焦点），bash 危险命令硬拒绝表 + 工作区外写入 ASK 规则
- **流式渲染**：打字机气泡 + 思考块 / 工具执行块折叠动画 + Todo 进度卡
- **记忆**：会话结束自动沉淀、下轮召回注入 system prompt、超阈值整理（Markdown 存储）
- **上下文压缩**：五段管线 + 溢出反应式压缩，转录与超大工具输出落盘
- **cron 定时任务**：5 段表达式、JSON 持久账本、at-least-once 投递
- **后台 bash**：`run_in_background` 异步执行，结果以通知注入后续回合
- **三主题**：light / dark / atomOneDark，QSS 资源打包、运行时热切换
- **中英文界面**：中文源文本 + Qt Linguist 英文译文（`i18n/`），设置页切换、重启生效；译文内嵌单 exe，FluentUI 官方中文随库资源
- **模型选择**：下拉切换（清单见 `AgentConstants.h`），API 经设置页配置

## 运行时配置

- **模型服务**（设置页「模型服务」分组，落盘 exe 同目录 `settings.ini`）：
  - `apiBaseUrl` — OpenAI 兼容 API 基地址（http/https）
  - `apiToken` — API token（值区脱敏显示；明文存本机 ini）
- **环境变量**：`MODEL_ID` — 模型名（缺省用内置默认值）

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

### 打包发布 ZIP

先完成 Release 构建，再经 CPack 出可分发压缩包（自动部署 Qt 与 VC 运行时，无需安装 Qt）：

```powershell
cpack --config build/CPackConfig.cmake -B build
```

产物：`build/lite-harness-s12.6-win64.zip`（约 54MB），解压后运行 `lite-harness-s12.6-win64/bin/lite-harness.exe` 即可。

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `src/` | 全部 C++ 源码：Agent 核心（AgentLoop / QOpenAi / TaskStore / CompactManager / MemoryManager / CronScheduler…）+ FluentUI 页面与控件 |
| `stylesheet/` | 三主题 QSS（light / dark / atomOneDark），经 qrc 打包 |
| `i18n/` | 翻译源 `lite-harness_en_US.ts`（中文源→英文译文）与 `lite-harness_zh_CN.ts`（同文镜像，供 Linguist 审计），构建期 lrelease 内嵌 |
| `3rdparty/FluentUI` | UI 框架子模块（构建必需） |
| `3rdparty/lcc` | 移植规格参考仓库子模块（不参与构建） |
| `res/` | 应用图标与 Windows 资源脚本 |
| `docs/` | 设计文档（`chat-session-design.md` 为早期草图，已过时） |

## 文档

- [AGENTS.md](AGENTS.md) — 架构地图、构建/主题/数据路径约定（面向开发代理）
- `docs/` — 历史设计草图

## 致谢

- [FluentUI](https://github.com/mowangshuying/FluentUI) — Qt Fluent Design 控件库
