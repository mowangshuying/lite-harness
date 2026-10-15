// tst_compacttokens.cpp —— 会话 token 计量单源直测（src/CompactTokens.h，header-only 零 CMake 改动）
// 被测：messageTokens 单条项 / conversationTokens 逐条求和 / **增量恒等式**
//
// 本套件的存在意义（不是为覆盖率凑数，是结构篇 §9.9 第 9 条整改的安全前提）：
// microCompact 与 fitToolResults 原为「循环内每轮对整段历史全量重估」= O(n²)，且跑在
// GUI 线程（零线程约定），表现为流式输出期可感知卡顿。改「循环前算一次总量、每次替换
// 做 total += new - old」之所以是**行为等价**而非近似，完全依赖一条性质：
//     conversationTokens 是 messageTokens 的逐条独立求和（项间零耦合）。
// 该性质一旦破（例如改成整段一次序列化、引入分隔符或跨条累计），增量维护会让压缩
// 触发点静默漂移 → 行为回归。故此处把它钉成断言：改口径即转红。
//
// 覆盖面边界（勿高估，与 §9.5 同口径）：本套件测的是**计量算术**，不是 microCompact/
// fitToolResults 的循环本体——后者是 CompactManager 私有方法，且该 TU 有 5 处依赖
// QOpenAi::AsyncRequest，违反「仅允许 QtCore-only 引擎进测试清单」（CMakeLists.txt:128），
// 进不了 ctest。testReplaceSequenceMatchesFullRecompute 用同款算术模拟那两个循环的
// 替换序列，是「算术等价」的证明，不是「循环控制流等价」的证明；后者的验证手段仍是
// 编译期等价 + 冒烟运行（清单篇压缩组）。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "CompactTokens.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>

namespace {

// 构造一条 OpenAI 形态的工具结果消息（与生产形态一致：role/content/tool_call_id）
QJsonObject toolMessage(const QString &content, const QString &id = QStringLiteral("call_1"))
{
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("tool"));
    message.insert(QStringLiteral("content"), content);
    message.insert(QStringLiteral("tool_call_id"), id);
    return message;
}

// 长 ASCII 文本（4:1 折算域）
QString asciiText(int repetitions)
{
    return QStringLiteral("abcdefghij").repeated(repetitions);
}

// 长 CJK 文本（1:1 折算域）
QString cjkText(int repetitions)
{
    return QStringLiteral("中文短语串").repeated(repetitions);
}

// ---- 空会话与单条：求和的退化边界 ----
void testEmptyAndSingleton()
{
    const QVector<QJsonObject> empty;
    TestHarness::check(CompactTokens::conversationTokens(empty) == 0,
                       "空会话总量为 0（求和单位元，非 -1 非未定义）");

    const QJsonObject single = toolMessage(QStringLiteral("hello"));
    const QVector<QJsonObject> one{single};
    TestHarness::check(CompactTokens::conversationTokens(one) ==
                           CompactTokens::messageTokens(single),
                       "单条会话总量 == 该条单项（退化到一项时两口径重合）");

    TestHarness::check(CompactTokens::messageTokens(single) > 0,
                       "非空单条估算为正（防口径退化成恒 0 让压缩永不触发）");
}

// ---- 核心性质①：逐条独立求和（可加性）----
void testAdditivity()
{
    const QVector<QJsonObject> conv{
        toolMessage(asciiText(40), QStringLiteral("a")),
        toolMessage(cjkText(40), QStringLiteral("b")),
        toolMessage(QStringLiteral("短"), QStringLiteral("c")),
        toolMessage(QString(), QStringLiteral("d")),
    };

    qsizetype sum = 0;
    for (const QJsonObject &message : conv)
        sum += CompactTokens::messageTokens(message);
    TestHarness::check(CompactTokens::conversationTokens(conv) == sum,
                       "总量 == 各项之和（可加性：项间零耦合，无分隔符/无跨条累计）");

    // 排列不变性：可加性的直接推论。整段序列化口径下此断言仍可能成立，故只作辅助佐证，
    // 真正的判据是上面那条逐项求和等式。
    QVector<QJsonObject> reversed = conv;
    std::reverse(reversed.begin(), reversed.end());
    TestHarness::check(CompactTokens::conversationTokens(conv) ==
                           CompactTokens::conversationTokens(reversed),
                       "总量与条序无关（可加性推论：置换不变）");
}

