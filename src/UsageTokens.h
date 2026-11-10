#pragma once

// 服务端 usage 用量计量纯头（header-only，零 CMake 改动即入 ctest，仿 CompactTokens.h 先例）：
// `fromUsage` 把 ChatStream::usageReceived 回传的 usage 对象解析成快照，`Totals` 做会话级累加
// 与缓存命中率派生。纯函数、无状态、无 IO、无翻译——**词条与句子组装（tr()）全在消费方**
// （ChatStatusStrip / SessionSidebar / ContributionHeatmap）。**唯一的例外是 `formatCompact`**：
// 它是数字口径不是文案，见其定义处的理由说明。
//
// 为什么单独成头（而非塞进 AgentLoop.cpp）：解析与累加是**算术**，与「谁在计量」无关；
// AgentLoop / TeammateRuntime 两条链都要用，而 AgentLoop 的 TU 进不了测试目标：类带
// Q_OBJECT 需 moc、又链 QOpenAi（Qt6::Network），按 CMakeLists.txt「仅允许 QtCore-only 引擎
// 进测试清单」的硬纪律被挡在外。（注意**不是** FluentUI：LiteLog.h 在测试目标经
// LITE_NO_FLU_LOG 退化成裸 qDebug 族、并不把 FluentUI::Utils 拉进测试链接。）抽成纯头后
// tst_usagetokens 直接 include 即测真实编译产物（同 CompactTokens.h 的「抽纯头」路线，结构篇 §9.5）。
//
// **覆盖面边界（务必如实理解，勿高估这个数字）**：只有携带
// `stream_options.include_usage` 的 ChatStream 路径才会回 usage——即**主循环**与**队友回合**两处。
// 压缩摘要 / 记忆链 / workflow 的 agent() runner 走 `QOpenAi::AsyncRequest`，该类型没有 usage
// 通道（QOpenAi.h 只有 ChatStream::usageReceived），服务端回不回用量都不落地。故本累计器是
// **部分口径**，不是会话真实总开销；UI 侧必须在 tooltip 里写明这条边界，否则数字会误导成本判断。
//
// 有效性判据与 `AgentLoop::adoptUsageAnchor` 的落锚门**同一条**（`prompt_tokens > 0`）：
// 兼容端点不回 usage、或回全 0 时整条作废，绝不把「没数据」累加成「0 消耗」——后者会让
// 命中率分母虚增、把成本显示成比实际便宜。`valid == false` 的快照 `Totals::add` 直接忽略。
//
// 嵌套 details 缺失是常态而非异常（OpenAI 标准里 prompt_tokens_details 为可选；实测阿里云
// DashScope 兼容模式两项都回，而部分兼容端点一项都不回）→ 取不到即 0，不视为解析失败。

#include <QJsonObject>
#include <QLatin1Char>
#include <QString>

