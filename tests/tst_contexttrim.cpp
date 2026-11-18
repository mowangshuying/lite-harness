// tst_contexttrim.cpp —— 上下文成组裁剪内核直测（src/ContextTrim.h，header-only 零 CMake 改动）
// 被测：trimConversation 成组裁剪 / keepHead 头部保护 / 目标比常量
//
// 本套件的存在意义：内核原先长在 TeammateRuntime.cpp（QObject + QOpenAi 依赖链进不了
// ctest），配对不变量——assistant 带 tool_calls 必须连同其后连续 tool 结果整组删，孤立删
// tool 会破坏 OpenAI 形态配对（上游 400）——是安全关键却零直测。子代理复用若复制一份即开
// 第二处漂移源。抽成纯头后两侧一行转发，本套件把判据钉成断言：改配对/头部保护/目标比即转红。
//
// 覆盖面边界（勿高估）：测的是**裁剪内核算术与结构不变量**，不是 SubAgent/TeammateRuntime
// 的压缩触发接线（何时调、预算多少、锚作废、反应式恰一次）——那些在 QObject + QOpenAi TU，
// 进不了 ctest，验证手段仍是编译期等价 + 冒烟。currentTokens 由调用方传入，本套件一律用
// CompactTokens::conversationTokens 的纯会话体口径（无 overhead），故减法为数学恒等，精确
// 停点断言成立；生产侧锚定调用方的混合口径等价性由抽头前后逐值一致保证（见 ContextTrim.h）。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "ContextTrim.h"
#include "CompactTokens.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace {

// ---- OpenAI 形态消息构造器（与生产一致：role/content，tool 带 tool_call_id）----
QJsonObject msg(const QString &role, const QString &content)
{
    QJsonObject o;
    o.insert(QStringLiteral("role"), role);
    o.insert(QStringLiteral("content"), content);
    return o;
}
QJsonObject sys(const QString &c) { return msg(QStringLiteral("system"), c); }
QJsonObject user(const QString &c) { return msg(QStringLiteral("user"), c); }
QJsonObject asst(const QString &c) { return msg(QStringLiteral("assistant"), c); }
QJsonObject tool(const QString &c, const QString &id = QStringLiteral("call_0"))
{
    QJsonObject o = msg(QStringLiteral("tool"), c);
    o.insert(QStringLiteral("tool_call_id"), id);
    return o;
}
// 带 tool_calls 的 assistant（n 个调用）——成组删除的单元头
QJsonObject asstCalls(const QString &c, int n = 1)
{
    QJsonObject o = msg(QStringLiteral("assistant"), c);
    QJsonArray calls;
    for (int i = 0; i < n; ++i) {
        QJsonObject fn;
        fn.insert(QStringLiteral("name"), QStringLiteral("bash"));
        fn.insert(QStringLiteral("arguments"), QStringLiteral("{}"));
        QJsonObject call;
        call.insert(QStringLiteral("id"), QStringLiteral("call_%1").arg(i));
        call.insert(QStringLiteral("type"), QStringLiteral("function"));
        call.insert(QStringLiteral("function"), fn);
        calls.append(call);
    }
    o.insert(QStringLiteral("tool_calls"), calls);
    return o;
}

QString big(int reps = 200) { return QStringLiteral("abcdefghij").repeated(reps); }
QString small() { return QStringLiteral("ok"); }

// 配对完整性判据：每个极大连续 tool 段必须紧跟在一个带 tool_calls 的 assistant 之后；
// 扫描到未被 preceding assistant 收编的 tool 即孤立（返回 false）。只删不重组的内核
// 对良构输入必须恒保持此性质。
bool pairingIntact(const QVector<QJsonObject> &m)
{
    qsizetype i = 0;
    while (i < m.size()) {
        const QJsonObject &o = m.at(i);
        const QString role = o.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("assistant") && o.contains(QLatin1String("tool_calls"))) {
            ++i;
            while (i < m.size()
                   && m.at(i).value(QStringLiteral("role")).toString() == QLatin1String("tool"))
                ++i;
        } else if (role == QLatin1String("tool")) {
            return false; // 孤立 tool
        } else {
            ++i;
        }
    }
    return true;
}

// ---- 退化边界：空 / keepHead>=size / current<=target 一律不动 ----
void testNoOpBoundaries()
{
    QVector<QJsonObject> empty;
    TestHarness::check(!ContextTrim::trimConversation(empty, 0, 0, 0),
                       "空会话裁剪返回 false");
    TestHarness::check(empty.isEmpty(), "空会话不被改动");

    QVector<QJsonObject> one{user(small())};
    TestHarness::check(!ContextTrim::trimConversation(one, 1,
                           CompactTokens::conversationTokens(one), 0),
                       "keepHead>=size 无可删单元，返回 false");
    TestHarness::check(one.size() == 1, "keepHead>=size 不改动");

    QVector<QJsonObject> two{sys(small()), user(small())};
    TestHarness::check(!ContextTrim::trimConversation(two, 2,
                           CompactTokens::conversationTokens(two), 0),
                       "keepHead=2 恰为头部两条，返回 false");
    TestHarness::check(two.size() == 2, "头部恰满时不改动");

    QVector<QJsonObject> three{sys(small()), user(small()), asst(big())};
    const qsizetype tot = CompactTokens::conversationTokens(three);
    TestHarness::check(!ContextTrim::trimConversation(three, 2, tot, tot),
                       "current==target 不触发（判据是 > 而非 >=）");
    TestHarness::check(three.size() == 3, "current==target 不改动");
    TestHarness::check(!ContextTrim::trimConversation(three, 2, tot, tot + 100),
                       "current<target 不触发");
    TestHarness::check(three.size() == 3, "current<target 不改动");
}

