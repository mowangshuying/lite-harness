#pragma once

// Agent 常量单源头：收敛散落的魔法数/字面量（模型清单、max_tokens、bash 超时、
// 输出截断上限、中间目录名、glob 有界化参数）。纯 header-only，被 include 即可编译，
// 无需加入 CMake 源列表。
// 使用方：AgentLoop.cpp / SubAgent.cpp / BashRunner.cpp / ChatMsgEdit.cpp / CompactManager.cpp /
// MemoryManager.cpp / SettingsPage.cpp（上下文上限设置卡）。

#include <QString>
#include <QStringList>
#include <QtGlobal> // qsizetype
#include <QMetaType> // iniTextValue 判返回值形态
#include <QVariant>  // iniTextValue 取原始值
#include "AppSettings.h"

namespace AgentConst {

// 可选模型清单：内置兜底两项 + settings.ini 覆盖（键 modelOptions，逗号分隔）。
// 用户裁决（配置化改造）：加模型改配置文件即可，下拉框即时可选，不再改代码重编。
// 生效清单首项为回落默认项（AgentLoop::model() 不在清单内时下拉显示并选中它），
// 也可用 defaultModel 键越过首项单独指定默认模型。
// 值带不带引号都认：不带引号的逗号串在 ini 语法里是「列表」写法，由 iniTextValue 归一化回逗号串。
inline const QStringList kBuiltinModelOptions = {QStringLiteral("qwen3.8-flash"),
                                                 QStringLiteral("qwen3.8-max")};
inline const QString kModelOptionsKey = QStringLiteral("modelOptions");
inline const QString kDefaultModelKey = QStringLiteral("defaultModel");

// ini 取值归一化（读侧唯一入口）：手改 settings.ini 时值一般不带引号，而「逗号分隔的一串」恰好是
// QSettings ini 语法的列表写法——它被解析成 QStringList，此时 .toString() 返回空串（Qt 6.9 实测），
// 配置就被误判成「未配置」而静默回退内置清单。故统一走这里：QStringList 用逗号回接（Qt 解析时已逐项
// trim），其余形态按字符串取。标量键若被写成逗号串，回接后带逗号 → 校验不过 → 按既有语义回退默认值。
inline QString iniTextValue(const QString &key)
{
    const QVariant stored = AppSettings::ini().value(key);
    if (stored.metaType() == QMetaType::fromType<QStringList>())
        return stored.toStringList().join(QLatin1Char(','));
    return stored.toString();
}
constexpr qsizetype kModelOptionsMax = 32; // 清单条数上限（防手工篡改塞进超长清单）

// 逗号分隔串 → 清单：切分去空项、逐项 trim、去重、截断上限。纯函数，
// 读侧（modelOptions）与设置页写侧（ModelListSettingCard 展示/回写）共用同一口径。
inline QStringList parseModelOptions(const QString &stored)
{
    QStringList result;
    const QStringList parts = stored.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts)
    {
        const QString name = part.trimmed();
        if (name.isEmpty() || result.contains(name))
            continue;
        result.append(name);
        if (result.size() >= kModelOptionsMax)
            break;
    }
    return result;
}

// 生效清单单点取值：settings.ini 的 modelOptions 解析结果为空（键缺失/空串/全空白）
// 一律回退内置清单——配置文件只增不改语义，写坏或清空都不会让下拉框变成空下拉。
inline QStringList modelOptions()
{
    const QStringList parsed =
        parseModelOptions(iniTextValue(kModelOptionsKey));
    return parsed.isEmpty() ? kBuiltinModelOptions : parsed;
}

// 默认模型单点取值（AgentLoop 构造期 MODEL_ID 环境变量为空、下拉框回落选中时共用）：
// defaultModel 非空且确在生效清单内才采信，否则取清单首项（防配置指向不存在的模型）。
inline QString defaultModel()
{
    const QStringList options = modelOptions();
    const QString stored = iniTextValue(kDefaultModelKey).trimmed();
    if (!stored.isEmpty() && options.contains(stored))
        return stored;
    return options.first();
}

// LLM 请求输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致）
constexpr int kMaxTokens = 8000;

// 推理强度档位（OpenAI 兼容端点 reasoning_effort）：端点按档位分配思考预算，
// 对齐 opencode 的 xhigh 观感（长思考、少工具轮）。端点不认此字段时行为回退
// 为服务端默认，无副作用。
inline const QString kReasoningEffort = QStringLiteral("xhigh");

