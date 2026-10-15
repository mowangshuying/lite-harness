// tst_streamrate.cpp —— token 输出速度纯头直测（src/StreamRate.h，header-only 零 CMake 改动）
// 被测：窗口有效性门 / 单窗口速率 / Σtokens ÷ ΣgenerationMs 的聚合口径 / 无效窗口不污染分母
//
// 本套件为什么必须存在（不是凑覆盖率）：这条算术是要**显示给用户看**的。它比消耗账更脆，
// 因为分母是时间——时间窗口一旦取错，数字照样能算出来、照样能显示，但含义完全不同：
//   · 把「请求发出 → usage」当生成窗口，慢首字（TTFT）会被算进速率，用户读成「模型吐字慢」，
//     而真相是「等了 8 秒才见第一个字，之后每秒 60 token」；
//   · 把各窗口速率做算术平均，一条 2000 token 的长回答会被九条 1 token 的短回答稀释；
//   · 纯 tool_calls 回合（SSE delta 里只有 tool_calls，不触发 textDelta）没有增量起点，
//     窗口无从定义——此时任何数字都是把 TTFT 冒充生成速度。
// 这三条都是「算得出但算错」的路径，只能靠断言钉死，靠肉眼看不出来。
//
// 覆盖面边界（勿高估）：本套件测的是**定窗与折算算术**。它不证明 AgentLoop 真的按定义观测了
// t0/t1/t2——那是接线（AgentLoopRequest.cpp 的起表/标点/收窗），验证手段是编译期等价 + 冒烟运行。
// 宿主 TU 链 QOpenAi/FluentUI，按 CMakeLists.txt「仅允许 QtCore-only 引擎进测试清单」的硬纪律
// 进不了 ctest，故本头按 UsageTokens.h / CompactTokens.h 先例抽出，测真实编译产物而非复刻品。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "StreamRate.h"

#include <QtGlobal>

namespace {

using StreamRate::Meter;
using StreamRate::Window;
using UsageTokens::Snapshot;

// ---- 构造工具 ----

// 有效用量快照（走真实解析路径，不手抄字段）
Snapshot usageFrom(qsizetype prompt, qsizetype completion)
{
    QJsonObject usage;
    usage.insert(QStringLiteral("prompt_tokens"), double(prompt));
    usage.insert(QStringLiteral("completion_tokens"), double(completion));
    return UsageTokens::fromUsage(usage);
}

// 手工窗口（用于纯算术断言）
Window window(qsizetype tokens, qsizetype generationMs, qsizetype firstTokenMs = 0)
{
    Window w;
    w.tokens = tokens;
    w.generationMs = generationMs;
    w.firstTokenMs = firstTokenMs;
    w.valid = tokens > 0 && generationMs > 0;
    return w;
}

bool near(double a, double b)
{
    return qAbs(a - b) < 1e-9;
}

} // namespace