// ---- 目标比常量钉死（4/5 主动、1/2 反应式，两消费点共用防漂移）----
void testRatioConstantsPinned()
{
    TestHarness::check(ContextTrim::kCompactTargetRatioNumerator == 4,
                       "主动压缩目标分子 == 4");
    TestHarness::check(ContextTrim::kCompactTargetRatioDenominator == 5,
                       "主动压缩目标分母 == 5");
    TestHarness::check(ContextTrim::kReactiveTargetRatioNumerator == 1,
                       "反应式压缩目标分子 == 1");
    TestHarness::check(ContextTrim::kReactiveTargetRatioDenominator == 2,
                       "反应式压缩目标分母 == 2");

    const qsizetype budget = 1000;
    const qsizetype proactive = budget * ContextTrim::kCompactTargetRatioNumerator
                                / ContextTrim::kCompactTargetRatioDenominator;
    const qsizetype reactive = budget * ContextTrim::kReactiveTargetRatioNumerator
                               / ContextTrim::kReactiveTargetRatioDenominator;
    TestHarness::check(proactive == 800, "主动目标 = 预算×4/5 = 800");
    TestHarness::check(reactive == 500, "反应式目标 = 预算×1/2 = 500");
    TestHarness::check(reactive < proactive, "反应式目标更激进（溢出后留余量）");
    TestHarness::check(proactive < budget, "主动目标低于满额预算");
}

// ---- keepHead 头部恒保留（无论头部多大、目标多低）----
void testKeepHeadProtected()
{
    // keepHead=2：system[0]+任务 prompt[1]，即便 prompt 很大也不删
    QVector<QJsonObject> m{sys(small()), user(big()), asst(big()), asst(big())};
    const bool changed = ContextTrim::trimConversation(
        m, 2, CompactTokens::conversationTokens(m), 0);
    TestHarness::check(changed, "target=0 有可删单元时返回 true");
    TestHarness::check(m.size() == 2, "target=0 删尽可删单元，只剩头部两条");
    TestHarness::check(m.at(0).value(QStringLiteral("role")).toString() == QLatin1String("system"),
                       "system[0] 恒保留");
    TestHarness::check(m.at(1).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "任务 prompt[1] 恒保留（大内容也不删）");

    // keepHead=1：队友任务卡形态
    QVector<QJsonObject> n{user(big()), asst(big()), asst(big())};
    const bool ch2 = ContextTrim::trimConversation(
        n, 1, CompactTokens::conversationTokens(n), 0);
    TestHarness::check(ch2, "keepHead=1 target=0 返回 true");
    TestHarness::check(n.size() == 1, "keepHead=1 只剩任务卡");
    TestHarness::check(n.at(0).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "任务卡[0] 恒保留");
}

// ---- oldest-first：只删最旧单元，新者保留且相对顺序不变 ----
void testOldestFirstAndOrderPreserved()
{
    QVector<QJsonObject> m{sys(small()), user(small()), asst(big()), asst(small()), user(small())};
    const qsizetype total = CompactTokens::conversationTokens(m);
    // 目标恰 = 删掉最旧 assistant 后的幸存总量 → 删一组即停
    const QVector<QJsonObject> survivors{m.at(0), m.at(1), m.at(3), m.at(4)};
    const qsizetype target = CompactTokens::conversationTokens(survivors);

    const bool changed = ContextTrim::trimConversation(m, 2, total, target);
    TestHarness::check(changed, "超目标触发裁剪");
    TestHarness::check(m.size() == 4, "只删最旧的一条（oldest-first，非 newest-first）");
    TestHarness::check(m.at(2).value(QStringLiteral("content")).toString() == small(),
                       "幸存最旧位是原次旧 assistant（最旧大条被删）");
    TestHarness::check(m.at(3).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "最新 user 保留");
    TestHarness::check(pairingIntact(m), "裁剪后配对完整、相对顺序不变");
}