// ---- 核心性质②：单条项与位置无关 ----
void testTermIsPositionIndependent()
{
    const QJsonObject target = toolMessage(asciiText(25), QStringLiteral("t"));
    const QJsonObject other = toolMessage(cjkText(30), QStringLiteral("o"));

    const QVector<QJsonObject> head{target, other};
    const QVector<QJsonObject> tail{other, target};
    TestHarness::check(CompactTokens::conversationTokens(head) ==
                           CompactTokens::conversationTokens(tail),
                       "同两条消息换序后总量不变（单条项不看邻居与位置）");

    const QVector<QJsonObject> alone{target};
    TestHarness::check(CompactTokens::conversationTokens(alone) ==
                           CompactTokens::messageTokens(target),
                       "单条总量 == 该项，与它在长会话中的项相同");
}

// ---- 核心性质③：替换恒等式（增量维护的成立条件）----
void testReplaceIdentity()
{
    struct Case
    {
        QString name;
        QString before;
        QString after;
    };
    // 覆盖折算域两侧 + JSON 转义面 + 代理对：任何一条上恒等式破，即口径不再逐条独立
    const QVector<Case> cases{
        {QStringLiteral("ASCII 长→短"), asciiText(60), asciiText(6)},
        {QStringLiteral("ASCII 短→长"), asciiText(3), asciiText(50)},
        {QStringLiteral("CJK 长→短"), cjkText(60), cjkText(5)},
        {QStringLiteral("CJK→ASCII 跨域"), cjkText(30), asciiText(30)},
        {QStringLiteral("任意→空 content"), asciiText(20), QString()},
        {QStringLiteral("空→任意 content"), QString(), cjkText(12)},
        {QStringLiteral("含引号与反斜杠"), QStringLiteral("say \"x\" \\ path\\to"),
         QStringLiteral("q")},
        {QStringLiteral("含换行与制表转义"), QStringLiteral("line1\nline2\ttail\r\n"),
         QStringLiteral("flat")},
        {QStringLiteral("代理对（非 BMP）"), QString::fromUtf8("\xF0\x9F\x98\x80"),
         QStringLiteral("plain")},
        {QStringLiteral("CJK+ASCII 混排"), cjkText(15) + asciiText(15),
         QStringLiteral("短")},
    };

    for (const Case &c : cases) {
        // 邻居消息：证明 delta 只落在被替换那条上，不受上下文影响
        QVector<QJsonObject> conv{
            toolMessage(cjkText(10), QStringLiteral("prev")),
            toolMessage(c.before, QStringLiteral("mid")),
            toolMessage(asciiText(10), QStringLiteral("next")),
        };
        const qsizetype totalBefore = CompactTokens::conversationTokens(conv);
        const qsizetype termBefore = CompactTokens::messageTokens(conv.at(1));

        conv[1] = toolMessage(c.after, QStringLiteral("mid"));
        const qsizetype totalAfter = CompactTokens::conversationTokens(conv);
        const qsizetype termAfter = CompactTokens::messageTokens(conv.at(1));

        // 这就是生产侧增量维护的那一行算术：必须与全量重算逐值相等
        TestHarness::check(totalAfter == totalBefore - termBefore + termAfter,
                           (QStringLiteral("替换恒等式成立：新总量 == 原总量-旧项+新项 [") +
                            c.name + QStringLiteral("]"))
                               .toUtf8()
                               .constData());

        // 邻居项必须分毫不动（增量维护只该改一项）
        TestHarness::check(
            CompactTokens::messageTokens(conv.at(0)) ==
                CompactTokens::messageTokens(toolMessage(cjkText(10), QStringLiteral("prev"))),
            (QStringLiteral("前邻项不受中条替换影响 [") + c.name + QStringLiteral("]"))
                .toUtf8()
                .constData());
    }
}