// 采样参数（对齐 opencode 配置 temperature 0.1 / topP 0.75）：低温让工具调用决策
// 更确定、少随机性绕路；端点不认这些字段时回退服务端默认，无副作用。
constexpr double kTemperature = 0.1;
constexpr double kTopP = 0.75;

// ---- 工具调用轮次上限（第十二轮：改为可设置项） ----
// 防止模型反复请求工具形成死循环（原 kMaxToolIterations=300，自 AgentLoop.cpp 匿名 ns
// 收敛）。用户可在设置页调整，落 settings.ini 配置文件（键 maxToolIterations）。校验界
// [10,1000]：下界防误设 0/过小值导致回合刚起步即被掐死，上界是失控防线的天花板——
// 再大也只是放大死循环损失，无正当用途。
constexpr int kMaxToolIterationsDefault = 500; // 未设置时的默认上限（用户裁决值）
constexpr int kMaxToolIterationsMin = 10;      // 校验下界
constexpr int kMaxToolIterationsMax = 1000;    // 校验上界
inline const QString kMaxToolIterationsKey = QStringLiteral("maxToolIterations");

// 轮次上限单点取值：settings.ini 配置文件读取 + 范围校验 + 默认回退。设置页（展示/校验回写）与
// AgentLoop（回合入口快照）共用同一实现，避免多处读盘/校验口径分叉；缺失、
// 非整数、越界（含手工篡改配置文件）一律回退默认值。存储单源见 AppSettings.h
// （exe 同目录 settings.ini）；调用频率为回合级/交互级，读盘成本可忽略。
inline int maxToolIterationsValue()
{
    QSettings settings = AppSettings::ini();
    bool ok = false;
    const qlonglong stored = settings.value(kMaxToolIterationsKey).toLongLong(&ok);
    if (!ok || stored < kMaxToolIterationsMin || stored > kMaxToolIterationsMax)
        return kMaxToolIterationsDefault;
    return static_cast<int>(stored);
}

// ---- 网络重试次数（429 / 5xx 指数退避）----
// QOpenAi 的重试上限。原默认 0 且全仓无 setMaxRetries 调用方，退避链路运行时不可达；
// 现由 QOpenAi::initFromSettings 读本键注入。校验界 [0,5]：下界 0 保留「不重试」语义
// （用户裁决），上界防手工改配置把单回合拖成分钟级等待（退避 1s,2s,4s…，5 次累计约 31s）。
constexpr int kMaxRetriesDefault = 2; // 未设置时的默认重试次数（用户裁决值）
constexpr int kMaxRetriesMin = 0;     // 校验下界（0 = 关闭重试）
constexpr int kMaxRetriesMax = 5;     // 校验上界
inline const QString kMaxRetriesKey = QStringLiteral("maxRetries");

// 重试次数单点取值：settings.ini 读取 + 范围校验 + 默认回退（与 maxToolIterationsValue 同纪律）。
// 消费方：QOpenAi::initFromSettings（启动注入）；缺失、非整数、越界一律回退默认值。
inline int maxRetriesValue()
{
    QSettings settings = AppSettings::ini();
    bool ok = false;
    const qlonglong stored = settings.value(kMaxRetriesKey).toLongLong(&ok);
    if (!ok || stored < kMaxRetriesMin || stored > kMaxRetriesMax)
        return kMaxRetriesDefault;
    return static_cast<int>(stored);
}

// PostToolUse large_output 提醒阈值（字符数，lcc 语义独立于 kOutputCharLimit 截断上限：
// 截断发生在 BashRunner/工具侧，此处是"未截断的超长输出"给模型的额外提醒门槛）
constexpr qsizetype kLargeOutputThreshold = 100000;

// todo_write 清单项数上限（schema maxItems，超限交模型重试）
constexpr int kTodoMaxItems = 20;

// task 子代理实时进度行的在卡条数上限（UI 侧裁剪）：超限丢弃最旧行、保留最新窗口，
// 日志顶部标注省略条数。子代理轮次预算与主循环同源可设置（默认 500 轮）× 每轮多工具时
// 日志可远超单卡承载，
// 60 行足够回看近况且限高滚动区不膨胀（进度行由 ToolBlock 消费）
constexpr int kSubagentProgressMaxLines = 60;

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