int tst_streamrate()
{
    const int before = TestHarness::failCount();

    // ---- 空账：不除零、不显示假 0 ----
    Meter empty;
    TestHarness::check(empty.isEmpty(), "empty meter reports isEmpty");
    TestHarness::check(near(empty.lastRate(), 0.0), "empty meter lastRate is 0");
    TestHarness::check(near(empty.averageRate(), 0.0), "empty meter averageRate is 0 (no div-by-zero)");
    TestHarness::check(near(empty.averageFirstTokenMs(), 0.0), "empty meter averageFirstTokenMs is 0");

    // ---- 无效窗口：算得出但不可信，一律不进分母 ----
    TestHarness::check(!window(0, 1000).valid, "zero-token window is invalid");
    TestHarness::check(!window(100, 0).valid, "zero-length window is invalid");
    TestHarness::check(!window(100, -500).valid, "negative window (usage before first delta) is invalid");
    TestHarness::check(near(StreamRate::tokensPerSecond(window(0, 0)), 0.0), "invalid window rate is 0, not NaN/inf");

    // ---- 单窗口算术 ----
    TestHarness::check(near(StreamRate::tokensPerSecond(window(100, 2000)), 50.0), "100 tok / 2000 ms = 50 tok/s");
    TestHarness::check(near(StreamRate::tokensPerSecond(window(1, 1000)), 1.0), "1 tok / 1000 ms = 1 tok/s");

    // ---- 定窗判据（接线侧只报时刻，判据在这里） ----
    // 正常：t0→t1 = 1000ms（TTFT），t0→t2 = 3000ms ⇒ 生成窗口 2000ms
    const Window ok = StreamRate::makeWindow(usageFrom(1000, 100), 1000, 3000);
    TestHarness::check(ok.valid, "well-formed window is valid");
    TestHarness::check(ok.generationMs == 2000, "generation window excludes TTFT (3000 - 1000 = 2000)");
    TestHarness::check(ok.firstTokenMs == 1000, "TTFT kept separate from the generation window");
    TestHarness::check(near(StreamRate::tokensPerSecond(ok), 50.0), "TTFT is not folded into the rate");

    // 无效读数：与 UsageTokens::fromUsage / adoptUsageAnchor 同一条判据（prompt<=0）
    TestHarness::check(!StreamRate::makeWindow(usageFrom(0, 100), 1000, 3000).valid,
                       "invalid usage snapshot yields invalid window (same gate as adoptUsageAnchor)");

    // 纯 tool_calls 回合：没有增量起点（firstDeltaMs<0），窗口无从定义
    TestHarness::check(!StreamRate::makeWindow(usageFrom(1000, 100), -1, 3000).valid,
                       "turn with no delta has no definable window (no TTFT masquerading as speed)");

    // ---- 聚合口径：Σtokens ÷ ΣgenerationMs，不是各窗口速率的算术平均 ----
    // 100 tok/1000ms（100 tok/s）+ 1 tok/9000ms（0.111 tok/s）：
    //   Σ/Σ = 101/10000 = 10.1 tok/s；算术平均 = 50.055 tok/s。两者差 5 倍，必须钉死。
    Meter m;
    m.add(window(100, 1000));
    m.add(window(1, 9000));
    TestHarness::check(m.calls == 2, "both valid windows counted");
    TestHarness::check(near(m.averageRate(), 10.1), "aggregate is sum/sum, not mean-of-rates");
    TestHarness::check(near(m.lastRate(), 0.11111111111111111), "lastRate reflects the last valid window");

    // 无效窗口不污染分母，也不改写 last
    m.add(window(0, 0));
    TestHarness::check(m.calls == 2, "invalid window does not enter the aggregate");
    TestHarness::check(near(m.lastRate(), 0.11111111111111111), "invalid window does not overwrite last");

    // 顺序无关：同一批窗口，聚合结果与到达顺序无关（last 才是顺序敏感的）
    Meter a;
    a.add(window(1, 9000));
    a.add(window(100, 1000));
    Meter b;
    b.add(window(100, 1000));
    b.add(window(1, 9000));
    TestHarness::check(near(a.averageRate(), b.averageRate()), "aggregate is order-independent");
    TestHarness::check(near(a.lastRate(), 100.0) && near(b.lastRate(), 0.11111111111111111),
                       "last is order-sensitive by design (UI shows the most recent turn)");

    // TTFT 均值口径：与速率分母无关，单独可查
    Meter t;
    t.add(window(100, 1000, 200));
    t.add(window(50, 1000, 400));
    TestHarness::check(near(t.averageFirstTokenMs(), 300.0), "averageFirstTokenMs is a mean over valid windows");

    // 长会话累加不得溢出（qsizetype 64 位；若哪天退回 int，这条会红）
    Meter big;
    for (int i = 0; i < 1000; ++i)
        big.add(window(1000, 1000));
    TestHarness::check(big.tokens == 1000000 && big.generationMs == 1000000, "long-session accumulation stays exact");
    TestHarness::check(near(big.averageRate(), 1000.0), "long-session aggregate stays exact (no int overflow)");

    return TestHarness::failCount() - before;
}
