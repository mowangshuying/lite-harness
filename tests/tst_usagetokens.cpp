// tst_usagetokens.cpp —— 服务端 usage 用量计量纯头直测（src/UsageTokens.h，header-only 零 CMake 改动）
// 被测：fromUsage 解析容错 / Totals 累加的有效性门 / hitRatio 与 uncachedPrompt 的钳位派生
//
// 本套件为什么必须存在（不是凑覆盖率）：`UsageTokens::Totals` 是要**显示给用户看**的成本数字。
// 它一旦把「没拿到数据」算成「0 消耗」，界面就理直气壮地显示「已用 0 token」——用户读作
// 「这轮没花钱」，而真相是「端点没回 usage」。这两种状态对成本判断的作用完全相反，
// 所以「无效读数整条作废、不进分母」是这套算术的承重墙，必须钉成断言。
//
// 同理 hitRatio 上界钳位：实测个别兼容端点会把 cached_tokens 报成大于 prompt_tokens
// （口径差异或 bug）。不钳就显示 120% 这种荒值；钳了之后 prompt/cached 原值仍在账上可查，
// 只是比率不再失真。这两条都在下面逐条断言。
//
// 覆盖面边界（勿高估）：本套件测的是**解析与累加算术**。它不证明
// 「AgentLoop/TeammateRuntime 真的调用了这两个函数」——那是接线，验证手段是编译期
// 等价 + 冒烟运行（清单篇 token 计量组）。两个宿主 TU 链 QOpenAi/FluentUI，按
// CMakeLists.txt:128「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest，
// 故本头按 CompactTokens.h 先例抽出，测真实编译产物而非复刻品。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "UsageTokens.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace {

// ---- 构造工具 ----

// 最小合法 usage（只有顶层两键，无任何 details）——部分兼容端点的真实形态
QJsonObject minimalUsage(double prompt, double completion)
{
    QJsonObject usage;
    usage.insert(QStringLiteral("prompt_tokens"), prompt);
    usage.insert(QStringLiteral("completion_tokens"), completion);
    return usage;
}

// 带嵌套 details 的完整形态
QJsonObject fullUsage(double prompt, double completion, double cached, double reasoning)
{
    QJsonObject usage = minimalUsage(prompt, completion);
    usage.insert(QStringLiteral("prompt_tokens_details"),
                 QJsonObject{{QStringLiteral("cached_tokens"), cached}});
    usage.insert(QStringLiteral("completion_tokens_details"),
                 QJsonObject{{QStringLiteral("reasoning_tokens"), reasoning}});
    return usage;
}

// ---- 1. 有效读数：逐字段如实解析，不做任何折算 ----

void testHappyPath()
{
    // 实测形态：qwen3.8-flash 同前缀第二次请求，缓存真实命中（本组数字来自真实响应）
    const UsageTokens::Snapshot s =
        UsageTokens::fromUsage(fullUsage(2311, 54, 2048, 50));
    TestHarness::check(s.valid, "happy: prompt>0 判有效");
    TestHarness::check(s.prompt == 2311, "happy: prompt 原样");
    TestHarness::check(s.completion == 54, "happy: completion 原样");
    TestHarness::check(s.cached == 2048, "happy: cached 自嵌套 details 取到");
    TestHarness::check(s.reasoning == 50, "happy: reasoning 自嵌套 details 取到");

    const UsageTokens::Totals t = [&s] {
        UsageTokens::Totals totals;
        totals.add(s);
        return totals;
    }();
    TestHarness::check(t.calls == 1, "happy: 计一次回读");
    TestHarness::check(t.prompt == 2311 && t.cached == 2048, "happy: 累加进总账");
    // 2048/2311 ≈ 0.88619
    TestHarness::check(qAbs(t.hitRatio() - 0.88619) < 0.0001, "happy: 命中率 = cached/prompt");
    TestHarness::check(t.uncachedPrompt() == 2311 - 2048, "happy: 未命中 = prompt-cached");
}

// ---- 2. 承重墙：无效读数整条作废，绝不进分母 ----