// ---- 成组配对：assistant 带 tool_calls 连同其后连续 tool 整组删（精确停点）----
void testGroupPairingWholeUnit()
{
    QVector<QJsonObject> m{
        sys(small()), user(small()),
        asstCalls(small()), tool(big()), tool(big()), tool(big()),
        asst(small()), user(small())};
    const qsizetype total = CompactTokens::conversationTokens(m);
    // 目标恰 = 删掉首个 assistant+3tool 组后的幸存总量 → 删该组即停
    const QVector<QJsonObject> survivors{m.at(0), m.at(1), m.at(6), m.at(7)};
    const qsizetype target = CompactTokens::conversationTokens(survivors);

    const bool changed = ContextTrim::trimConversation(m, 2, total, target);
    TestHarness::check(changed, "超目标触发成组裁剪");
    TestHarness::check(m.size() == 4, "恰好删掉 assistant+3tool 一组（4 条）即停");
    TestHarness::check(pairingIntact(m), "裁剪后无孤立 tool（配对完整）");
    bool anyTool = false;
    for (const QJsonObject &o : m)
        if (o.value(QStringLiteral("role")).toString() == QLatin1String("tool"))
            anyTool = true;
    TestHarness::check(!anyTool, "三个 tool 结果随其 assistant 整组删除（无残留）");
    TestHarness::check(m.at(2).value(QStringLiteral("role")).toString() == QLatin1String("assistant")
                           && !m.at(2).contains(QStringLiteral("tool_calls")),
                       "幸存首条是无 tool_calls 的 assistant（未被误并入前组）");
    TestHarness::check(m.at(3).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "末条 user 保留");
}

// ---- 连续两组：各 assistant+tool 独立成组，逐组删（精确停点）----
void testTwoConsecutiveGroups()
{
    QVector<QJsonObject> m{
        sys(small()), user(small()),
        asstCalls(small()), tool(big()),
        asstCalls(small()), tool(big()),
        user(small())};
    const qsizetype total = CompactTokens::conversationTokens(m);
    const QVector<QJsonObject> survivors{m.at(0), m.at(1), m.at(6)};
    const qsizetype target = CompactTokens::conversationTokens(survivors);

    const bool changed = ContextTrim::trimConversation(m, 2, total, target);
    TestHarness::check(changed, "两组均超目标时触发");
    TestHarness::check(m.size() == 3, "两组各 assistant+tool 整组删除，末条 user 保留");
    TestHarness::check(m.at(2).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "幸存末条是 user");
    TestHarness::check(pairingIntact(m), "两组删尽后配对完整");
}

// ---- 孤立 tool 兜底：扫描位上的 tool 跳过不删，裁剪越过它继续 ----
void testOrphanToolSkipped()
{
    // keepHead=1，[1] 是孤立 tool（其前无带 tool_calls 的 assistant）
    QVector<QJsonObject> m{user(small()), tool(big()), asst(big()), user(small())};
    const bool changed = ContextTrim::trimConversation(
        m, 1, CompactTokens::conversationTokens(m), 0);
    TestHarness::check(changed, "有可删单条单元时返回 true");
    TestHarness::check(m.size() == 2, "删掉 assistant 与末条 user，孤立 tool 与任务卡保留");
    TestHarness::check(m.at(0).value(QStringLiteral("role")).toString() == QLatin1String("user"),
                       "keepHead=1 保住 [0]");
    TestHarness::check(m.at(1).value(QStringLiteral("role")).toString() == QLatin1String("tool"),
                       "扫描位孤立 tool 被跳过不删（畸形历史兜底，绝不孤立删 tool）");
}

// ---- 幂等：已达头部后再裁同目标返回 false、不改动 ----
void testIdempotent()
{
    QVector<QJsonObject> m{sys(small()), user(small()), asst(big()), asst(big()), user(small())};
    const bool first = ContextTrim::trimConversation(
        m, 2, CompactTokens::conversationTokens(m), 0);
    TestHarness::check(first, "首次裁剪动过");
    const qsizetype sizeAfter = m.size();
    const bool second = ContextTrim::trimConversation(
        m, 2, CompactTokens::conversationTokens(m), 0);
    TestHarness::check(!second, "已达头部，再裁同目标返回 false（幂等）");
    TestHarness::check(m.size() == sizeAfter, "幂等：二次裁剪不改动");
}

// ---- changed 旗标当且仅当真的删了条 ----
void testChangedFlagIffRemoved()
{
    QVector<QJsonObject> m{sys(small()), user(small()), asst(small())};
    const qsizetype before = m.size();
    const qsizetype tot = CompactTokens::conversationTokens(m);
    const bool changed = ContextTrim::trimConversation(m, 2, tot, tot); // 相等不触发
    TestHarness::check(!changed && m.size() == before,
                       "未删任何条时 changed=false（旗标与删除严格对应）");
}

} // namespace

int tst_contexttrim()
{
    const int before = TestHarness::failCount();
    testNoOpBoundaries();
    testRatioConstantsPinned();
    testKeepHeadProtected();
    testOldestFirstAndOrderPreserved();
    testGroupPairingWholeUnit();
    testTwoConsecutiveGroups();
    testOrphanToolSkipped();
    testIdempotent();
    testChangedFlagIffRemoved();
    return TestHarness::failCount() - before;
}