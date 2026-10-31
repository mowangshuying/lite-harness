#pragma once

// Agent 常量单源头：收敛散落的魔法数/字面量（模型清单、max_tokens、bash 超时、
// 输出截断上限、中间目录名、glob 有界化参数）。纯 header-only，被 include 即可编译，
// 无需加入 CMake 源列表。
// 使用方：AgentLoop.cpp / SubAgent.cpp / BashRunner.cpp / ChatMsgEdit.cpp / CompactManager.cpp /
// MemoryManager.cpp / SettingsPage.cpp（上下文上限设置卡）。

#include <QString>
#include <QStringList>
#include <QtGlobal> // qsizetype
#include <QMetaType> // textValue 判返回值形态
#include <QVariant>  // textValue 取原始值
#include "AppSettings.h"

namespace AgentConst {

// 可选模型清单：内置兜底两项 + config.json 覆盖（键 modelOptions，逗号分隔）。
// 用户裁决（配置化改造）：加模型改配置文件即可，下拉框即时可选，不再改代码重编。
// 生效清单首项为回落默认项（AgentLoop::model() 不在清单内时下拉显示并选中它），
// 也可用 defaultModel 键越过首项单独指定默认模型。
// 值带不带引号都认：ini 后端的裸逗号串（QStringList）与 json 后端的数组（QVariantList）
// 两种「列表」形态，都由 textValue 归一化回逗号串。
inline const QStringList kBuiltinModelOptions = {QStringLiteral("qwen3.8-flash"),
                                                 QStringLiteral("qwen3.8-max")};
inline const QString kModelOptionsKey = QStringLiteral("modelOptions");
inline const QString kDefaultModelKey = QStringLiteral("defaultModel");

// 「一串」的两种列表形态 → 逗号串回接（纯函数，零 QSettings/零夹具，故可进 ctest 直测）。
// 为什么必须判**两种**形态，这是配置文件由 ini 合流成 json 后才出现的分叉：
//   · ini 后端：手改时值一般不带引号，而「逗号分隔的一串」恰好是 QSettings ini 的列表语法，
//     被解析成 QStringList；
//   · json 后端：自定义格式的 flatten 走 QJsonValue::toVariant()，数组落 **QVariantList**
//     而非 QStringList（Qt 6.9 实测：typeId=9，`== QStringList` 判据为假）。
// 两种形态的 .toString() 都返回空串，一旦被当成标量取走，配置就被误判「未配置」而静默回退
// 内置清单——合流前只判 QStringList 是对的，合流后只判 QStringList 会让用户手写
// `"modelOptions": ["a","b"]`（JSON 下最自然的清单写法）读不回。故两个判据并列，缺一不可。
// 标量键若被写成逗号串，回接后带逗号 → 校验不过 → 按既有语义回退默认值。
inline QString normalizedText(const QVariant &stored)
{
    if (stored.metaType() == QMetaType::fromType<QStringList>()
        || stored.metaType() == QMetaType::fromType<QVariantList>())
        return stored.toStringList().join(QLatin1Char(','));
    return stored.toString();
}

// 取值归一化（读侧唯一入口）：取原始值走 AppSettings::value()（缓存解析产物，按文件身份惰性
// 重载）而非现构造 QSettings——本函数被 modelOptions/defaultModel/fallbackModelIdValue 共用，
// 消费点已下沉到 per-tool 级。归一化本体是上面的纯函数 normalizedText，此处只做取数转发。
inline QString textValue(const QString &key)
{
    return normalizedText(AppSettings::value(key));
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

// 生效清单单点取值：config.json 的 modelOptions 解析结果为空（键缺失/空串/全空白）
// 一律回退内置清单——配置文件只增不改语义，写坏或清空都不会让下拉框变成空下拉。
inline QStringList modelOptions()
{
    const QStringList parsed =
        parseModelOptions(textValue(kModelOptionsKey));
    return parsed.isEmpty() ? kBuiltinModelOptions : parsed;
}

// 默认模型单点取值（AgentLoop 构造期 MODEL_ID 环境变量为空、下拉框回落选中时共用）：
// defaultModel 非空且确在生效清单内才采信，否则取清单首项（防配置指向不存在的模型）。
inline QString defaultModel()
{
    const QStringList options = modelOptions();
    const QString stored = textValue(kDefaultModelKey).trimmed();
    if (!stored.isEmpty() && options.contains(stored))
        return stored;
    return options.first();
}

// ---- s15 过载换模后备模型（lcc s15 af26862 recovery.py fallbackModel / env.py FALLBACK_MODEL_ID）----
// 传输层连续过载（529/overloaded）达 kMaxConsecutive529 上限时，重发体 model 字段改用此值。
// 空串 = 禁用换模（对照 lcc env.py 的 "or None" 归一：未配置则全程不换模，行为与 s15 前逐字节一致）。
// 刻意**不做** defaultModel 式的「清单内才采信」校验：fallback 语义是给清单外的另一服务兜底，
// 且换模失败自然回落既有错误链，无须入口设闸。读值必须经 textValue（同清单键纪律，防逗号串形态误判未配置）。
inline const QString kFallbackModelKey = QStringLiteral("fallbackModelId");
inline QString fallbackModelIdValue()
{
    return textValue(kFallbackModelKey).trimmed();
}

// LLM 请求输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致）
constexpr int kMaxTokens = 8000;

// ---- s15 错误恢复参数（对照 lcc s15 af26862 recovery.py 常量块 :17-21）----
// kMaxConsecutive529 本轮由 QOpenAi 传输层消费；另两个的消费点在 AgentLoop 恢复接线（后续 lane），
// 先入本头单源，防后续 lane 各处字面量分叉。
// 偏差登记：lcc 的 MAX_RETRIES=3 与退避（500ms·2^n 封顶 32s + 25% 抖动）**不采**——lite 重试预算
// 与退避沿用 config.json 键 maxRetries 的既有口径（1s,2s,4s…封顶 64s 无抖动），语义等价，
// 不引入随机抖动也保证事件驱动下的行为可复现。
constexpr int kEscalatedMaxTokens = 16000; // max_tokens 溢出第一拍：升档到该值重发（lcc ESCALATED_MAX_TOKENS）
constexpr int kMaxConsecutive529 = 2;      // 连续过载计数界：达到即换 fallback 模型（lcc MAX_CONSECUTIVE_529）
constexpr int kMaxRecoveryRetries = 2;     // 恢复级重试预算（lcc MAX_RECOVERY_RETRIES，后续 lane 消费）

// ---- s17 goal loop 常量（对照 lcc s17 a20fdec goal_manager.py 常量块 L38-43）----
// 消费点在 GoalRules.h / GoalManager 引擎层；前三个是契约值（评估器单次输出上限、条件长度门、
// 转录渲染预算），第四个同时是 config.json 键 stopHookBlockCap 的缺省回退值。
constexpr int kGoalEvaluatorMaxTokens = 512;    // lcc DEFAULT_EVALUATOR_MAX_TOKENS：评估器不传 tools 的单发上限
constexpr int kGoalMaxConditionLength = 4000;   // lcc MAX_GOAL_LENGTH：goal 条件长度门（超限折叠逐字错误串）
constexpr int kGoalTranscriptCharLimit = 24000; // lcc TRANSCRIPT_CHAR_LIMIT：评估器转录预算（从新往回累加）

// 评估器专用模型（lcc env GOAL_EVALUATOR_MODEL_ID，port ⑦ 三级回退的 lite 形）：
// 空串 = 用主循环当前模型。读值必须经 textValue（同 fallbackModelId 的裸逗号串纪律）。
// 刻意不做「清单内才采信」校验：与 fallbackModelId 同理，评估器允许挂在模型清单外的服务上。
inline const QString kGoalEvaluatorModelKey = QStringLiteral("goalEvaluatorModelId");
inline QString goalEvaluatorModelIdValue()
{
    return textValue(kGoalEvaluatorModelKey).trimmed();
}

// 连续 block 上限（lcc env STOP_HOOK_BLOCK_CAP + GoalController block_cap 唯一守卫 L249-250）：
// 校验只有下界 >=1（lcc 无上限，不另造）；缺失/非整数/<1 一律回退 kGoalStopHookBlockCap。
constexpr int kGoalStopHookBlockCap = 8; // lcc DEFAULT_STOP_HOOK_BLOCK_CAP：连续 block 超限即 limit（不清 goal）
inline const QString kStopHookBlockCapKey = QStringLiteral("stopHookBlockCap");
inline int stopHookBlockCapValue()
{
    bool ok = false;
    const qlonglong stored = AppSettings::value(kStopHookBlockCapKey).toLongLong(&ok);
    if (!ok || stored < 1)
        return kGoalStopHookBlockCap;
    return static_cast<int>(stored);
}

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
// 收敛）。用户可在设置页调整，落 config.json 配置文件（键 maxToolIterations）。校验界
// [10,1000]：下界防误设 0/过小值导致回合刚起步即被掐死，上界是失控防线的天花板——
// 再大也只是放大死循环损失，无正当用途。
constexpr int kMaxToolIterationsDefault = 500; // 未设置时的默认上限（用户裁决值）
constexpr int kMaxToolIterationsMin = 10;      // 校验下界
constexpr int kMaxToolIterationsMax = 1000;    // 校验上界
inline const QString kMaxToolIterationsKey = QStringLiteral("maxToolIterations");

// 轮次上限单点取值：config.json 配置文件读取 + 范围校验 + 默认回退。设置页（展示/校验回写）与
// AgentLoop（回合入口快照）共用同一实现，避免多处读盘/校验口径分叉；缺失、
// 非整数、越界（含手工篡改配置文件）一律回退默认值。存储单源见 AppSettings.h
// （存储根下唯一的 config.json，宿主键挂 `app/` 组）；取值经 AppSettings::value() 的读缓存，
// 调用频率不再受读盘成本约束。
inline int maxToolIterationsValue()
{
    bool ok = false;
    const qlonglong stored = AppSettings::value(kMaxToolIterationsKey).toLongLong(&ok);
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

// 重试次数单点取值：config.json 读取 + 范围校验 + 默认回退（与 maxToolIterationsValue 同纪律，
// 含同走 AppSettings::value() 读缓存）。
// 消费方：QOpenAi::initFromSettings（启动注入）；缺失、非整数、越界一律回退默认值。
inline int maxRetriesValue()
{
    bool ok = false;
    const qlonglong stored = AppSettings::value(kMaxRetriesKey).toLongLong(&ok);
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
// 调整，落 config.json 配置文件（键 contextCharLimit）。其余三个压缩阈值按主上限等比派生
// （batch=4S、large=0.6S、summary=1.6S、压缩目标=0.8S——比例与原 lcc 常量在 50000
// 基准下逐一对应，派生表达式见 CompactManager.cpp 消费点 helper，防比例漂移）。
constexpr qsizetype kContextCharLimitDefault = 200000; // 未设置时的主上限（用户裁决值）
constexpr qsizetype kContextCharLimitMin = 10000;      // 校验下界
constexpr qsizetype kContextCharLimitMax = 5000000;    // 校验上界
inline const QString kContextCharLimitKey = QStringLiteral("contextCharLimit");

// 主上限单点取值：config.json 配置文件读取 + 范围校验 + 默认回退。设置页（展示/校验回写）与
// CompactManager（管线消费）共用同一实现，避免多处读盘/校验口径分叉；缺失、
// 非整数、越界（含手工篡改配置文件）一律回退默认值。存储单源见 AppSettings.h
// （存储根下唯一的 config.json）；取值经 AppSettings::value() 读缓存——本函数被
// contextTokenBudget 复用，而后者已下沉到 per-tool 级消费点（ChatSessionPage 侧栏刷新），
// 每次现构造 QSettings 的读盘成本不再可忽略（§9.10 问题②）。
inline qsizetype contextCharLimitValue()
{
    bool ok = false;
    const qlonglong stored = AppSettings::value(kContextCharLimitKey).toLongLong(&ok);
    if (!ok || stored < kContextCharLimitMin || stored > kContextCharLimitMax)
        return kContextCharLimitDefault;
    return static_cast<qsizetype>(stored);
}

// ---- token 估算与预算派生（修4：计量口径 token 化） ----
// 触发与 UI 从「UTF-16 字符数」切换到 token 估算：CJK 码点 1 字符 ≈ 1 token，
// 其余 4 字符 ≈ 1 token（向上取整）。预算换算单点：contextCharLimit（键名沿用不改——它是
// 用户手改面，改名会让既有配置文件里的值静默失联；语义自此为「预算派生基准」）÷ 每 token 均字符数。
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
inline const QString kWorkflowDirName = QStringLiteral(".workflows"); // workflow 台账目录（lcc s16 env _ensure_store：首用时建、构造不 mkdir——目录创建自愈在 Lane A ensureStoreDirs，宿主只推路径）

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

// ---- 工具执行权限审批开关（s17.2：设置页「工具执行权限审批」） ----
// config.json 键 permissionApproval：true=需要审批（缺省，命中询问规则弹 PermissionCard 挂起队列），
// false=无需审批（三道「询问类」门一律放行）。纯布尔无区间校验；键缺失或写成读不出来的串一律回退
// true——回退方向刻意选更保守的一侧：配置解析不出来时宁可多问一次，也不静默放行。
// 消费方两处同源：AgentLoopHooks.cpp 内置 permission 钩子（ASK 规则门 / mcp__ confirm 门 /
// workflow confirm 门）与 AgentLoopTeam.cpp 队友 permissionCheck 注入。
// **本开关撤的是「问不问」，不是「许不许」**：bash 硬拒绝黑名单（拒绝先于用户意志）与 workflow
// 策略表的 deny 态都是定值硬拒，不受开关影响照常回填拒绝文本。
// 生效时机：钩子每次工具调用现取（经 AppSettings::value() 读缓存，零 QSettings 构造），故改完
// 下一个工具调用即生效，无需重启、无需新会话——与 contextCharLimit 同口径，不同于 maxToolIterations
// 的回合入口快照。
constexpr bool kPermissionApprovalDefault = true; // 未设置时需要审批
inline const QString kPermissionApprovalKey = QStringLiteral("permissionApproval");
inline bool permissionApprovalEnabled()
{
    return AppSettings::value(kPermissionApprovalKey, kPermissionApprovalDefault).toBool();
}

} // namespace AgentConst