// read_file 读入字节上限（挂起审计防御加固：约 4×kOutputCharLimit 的字节口径）：
// runReadFileIn 原为同步 readAll，LLM 指到数百 MB 文件时主线程冻结秒级（零线程纪律下
// 只能限量读）。超限只读开头 kReadFileMaxBytes 字节并在返回文本尾部附截断说明；
// 展示层截断仍由 kOutputCharLimit 负责，本上限只界定"读进内存的字节量"。
constexpr qint64 kReadFileMaxBytes = 200000;

// edit_file 体量上限（字节）：与 read_file 的 kReadFileMaxBytes 同属「挂起审计防御加固」族。
// 零线程纪律下 edit_file 必须整文件读入，且要做多次整串拷贝（UTF-8 往返等价预检、
// 行尾两级匹配的归一化与还原），数百 MB 文件会冻结主线程秒级——read_file 已因此加了限量读，
// edit_file 不能反而无界。超限直接拒绝编辑并回可判定错误，不尝试部分读写（部分写会毁文件）。
// 取值远大于任何真实源文件/文档，只拦「LLM 指到巨型文件」这类异常，不误伤正常编辑。
constexpr qint64 kEditFileMaxBytes = 5 * 1024 * 1024;

// write_file 覆盖已存在文件时的行尾探测读入字节数：只为统计主导行尾（CRLF vs 裸 LF），
// 不需全文——大文件整体读进内存纯属浪费。64KB 足以覆盖任何真实文件的行尾样本；
// 已知边界：窗口内一个换行都没有（超长单行文件）时判为 LF，见 runWriteFileIn 注释。
constexpr qint64 kEndingProbeBytes = 65536;

// ChatStream 总时长哨兵（毫秒，挂起审计防御加固：对齐同文件 AsyncRequest 的总量防线）：
// idle 静默超时每收字节即重置，杀不死"慢而不断"的流——上游持续发字节（间隔 < idle 窗口）
// 却永不发 [DONE]/finish_reason 时，主链回合永不终结。总量上限按主链长回复场景取 30 分钟；
// <=0 表示不设总时限。
constexpr int kStreamTotalTimeoutMs = 1800000;

// ---- 上下文压缩主上限（第九轮：改为可设置项） ----
// 原 CompactManager 文件级常量 kContextCharLimit=50000 现为本默认值；用户可在设置页
// 调整，落 settings.ini 配置文件（键 contextCharLimit）。其余三个压缩阈值按主上限等比派生
// （batch=4S、large=0.6S、summary=1.6S、压缩目标=0.8S——比例与原 lcc 常量在 50000
// 基准下逐一对应，派生表达式见 CompactManager.cpp 消费点 helper，防比例漂移）。
constexpr qsizetype kContextCharLimitDefault = 200000; // 未设置时的主上限（用户裁决值）
constexpr qsizetype kContextCharLimitMin = 10000;      // 校验下界
constexpr qsizetype kContextCharLimitMax = 5000000;    // 校验上界
inline const QString kContextCharLimitKey = QStringLiteral("contextCharLimit");

// 主上限单点取值：settings.ini 配置文件读取 + 范围校验 + 默认回退。设置页（展示/校验回写）与
// CompactManager（管线消费）共用同一实现，避免多处读盘/校验口径分叉；缺失、
// 非整数、越界（含手工篡改配置文件）一律回退默认值。存储单源见 AppSettings.h
// （exe 同目录 settings.ini）；调用频率为管线级/交互级，读盘成本可忽略。
inline qsizetype contextCharLimitValue()
{
    QSettings settings = AppSettings::ini();
    bool ok = false;
    const qlonglong stored = settings.value(kContextCharLimitKey).toLongLong(&ok);
    if (!ok || stored < kContextCharLimitMin || stored > kContextCharLimitMax)
        return kContextCharLimitDefault;
    return static_cast<qsizetype>(stored);
}

// ---- token 估算与预算派生（修4：计量口径 token 化） ----
// 触发与 UI 从「UTF-16 字符数」切换到 token 估算：CJK 码点 1 字符 ≈ 1 token，
// 其余 4 字符 ≈ 1 token（向上取整）。预算换算单点：contextCharLimit（键名拼法涉
// settings.ini 数据兼容，不可改；语义自此为「预算派生基准」）÷ 每 token 均字符数。
// 不新增设置键（裁决）。锚点校准（usage.prompt_tokens 回填）在宿主侧与本估算组合，
// 本头只提供纯函数估算，零 JSON 依赖。
constexpr qsizetype kCharsPerTokenBudget = 4; // contextCharLimit(字符) → token 预算的换算基准

