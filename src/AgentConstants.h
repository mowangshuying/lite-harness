#pragma once

// Agent 常量单源头：收敛散落的魔法数/字面量（模型清单、max_tokens、bash 超时、
// 输出截断上限、中间目录名、glob 有界化参数）。纯 header-only，被 include 即可编译，
// 无需加入 CMake 源列表。
// 使用方：AgentLoop.cpp / SubAgent.cpp / BashRunner.cpp / ChatMsgEdit.cpp / CompactManager.cpp /
// MemoryManager.cpp / SettingsPage.cpp（上下文上限设置卡）。

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtGlobal> // qsizetype

namespace AgentConst {

// 可选模型清单（原 ChatMsgEdit.cpp 静态字面量迁入）：不做注册表/配置等多余抽象；
// 首项为回落默认项（AgentLoop::model() 不在列表内时下拉显示并选中它）
inline const QStringList kModelOptions = {QStringLiteral("qwen3.8-flash"),
                                         QStringLiteral("qwen3.8-max")};

// 回落默认模型 = 清单首项（AgentLoop 构造期 MODEL_ID 环境变量为空时使用）
inline const QString kDefaultModel = kModelOptions.first();

// LLM 请求输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致）
constexpr int kMaxTokens = 8000;

// 工具调用轮次上限（防止模型反复请求工具形成死循环；自 AgentLoop.cpp 匿名 ns 收敛）
constexpr int kMaxToolIterations = 300;

// PostToolUse large_output 提醒阈值（字符数，lcc 语义独立于 kOutputCharLimit 截断上限：
// 截断发生在 BashRunner/工具侧，此处是"未截断的超长输出"给模型的额外提醒门槛）
constexpr qsizetype kLargeOutputThreshold = 100000;

// todo_write 清单项数上限（schema maxItems，超限交模型重试）
constexpr int kTodoMaxItems = 20;

// glob 工具结果展示条数（超出部分折叠为 "more matches omitted" 提示；
// 受 kGlobCollectLimit 收集上限约束，见上方注释的相对关系）
constexpr qsizetype kGlobDisplayLimit = 200;

// bash 工具执行超时（毫秒）：前台/后台/子代理三处共用
constexpr int kBashTimeoutMs = 120000;

// bash 超时回填给模型的错误文案：由 kBashTimeoutMs 派生，避免常量与文案漂移
// （逐字符等于原硬编码 "Error: Timeout (120s)"，前缀 "Error: " 为主循环错误族格式）
inline const QString kBashTimeoutError =
    QStringLiteral("Error: Timeout (%1s)").arg(kBashTimeoutMs / 1000);

// 工具输出字符截断上限（bash 前台/后台、read_file 共用；lcc [:50000] 语义）
constexpr qsizetype kOutputCharLimit = 50000;

// ---- 上下文压缩主上限（第九轮：改为可设置项） ----
// 原 CompactManager 文件级常量 kContextCharLimit=50000 现为本默认值；用户可在设置页
// 调整，落注册表 QSettings（键 contextCharLimit）。其余三个压缩阈值按主上限等比派生
// （batch=4S、large=0.6S、summary=1.6S、压缩目标=0.8S——比例与原 lcc 常量在 50000
// 基准下逐一对应，派生表达式见 CompactManager.cpp 消费点 helper，防比例漂移）。
constexpr qsizetype kContextCharLimitDefault = 200000; // 未设置时的主上限（用户裁决值）
constexpr qsizetype kContextCharLimitMin = 10000;      // 校验下界
constexpr qsizetype kContextCharLimitMax = 5000000;    // 校验上界
inline const QString kContextCharLimitKey = QStringLiteral("contextCharLimit");

// 主上限单点取值：注册表读取 + 范围校验 + 默认回退。设置页（展示/校验回写）与
// CompactManager（管线消费）共用同一实现，避免多处读盘/校验口径分叉；缺失、
// 非整数、越界（含手工篡改注册表）一律回退默认值。QSettings 默认构造命中
// org/app=LiteHarness（App.cpp 全局设定）；调用频率为管线级/交互级，读盘成本可忽略。
inline qsizetype contextCharLimitValue()
{
    QSettings settings;
    bool ok = false;
    const qlonglong stored = settings.value(kContextCharLimitKey).toLongLong(&ok);
    if (!ok || stored < kContextCharLimitMin || stored > kContextCharLimitMax)
        return kContextCharLimitDefault;
    return static_cast<qsizetype>(stored);
}

// ---- 会话数据根下的中间目录名（叶子段）单源 ----
// 根路径统一由 AgentLoop::sessionDataRoot()（含 .lite-harness 中间层/会话段）或各引擎
// 注入的 workDirSink 提供，此处只收敛最后拼接的叶子段，防止 AgentLoop/CompactManager
// 多份拼法漂移。**拼法逐字符不可改动——涉及既有落盘数据的读取兼容**（历史会话的
// .task/.transcripts 等按旧名写盘，任何改名等价于数据丢失）。
inline const QString kTaskDirName = QStringLiteral(".task"); // 任务图（一任务一 JSON）
inline const QString kTempDirName = QStringLiteral(".temp"); // prompt 临时目录（system prompt 引导语指向）
inline const QString kTranscriptsDirName = QStringLiteral(".transcripts"); // 压缩转写 JSONL
inline const QString kToolResultsDirName =
    QStringLiteral(".task_outputs/tool-results"); // 大工具输出卸载（含一级子目录）
inline const QString kMemoryDirName = QStringLiteral(".memory"); // 记忆存储（MEMORY.md 索引 + slug 记录）

// ---- glob 工具（runGlobIn）有界化参数 ----
// runGlobIn 在 GUI 线程同步递归遍历（全仓零线程约定，不改线程模型），必须硬限界：
// 原 QDirIterator 无上限且进入 .git/build 等巨型目录，大仓库直接冻结 UI。
// 剪枝目录名（大小写不敏感整目录跳过；覆盖 VCS/构建产物/依赖/缓存/虚拟环境常见巨头）：
inline const QStringList kGlobPruneDirNames = {
    QStringLiteral(".git"),          QStringLiteral("build"),
    QStringLiteral("node_modules"),  QStringLiteral("out"),
    QStringLiteral("x64"),           QStringLiteral(".vs"),
    QStringLiteral("__pycache__"),   QStringLiteral(".venv"),
    QStringLiteral("venv")};

// 遍历条目硬上限（目录+文件合计计数，超限停止遍历并在结果尾部附截断提示）
constexpr qsizetype kGlobScanEntryLimit = 20000;

// 命中收集硬上限（匹配且过 safePathIn 的文件数；须大于展示条数 200，
// 否则上限先于展示窗口生效、"more matches omitted" 提示失去意义）
constexpr qsizetype kGlobCollectLimit = 2000;

} // namespace AgentConst