// ---- 模拟两个循环的替换序列：逐步增量 vs 每步全量重算 ----
// microCompact / fitToolResults 的算术形态：算一次总量，此后每替换一条做
// total += new - old，并在每步开头拿 total 与 targetTokens 比。这里用同款算术跑完整
// 序列，逐步与全量重算对表——只要有一步不等价，压缩触发点就会漂移。
void testReplaceSequenceMatchesFullRecompute()
{
    QVector<QJsonObject> conv;
    for (int i = 0; i < 12; ++i)
        conv.append(toolMessage(asciiText(30 - i) + cjkText(5), QStringLiteral("id_%1").arg(i)));

    qsizetype total = CompactTokens::conversationTokens(conv); // 循环前算一次
    TestHarness::check(total > 0, "初始总量为正（12 条长结果会话）");

    bool sawStep = false;
    for (qsizetype i = 0; i < conv.size(); ++i) {
        const qsizetype before = CompactTokens::messageTokens(conv.at(i));
        // 换成生产侧同款占位文案（Format B 形态）
        conv[i][QStringLiteral("content")] =
            QStringLiteral("[Earlier tool result saved at %1]").arg(QStringLiteral("p"));
        total += CompactTokens::messageTokens(conv.at(i)) - before; // 增量维护
        sawStep = true;

        // 每一步都对表：增量值必须恰等于此刻的全量重算值
        TestHarness::check(total == CompactTokens::conversationTokens(conv),
                           "增量 total 与全量重算逐步相等（替换序列中某步）");
    }
    TestHarness::check(sawStep, "替换序列确实跑满（防空循环假绿）");

    // 单调性：全部换短后总量必须下降（增量维护若漏更新某条，此处仍可能相等而蒙混）
    TestHarness::check(total < CompactTokens::conversationTokens(
                                    [&] {
                                        QVector<QJsonObject> fresh;
                                        for (int i = 0; i < 12; ++i)
                                            fresh.append(toolMessage(
                                                asciiText(30 - i) + cjkText(5),
                                                QStringLiteral("id_%1").arg(i)));
                                        return fresh;
                                    }()),
                       "全部结果换短后总量低于原始总量（增量维护未漏更新）");
}

// ---- 幂等与稳定：同输入同值（压缩门槛比较依赖可重复）----
void testDeterminism()
{
    const QJsonObject message = toolMessage(cjkText(20) + asciiText(20), QStringLiteral("s"));
    TestHarness::check(CompactTokens::messageTokens(message) ==
                           CompactTokens::messageTokens(message),
                       "同一条消息两次估算值相等（无隐藏状态/无随机）");

    const QVector<QJsonObject> conv{message, toolMessage(asciiText(9), QStringLiteral("x"))};
    const qsizetype first = CompactTokens::conversationTokens(conv);
    const qsizetype second = CompactTokens::conversationTokens(conv);
    TestHarness::check(first == second, "同一会话两次估算值相等");
    // 值语义不变性：conversationTokens 不得改动入参
    TestHarness::check(conv.size() == 2 &&
                           conv.at(0).value(QStringLiteral("content")).toString() ==
                               message.value(QStringLiteral("content")).toString(),
                       "conversationTokens 为只读，未改动入参会话");
}

// ---- 折算口径锚点：锁死「CJK 1:1 / 其余 4:1 向上取整」的序列化后口径 ----
// 这几条锚的是 AgentConst::estimateTokens 经 JSON 包裹后的实际观感，防有人「顺手」改
// 计量函数而没同步压缩门槛语义（改断言即转红，逼人来读这段注释）。
void testEstimationScale()
{
    // 纯 ASCII：JSON 形态 {"content":"...","role":"tool","tool_call_id":"s"}
    // 只断言量级关系，不硬刻绝对值——键顺序由 Qt 决定，刻死即假红。
    const QJsonObject tiny = toolMessage(QStringLiteral("abcd"), QStringLiteral("s"));
    const QJsonObject twice = toolMessage(QStringLiteral("abcdefgh"), QStringLiteral("s"));
    TestHarness::check(CompactTokens::messageTokens(twice) >
                           CompactTokens::messageTokens(tiny),
                       "ASCII 内容加倍 → 估算严格增大（4:1 折算生效且非恒等）");

    const QJsonObject cjk2 = toolMessage(QStringLiteral("中文"), QStringLiteral("s"));
    const QJsonObject cjk4 = toolMessage(QStringLiteral("中文中文"), QStringLiteral("s"));
    TestHarness::check(CompactTokens::messageTokens(cjk4) -
                               CompactTokens::messageTokens(cjk2) ==
                           2,
                       "CJK 增 2 码元 → 恰增 2 token（1:1 折算，非 4:1）");

    // 同码元数下 CJK 比 ASCII 贵：证明分类折算真的分流，而非统一按某比例
    const QJsonObject cjk6 = toolMessage(cjkText(6).left(6), QStringLiteral("s"));
    const QJsonObject ascii6 = toolMessage(QStringLiteral("abcdef"), QStringLiteral("s"));
    TestHarness::check(CompactTokens::messageTokens(cjk6) >
                           CompactTokens::messageTokens(ascii6),
                       "同码元数 CJK 估算高于 ASCII（分类折算分流生效）");
}

} // namespace

int tst_compacttokens()
{
    const int before = TestHarness::failCount();
    testEmptyAndSingleton();
    testAdditivity();
    testTermIsPositionIndependent();
    testReplaceIdentity();
    testReplaceSequenceMatchesFullRecompute();
    testDeterminism();
    testEstimationScale();
    return TestHarness::failCount() - before;
}