void testInvalidReadingsRejected()
{
    // (a) 空对象——端点压根没回 usage
    {
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(QJsonObject());
        TestHarness::check(!s.valid, "invalid: 空对象判无效");
        TestHarness::check(s.prompt == 0, "invalid: 空对象 prompt=0");
    }
    // (b) prompt_tokens 显式为 0（流被中途取消等）
    {
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(minimalUsage(0, 120));
        TestHarness::check(!s.valid, "invalid: prompt=0 判无效");
        TestHarness::check(s.completion == 0, "invalid: 无效快照的 completion 清零不带入");
    }
    // (c) 负数（脏数据）
    {
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(minimalUsage(-5, 7));
        TestHarness::check(!s.valid, "invalid: prompt<0 判无效");
        TestHarness::check(s.completion == 0, "invalid: 负 prompt 时 completion 也清零");
    }
    // (d) 非数值类型（字符串/null）——toDouble 对它们返回 0.0
    {
        QJsonObject usage;
        usage.insert(QStringLiteral("prompt_tokens"), QStringLiteral("2311"));
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(usage);
        TestHarness::check(!s.valid, "invalid: prompt 为字符串判无效");
    }
    {
        QJsonObject usage;
        usage.insert(QStringLiteral("prompt_tokens"), QJsonValue::Null);
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(usage);
        TestHarness::check(!s.valid, "invalid: prompt 为 null 判无效");
    }
    // (e) 关键断言：无效读数不进账——calls 不增、分母不虚增
    //     这条破了，UI 就会把「没数据」显示成「0 消耗」，用户读成「没花钱」。
    {
        UsageTokens::Totals totals;
        totals.add(UsageTokens::fromUsage(QJsonObject()));
        totals.add(UsageTokens::fromUsage(minimalUsage(0, 999)));
        totals.add(UsageTokens::fromUsage(minimalUsage(-1, 999)));
        TestHarness::check(totals.isEmpty(), "invalid: 全无效读数后仍为空账");
        TestHarness::check(totals.calls == 0, "invalid: 无效读数不计数");
        TestHarness::check(totals.prompt == 0 && totals.completion == 0,
                           "invalid: 无效读数不污染任何字段");
        TestHarness::check(totals.hitRatio() == 0.0, "invalid: 空账命中率 0（不是 1、不是 NaN）");
    }
    // (f) 只有 completion、没有 prompt：仍判无效（输入侧无真值 = 整条不可信）
    {
        QJsonObject usage;
        usage.insert(QStringLiteral("completion_tokens"), 500);
        UsageTokens::Totals totals;
        totals.add(UsageTokens::fromUsage(usage));
        TestHarness::check(totals.isEmpty(), "invalid: 缺 prompt 的读数整条丢弃");
    }
}

// ---- 3. 嵌套 details 缺失是常态，不是异常 ----

void testMissingDetailsAreZero()
{
    // 只有顶层两键（部分兼容端点的真实形态）：cached/reasoning 记 0，但读数**仍有效**
    const UsageTokens::Snapshot s = UsageTokens::fromUsage(minimalUsage(1000, 200));
    TestHarness::check(s.valid, "details: 缺嵌套仍判有效（顶层 prompt 是真值）");
    TestHarness::check(s.cached == 0, "details: 缺 prompt_tokens_details → cached=0");
    TestHarness::check(s.reasoning == 0, "details: 缺 completion_tokens_details → reasoning=0");

    // details 存在但目标键缺失
    {
        QJsonObject usage = minimalUsage(1000, 200);
        usage.insert(QStringLiteral("prompt_tokens_details"),
                     QJsonObject{{QStringLiteral("text_tokens"), 1000}});
        const UsageTokens::Snapshot d = UsageTokens::fromUsage(usage);
        TestHarness::check(d.valid && d.cached == 0, "details: 有 details 无 cached 键 → 0");
    }
    // details 是脏类型（字符串而非对象）→ toObject() 给空对象 → 0，不炸
    {
        QJsonObject usage = minimalUsage(1000, 200);
        usage.insert(QStringLiteral("prompt_tokens_details"), QStringLiteral("oops"));
        const UsageTokens::Snapshot d = UsageTokens::fromUsage(usage);
        TestHarness::check(d.valid && d.cached == 0, "details: details 为字符串 → cached=0 不炸");
    }
    // 命中率 0 与「无缓存」在算术上同值，UI 靠 calls>0 区分「有账但没命中」和「没账」
    {
        UsageTokens::Totals totals;
        totals.add(s);
        TestHarness::check(!totals.isEmpty(), "details: 无缓存读数照样入账");
        TestHarness::check(totals.hitRatio() == 0.0, "details: 无缓存命中率 0%");
    }
}

// ---- 4. 钳位：脏数据不让比率越界 ----