// CJK 区段判定（估算用粗分类，非严格字族学）：假名/汉字/日韩谚文/兼容汉字/全角半角。
// 落在 U+2E80–U+9FFF、U+AC00–U+D7AF、U+F900–U+FAFF、U+FF00–U+FFEF 之外的一律按 4:1 折算。
inline bool isCjkCodePoint(char16_t u)
{
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF)
        || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF);
}

// UTF-16 码元串 → token 估算：CJK 每码元计 1，其余按 4 字符=1 token 向上取整
// （代理对按 2 个非 CJK 码元参与折算，与全仓 UTF-16 字符计量口径同源的近似，登记偏差）。
inline qsizetype estimateTokens(const QString &text)
{
    qsizetype cjk = 0;
    for (const QChar ch : text) {
        if (isCjkCodePoint(ch.unicode()))
            ++cjk;
    }
    const qsizetype other = text.size() - cjk;
    return cjk + (other + 3) / 4;
}

// 全局 token 预算单点取值：现取现用（与 contextCharLimitValue 同纪律，改设置下一回合生效）。
inline qsizetype contextTokenBudget()
{
    return contextCharLimitValue() / kCharsPerTokenBudget;
}

// usage 尾 chunk 宽限毫秒（修3）：流 finished 后等待末个 SSE chunk 携带 usage 落地的
// 短窗口，超时即按无 usage 交付（估算口径兜底）。消费方：QOpenAi::ChatStream（lane A）。
constexpr int kUsageGraceMs = 1500;

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
inline const QString kMailboxesDirName = QStringLiteral(".mailboxes"); // 团队消息总线邮箱（一人一个 JSONL，lcc s13）
inline const QString kWorktreesDirName = QStringLiteral(".worktrees"); // git worktree 隔离目录（一任务一 checkout，lcc s13）

// ---- 队友保留名单源（lcc message_bus.py:17 RESERVED_TEAMMATE_NAMES={"lead","agent"}，
// 原注释「供 Lane B/C/D 复用」——gate① M7 收口进本头，casefold 比较）----
// **双重语义提醒**（fix-4 须在侧别分清，勿混用）：
//  · "lead"  = 消息总线保留收件名——Lead 的邮箱名，队友不得占用（spawn 侧拒用）；
//  · "agent" = 双重身份——既是邮箱保留名（同上拒 spawn），又是 TaskStore Lead owner 键
//    （无租约回落 workDir 的 owner 判定，TaskStore 租约/释放语义，与本名单无关）。
// 本轮只立单源：MessageBus 路径三关**不**查保留名（lcc 同款，本类只 fail-closed 路径），
// 真正的拒 spawn 校验在 fix-4 AgentTeamsManager 侧消费本单源。
inline const QStringList kReservedTeammateNames = {
    QStringLiteral("lead"), QStringLiteral("agent"),
};

inline bool isReservedTeammateName(const QString &name)
{
    // casefold 口径：lcc python `name.lower() in RESERVED` 的等价物（名单仅 ASCII，
    // Qt::CaseInsensitive 足够；邮箱名正则本身大小写敏感，比较只在保留名判定处放宽）
    for (const QString &reserved : kReservedTeammateNames) {
        if (name.compare(reserved, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// ---- 团队空闲心跳节拍单源（Gate② FIND-N3：自 AgentTeamsManager.h 文件尾迁入本头）----
// lcc agent_teams_manager.py:185 `IDLE_SCAN_INTERVAL = 2.0`（wait_for_work :998-1023 的
// 轮询节拍，一拍的活=看信箱+扫任务板两张嘴）。消费方：TeammateRuntime 空闲心跳 QTimer
// （毫秒域，故存 int 毫秒而非秒，2000ms=2.0s 逐值对位）。
constexpr int kTeamIdleScanIntervalMs = 2000;

// ---- 队友同步 bash 适配器等待界（s13 P3b 宿主侧，AgentLoopTeam.cpp 消费）----
// 引擎工具适配器契约为同步 QString 返回（lcc 队友 daemon 线程 subprocess.run(timeout)
// 同型阻塞，lite 零线程约束把阻塞压缩到主线程）：进程启动确认与超时 kill 后回收
// 两个兜底等待都必须有界，勿学 lcc 无界 join。
constexpr int kTeamBashStartWaitMs = 5000; // QProcess::waitForStarted 上界
constexpr int kTeamBashKillWaitMs = 1000;  // kill() 后 waitForFinished 回收上界

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