namespace UsageTokens {

// 单次请求的 usage 快照。全部字段为服务端原样口径（token 数），不做任何折算。
struct Snapshot {
    qint64 prompt = 0;      // usage.prompt_tokens
    qint64 completion = 0;  // usage.completion_tokens
    qint64 cached = 0;      // usage.prompt_tokens_details.cached_tokens（缓存命中的输入）
    qint64 reasoning = 0;   // usage.completion_tokens_details.reasoning_tokens（思考输出）
    bool valid = false;     // prompt > 0；与 adoptUsageAnchor 落锚判据同源
};

// 取一个数值键：缺失 / null / 非数值 / 负数一律归 0（toDouble 对非数值返回 0.0）。
// 单独成函数是为了让「脏数据不污染累加」这条只写一次、两处（顶层键与嵌套键）共用。
inline qint64 numberOrZero(const QJsonObject &object, const QString &key)
{
    const qint64 value = qint64(object.value(key).toDouble());
    return value > 0 ? value : 0;
}

// 紧凑读数（K / M）：行内与摘要位用它——状态条消耗段、侧栏「上下文」数字行、侧栏 goal
// 消耗、热力图 tooltip。精确读数不走这里，仍由消费方经 QLocale::c().toString() 出千分位，
// 且只出现在 tooltip 里：紧凑与精确分处两个视图，同一时刻用户能从「1.2K」跳回
// 「1,234 输入 / 567 产出」，不会因为紧凑而丢可核对的原值。
//
// 口径（四条都是决策，不是疏漏）：
//  · <1000 原样整数（"847"）——小数字带小数点反而更难读，也不产生「1.0K」这种假精度；
//  · <1e6 折 K、>=1e6 折 M，一位小数四舍五入，整十位省掉 ".0"（2000→"2K"、2500→"2.5K"）；
//  · K 档四舍五入进位到 1000 时升一档成 M，绝不出 "1000K"；
//  · **刻意只到 M，不设 G/B**：token 计数现实里到不了十亿，真到了显示 "1234.6M" 仍远短于
//    千分位全量，而多一种单位会让本头的口径与 tooltip 的口径分叉（用户要在两处认两种缩写）。
//
// 文案里刻意不再加「≈」：K/M 后缀本身就是「已四舍五入」的信号，再加 ≈ 会新增一批 tr()
// 词条、让状态条与侧栏两套措辞分叉，而精确值本来就在 tooltip 里可查。
//
// 全程整数运算，不走 double：double 在 2^53 以上丢精度，而这里只是整型除法 + 四舍五入。
// 舍入一律「先除后取整」——`round(magnitude/1000*10)` 恒等于 `round(magnitude/100)`，
// 写成 (magnitude + 50) / 100 而不是 (magnitude * 10 + 500) / 1000，是为了让被乘数
// 永远不超过入参本身：qint64 在 9.2e18 量级，*10 会溢出（长会话累计 + 未来更大端点
// 并非不可能），而加法版最多溢出 50 的量、且实际只在 magnitude 接近上限时才可能。
inline QString formatCompact(qint64 value)
{
    const bool negative = value < 0;
    // 绝对值用 unsigned 域取：qint64 最小负数取反是 UB（-9.2e18 超出正范围 1），
    // 本函数唯一的负值入口就是这条防御性透传。
    const quint64 magnitude = negative ? quint64(-(value + 1)) + 1ULL : quint64(value);

    QString digits;
    if (magnitude < 1000ULL) {
        digits = QString::number(magnitude);
    } else {
        // K 档：tenths = 一位小数的整十位表示 = round(magnitude / 100)
        const quint64 kTenths = (magnitude + 50ULL) / 100ULL;
        if (kTenths < 10000ULL) {       // < 1000.0K：留在 K 档
            digits = QString::number(kTenths / 10ULL);
            if (const quint64 frac = kTenths % 10ULL) {   // 整十位省掉 ".0"
                digits += QLatin1Char('.');
                digits += QString::number(frac);
            }
            digits += QLatin1Char('K');
        } else {                        // 进位到 1000K 及以上：走 M 档
            const quint64 mTenths = (magnitude + 50000ULL) / 100000ULL;
            digits = QString::number(mTenths / 10ULL);
            if (const quint64 frac = mTenths % 10ULL) {
                digits += QLatin1Char('.');
                digits += QString::number(frac);
            }
            digits += QLatin1Char('M');
        }
    }
    if (negative)
        digits.prepend(QLatin1Char('-'));
    return digits;
}

// 上下文占用率：used / limit。limit<=0 返回 0，上界钳到 1.0（与 hitRatio 同一条钳法）。
// 单独成函数是为了让这条算术只有**一个实现**：侧栏「上下文」节的占用条与数字行分属两个视图，
// 各自算就会在同一时刻给出两个读数（一个显示 100%、另一个画满 120% 宽）。
inline double usageRatio(qsizetype used, qsizetype limit)
{
    if (limit <= 0)
        return 0.0;
    const double ratio = double(used) / double(limit);
    return ratio > 1.0 ? 1.0 : ratio;
}

// 解析 usage 对象。不抛异常、不假设任何键存在。
inline Snapshot fromUsage(const QJsonObject &usage)
{
    Snapshot snapshot;
    snapshot.prompt = numberOrZero(usage, QStringLiteral("prompt_tokens"));
    snapshot.completion = numberOrZero(usage, QStringLiteral("completion_tokens"));
    // 嵌套 details：不是对象（缺失/null/字符串）→ toObject() 给空对象 → 取键得 0。
    snapshot.cached = numberOrZero(
        usage.value(QStringLiteral("prompt_tokens_details")).toObject(),
        QStringLiteral("cached_tokens"));
    snapshot.reasoning = numberOrZero(
        usage.value(QStringLiteral("completion_tokens_details")).toObject(),
        QStringLiteral("reasoning_tokens"));
    snapshot.valid = snapshot.prompt > 0;
    // 无效快照把其余字段一并清零：调用方若误用未判 valid 的快照，至多贡献 0，不会虚增。
    if (!snapshot.valid) {
        snapshot.completion = 0;
        snapshot.cached = 0;
        snapshot.reasoning = 0;
    }
    return snapshot;
}

// 会话级累加器。语义是「只增不清零」——生命周期跟随宿主（AgentLoop / TeammateRuntime），
// 换会话即新实例自然归零；回合结束、stop()、压缩改写历史都**不**重置它（消耗是既成事实）。
struct Totals {
    qint64 prompt = 0;
    qint64 completion = 0;
    qint64 cached = 0;
    qint64 reasoning = 0;
    int calls = 0;   // 计入的有效回读次数（invalid 不计数）

    // 无效快照直接忽略（见文件头「有效性判据」）。
    void add(const Snapshot &snapshot)
    {
        if (!snapshot.valid)
            return;
        prompt += snapshot.prompt;
        completion += snapshot.completion;
        cached += snapshot.cached;
        reasoning += snapshot.reasoning;
        ++calls;
    }

    bool isEmpty() const { return calls <= 0; }

    // 缓存命中率 = cached / prompt。无数据返回 0（不是 1，也不是未定义）。
    // 上界钳到 1.0：个别兼容端点会把 cached 报成大于 prompt（口径不同或 bug），
    // 不钳就会显示 120% 这种荒值；钳掉后仍可从 prompt/cached 原值看出异常，只是比率不失真。
    double hitRatio() const
    {
        if (prompt <= 0)
            return 0.0;
        const double ratio = double(cached) / double(prompt);
        return ratio > 1.0 ? 1.0 : ratio;
    }

    // 未命中缓存、按全价计费的输入 token（同样钳到非负）。
    qint64 uncachedPrompt() const
    {
        return cached > prompt ? 0 : prompt - cached;
    }
};

} // namespace UsageTokens