void testClamping()
{
    // cached > prompt（实测个别兼容端点会这样报）：比率钳到 1.0，未命中钳到非负
    {
        UsageTokens::Totals totals;
        totals.add(UsageTokens::fromUsage(fullUsage(1000, 100, 1500, 0)));
        TestHarness::check(totals.prompt == 1000 && totals.cached == 1500,
                           "clamp: 原值不篡改（异常仍可查）");
        TestHarness::check(totals.hitRatio() == 1.0, "clamp: 命中率上界钳 1.0（不显示 150%）");
        TestHarness::check(totals.uncachedPrompt() == 0, "clamp: 未命中不为负");
    }
    // 负 cached 在解析期已归 0，不会把命中率拉成负数
    {
        const UsageTokens::Snapshot s = UsageTokens::fromUsage(fullUsage(1000, 100, -50, -20));
        TestHarness::check(s.valid && s.cached == 0 && s.reasoning == 0,
                           "clamp: 嵌套负值归 0");
        UsageTokens::Totals totals;
        totals.add(s);
        TestHarness::check(totals.hitRatio() == 0.0, "clamp: 命中率不为负");
    }
}

// ---- 5. 累加语义：只增不减，跨多次回读求和 ----

void testAccumulation()
{
    UsageTokens::Totals totals;
    totals.add(UsageTokens::fromUsage(fullUsage(1000, 100, 800, 40)));
    totals.add(UsageTokens::fromUsage(fullUsage(2000, 200, 1000, 60)));
    totals.add(UsageTokens::fromUsage(QJsonObject())); // 中间夹一次无效：跳过，不影响其余
    totals.add(UsageTokens::fromUsage(fullUsage(3000, 300, 3000, 0)));

    TestHarness::check(totals.calls == 3, "accum: 三次有效回读（无效那次不计数）");
    TestHarness::check(totals.prompt == 6000, "accum: prompt 求和");
    TestHarness::check(totals.completion == 600, "accum: completion 求和");
    TestHarness::check(totals.cached == 4800, "accum: cached 求和");
    TestHarness::check(totals.reasoning == 100, "accum: reasoning 求和");
    // 4800/6000 = 0.8——命中率是**跨请求加权**（按 token 量），不是三次比率的算术平均
    TestHarness::check(qAbs(totals.hitRatio() - 0.8) < 1e-9, "accum: 命中率按 token 加权");
    TestHarness::check(totals.uncachedPrompt() == 1200, "accum: 未命中 = 总 prompt - 总 cached");
}

// ---- 6. 大数与类型宽度 ----

void testLargeValues()
{
    // 长会话累计可远超 int32（21 亿）；qint64 必须容得下，且求和不溢出
    UsageTokens::Totals totals;
    totals.add(UsageTokens::fromUsage(fullUsage(3e9, 2e9, 1e9, 5)));
    totals.add(UsageTokens::fromUsage(fullUsage(3e9, 2e9, 1e9, 5)));
    TestHarness::check(totals.prompt == 6000000000LL, "large: prompt 跨 int32 不截断");
    TestHarness::check(totals.completion == 4000000000LL, "large: completion 跨 int32 不截断");
    TestHarness::check(totals.cached == 2000000000LL, "large: cached 累加正确");
    TestHarness::check(qAbs(totals.hitRatio() - 1.0 / 3.0) < 1e-9, "large: 比率仍正确");
}

// ---- 7. 真实响应全形态（含本头不消费的键）：多余键不得干扰解析 ----

void testRealWorldPayload()
{
    // 逐字段照抄实测 DashScope 兼容模式响应（qwen3.8-flash，同前缀第二次请求）
    QJsonObject usage;
    usage.insert(QStringLiteral("prompt_tokens"), 2311);
    usage.insert(QStringLiteral("total_tokens"), 2365);
    usage.insert(QStringLiteral("completion_tokens"), 54);
    usage.insert(QStringLiteral("prompt_tokens_details"),
                 QJsonObject{{QStringLiteral("cached_tokens"), 2048},
                             {QStringLiteral("text_tokens"), 2311}});
    usage.insert(QStringLiteral("completion_tokens_details"),
                 QJsonObject{{QStringLiteral("reasoning_tokens"), 50},
                             {QStringLiteral("text_tokens"), 4}});

    const UsageTokens::Snapshot s = UsageTokens::fromUsage(usage);
    TestHarness::check(s.valid, "real: 真实响应判有效");
    TestHarness::check(s.prompt == 2311 && s.completion == 54, "real: 顶层键正确");
    TestHarness::check(s.cached == 2048 && s.reasoning == 50, "real: 嵌套目标键正确");
    // total_tokens 本头不消费（可由 prompt+completion 派生）；此处只确认它的存在不影响解析
    TestHarness::check(s.prompt + s.completion == 2365, "real: 与 total_tokens 自洽");
}

} // namespace

int tst_usagetokens()
{
    const int before = TestHarness::failCount();
    testHappyPath();
    testInvalidReadingsRejected();
    testMissingDetailsAreZero();
    testClamping();
    testAccumulation();
    testLargeValues();
    testRealWorldPayload();
    return TestHarness::failCount() - before;
}
