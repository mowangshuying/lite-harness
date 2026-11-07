// tst_activityledger.cpp —— 会话每日 token 消耗账本纯头直测（src/ActivityLedger.h，
// header-only 零 CMake 改动即入 ctest，仿 tst_usagetokens / tst_compacttokens 先例）
//
// 判据族（覆盖新会话页热力图数据侧的三条静默失效面）：
//   · 色阶阈值边界——分档一旦漂，用户读到「今天颜色浅」会误判为「今天用得少」；
//   · valid=false 不落地——与 UsageTokens::Totals::add 同判据，把「端点没回 usage」
//     算成「0 消耗」是最阴的错误（数字看起来对，只是含义反了）；
//   · 坏 JSON / 脏数据降级——启动扫盘要跨 N 个会话目录，一个坏文件不该把整个热力图打挂。
//
// 分组：组 1~5 零夹具恒跑（阈值/日期键/累加/求和/截断，纯算术）；
// 组 6~7 需 LITE_TEST_TMPROOT（真文件读写往返、脏数据降级），缺失即 SKIP。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "ActivityLedger.h"
#include "UsageTokens.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>

namespace {

// ---- 构造工具 ----

UsageTokens::Snapshot validSnapshot(qint64 prompt, qint64 completion,
                                     qint64 cached = 0, qint64 reasoning = 0)
{
    UsageTokens::Snapshot s;
    s.prompt = prompt;
    s.completion = completion;
    s.cached = cached;
    s.reasoning = reasoning;
    s.valid = prompt > 0;
    return s;
}

// ---- 组 1：色阶阈值边界 ----

void testLevelThresholds()
{
    // 0 与负值一律档 0（无数据/脏数据同色，UI 不区分）
    TestHarness::check(ActivityLedger::levelForTokens(-1) == 0, "level: 负值档 0");
    TestHarness::check(ActivityLedger::levelForTokens(0) == 0, "level: 零档 0");
    // 边界：档 1 上界 4999、档 2 下界 5000
    TestHarness::check(ActivityLedger::levelForTokens(1) == 1, "level: 1 档 1");
    TestHarness::check(ActivityLedger::levelForTokens(4999) == 1, "level: 4999 档 1（档 1 上界）");
    TestHarness::check(ActivityLedger::levelForTokens(5000) == 2, "level: 5000 档 2（下界即进位）");
    TestHarness::check(ActivityLedger::levelForTokens(19999) == 2, "level: 19999 档 2");
    TestHarness::check(ActivityLedger::levelForTokens(20000) == 3, "level: 20000 档 3");
    TestHarness::check(ActivityLedger::levelForTokens(49999) == 3, "level: 49999 档 3");
    TestHarness::check(ActivityLedger::levelForTokens(50000) == 4, "level: 50000 档 4");
    TestHarness::check(ActivityLedger::levelForTokens(99999) == 4, "level: 99999 档 4");
    TestHarness::check(ActivityLedger::levelForTokens(100000) == 5, "level: 100000 档 5（顶档）");
    TestHarness::check(ActivityLedger::levelForTokens(9999999999LL) == 5, "level: 大值仍档 5");
}

// ---- 组 2：日期键格式往返 ----

void testDateKeyRoundtrip()
{
    const QDate d(2026, 11, 7);
    const QString key = ActivityLedger::dateKey(d);
    TestHarness::check(key == QStringLiteral("2026-11-07"), "dateKey: ISO 定值形态");
    const QDate back = ActivityLedger::dateFromKey(key);
    TestHarness::check(back == d, "dateKey: 往返等值");

    // 单双位数月/日补零（否则字典序乱、JSON 键集合不匹配）
    TestHarness::check(ActivityLedger::dateKey(QDate(2026, 1, 1)) == QStringLiteral("2026-01-01"),
                       "dateKey: 月/日补零");

    // 无效日期键解析回无效 QDate（不猜、不默认）
    TestHarness::check(!ActivityLedger::dateFromKey(QStringLiteral("")).isValid(),
                       "dateFromKey: 空串返无效日期");
    TestHarness::check(!ActivityLedger::dateFromKey(QStringLiteral("2026/11/07")).isValid(),
                       "dateFromKey: 斜杠写法不匹配 ISO");
    TestHarness::check(!ActivityLedger::dateFromKey(QStringLiteral("not-a-date")).isValid(),
                       "dateFromKey: 垃圾串返无效日期");
}

// ---- 组 3：累加判据（valid=false 不落地，与 Totals::add 同源）----

void testAccumulateSnapshot()
{
    // 有效快照：五项 + calls 全部累加
    {
        ActivityLedger::DayTotal day;
        ActivityLedger::accumulateSnapshot(day, validSnapshot(1000, 100, 500, 20));
        TestHarness::check(day.prompt == 1000 && day.completion == 100, "accum: 有效快照入账");
        TestHarness::check(day.cached == 500 && day.reasoning == 20, "accum: 嵌套字段入账");
        TestHarness::check(day.calls == 1, "accum: 有效快照计一次");
    }
    // 无效快照：整条忽略，calls 不增、五项不动
    // 破了这条，UI 会把「端点没回 usage」显示成「0 消耗」——用户读作「这轮没花钱」
    {
        ActivityLedger::DayTotal day;
        ActivityLedger::accumulateSnapshot(day, validSnapshot(500, 50));
        UsageTokens::Snapshot invalid; // 默认 valid=false
        ActivityLedger::accumulateSnapshot(day, invalid);
        TestHarness::check(day.prompt == 500 && day.calls == 1,
                           "accum: 无效快照不污染任何字段");
    }
    // prompt=0 但 completion>0（端点回半截）：判无效（valid 门在 Snapshot 侧）
    {
        UsageTokens::Snapshot half;
        half.prompt = 0;
        half.completion = 999;
        half.valid = false; // 承 UsageTokens::fromUsage 的判据（prompt>0 才有效）
        ActivityLedger::DayTotal day;
        ActivityLedger::accumulateSnapshot(day, half);
        TestHarness::check(day.calls == 0 && day.completion == 0,
                           "accum: prompt=0 快照整条作废");
    }
    // 多次累加
    {
        ActivityLedger::DayTotal day;
        ActivityLedger::accumulateSnapshot(day, validSnapshot(1000, 100));
        ActivityLedger::accumulateSnapshot(day, validSnapshot(2000, 200, 500, 0));
        ActivityLedger::accumulateSnapshot(day, validSnapshot(3000, 300));
        TestHarness::check(day.prompt == 6000 && day.completion == 600, "accum: 多次求和");
        TestHarness::check(day.cached == 500, "accum: cached 累加");
        TestHarness::check(day.calls == 3, "accum: calls 累加");
    }
}

// ---- 组 4：DayTotal::totalProcessed 口径 ----

void testTotalProcessedSemantics()
{
    // 关键：totalProcessed = prompt + completion，**不减 cached**
    // 因为 cached 是 prompt 的子集（服务端已按全价报进 prompt），减掉会低估实际处理量
    ActivityLedger::DayTotal day;
    day.prompt = 10000;
    day.completion = 500;
    day.cached = 8000;
    TestHarness::check(day.totalProcessed() == 10500,
                       "processed: prompt+completion 不减 cached");
    // 边界：全命中缓存（cached==prompt）仍按 prompt 计
    day.prompt = 1000;
    day.completion = 0;
    day.cached = 1000;
    TestHarness::check(day.totalProcessed() == 1000, "processed: 全命中不减");
    // 空账
    ActivityLedger::DayTotal empty;
    TestHarness::check(empty.totalProcessed() == 0, "processed: 空账返 0");
}

// ---- 组 5：跨会话求和 + 窗口截断 ----

void testAggregateAndWindow()
{
    const QDate d1(2026, 11, 1);
    const QDate d2(2026, 11, 2);
    const QDate d3(2026, 11, 3);

    // 两个会话，同一日贡献求和
    QList<QHash<QDate, ActivityLedger::DayTotal>> perSession;
    {
        QHash<QDate, ActivityLedger::DayTotal> s1;
        ActivityLedger::DayTotal a; a.prompt = 1000; a.completion = 100; a.calls = 1;
        s1.insert(d1, a);
        perSession.append(s1);
    }
    {
        QHash<QDate, ActivityLedger::DayTotal> s2;
        ActivityLedger::DayTotal b; b.prompt = 2000; b.completion = 200; b.calls = 2;
        s2.insert(d1, b);
        ActivityLedger::DayTotal c; c.prompt = 500; c.completion = 50; c.calls = 1;
        s2.insert(d2, c);
        perSession.append(s2);
    }
    const auto sum = ActivityLedger::aggregate(perSession);
    TestHarness::check(sum.contains(d1) && sum[d1].prompt == 3000,
                       "aggregate: 同日跨会话求和");
    TestHarness::check(sum[d1].completion == 300 && sum[d1].calls == 3,
                       "aggregate: completion/calls 求和");
    TestHarness::check(sum.contains(d2) && sum[d2].prompt == 500,
                       "aggregate: 单日独有键保留");
    TestHarness::check(!sum.contains(d3), "aggregate: 无贡献日不出现");

    // 空列表 → 空表
    TestHarness::check(ActivityLedger::aggregate({}).isEmpty(),
                       "aggregate: 空输入空输出");

    // 窗口截断：闭区间 [from, to]
    QHash<QDate, ActivityLedger::DayTotal> data;
    data.insert(d1, ActivityLedger::DayTotal{});
    data.insert(d2, ActivityLedger::DayTotal{});
    data.insert(d3, ActivityLedger::DayTotal{});
    const auto win = ActivityLedger::windowFilter(data, d1, d2);
    TestHarness::check(win.size() == 2, "window: 闭区间两端都含");
    TestHarness::check(win.contains(d1) && win.contains(d2) && !win.contains(d3),
                       "window: 边界判定");
    // from>to → 空
    TestHarness::check(ActivityLedger::windowFilter(data, d3, d1).isEmpty(),
                       "window: from>to 空表");
}

// ---- 组 6：真文件读写往返（需 LITE_TEST_TMPROOT）----

void testFileRoundtrip(const QString &dirPath)
{
    const QString path = QDir(dirPath).filePath(QStringLiteral("daily.json"));
    const QDate d1(2026, 11, 1);
    const QDate d2(2026, 11, 2);

    QHash<QDate, ActivityLedger::DayTotal> data;
    {
        ActivityLedger::DayTotal a;
        a.prompt = 12345; a.completion = 678; a.cached = 5000;
        a.reasoning = 100; a.calls = 3;
        data.insert(d1, a);
    }
    {
        ActivityLedger::DayTotal b;
        b.prompt = 100; b.completion = 20; b.calls = 1;
        data.insert(d2, b);
    }
    TestHarness::check(ActivityLedger::writeDaily(path, data), "file: 写成功");
    const auto read = ActivityLedger::readDaily(path);
    TestHarness::check(read.size() == 2, "file: 读回条数等");
    TestHarness::check(read[d1].prompt == 12345 && read[d1].completion == 678,
                       "file: 大整数往返不截断");
    TestHarness::check(read[d1].cached == 5000 && read[d1].reasoning == 100,
                       "file: 嵌套字段往返");
    TestHarness::check(read[d1].calls == 3, "file: calls 往返");
    TestHarness::check(read[d2].prompt == 100, "file: 第二条往返");

    // 空表写盘 → 读回空表（不是坏 JSON）
    const QString emptyPath = QDir(dirPath).filePath(QStringLiteral("empty.json"));
    QHash<QDate, ActivityLedger::DayTotal> emptyData;
    TestHarness::check(ActivityLedger::writeDaily(emptyPath, emptyData),
                       "file: 空表写成功");
    TestHarness::check(ActivityLedger::readDaily(emptyPath).isEmpty(),
                       "file: 空表读回空");

    // 覆盖写：第二次写不同数据，第一次数据不残留
    const QString overwritePath = QDir(dirPath).filePath(QStringLiteral("overwrite.json"));
    QHash<QDate, ActivityLedger::DayTotal> first;
    ActivityLedger::DayTotal x; x.prompt = 1000; first.insert(d1, x);
    TestHarness::check(ActivityLedger::writeDaily(overwritePath, first),
                       "file: 首次写成功");
    QHash<QDate, ActivityLedger::DayTotal> second;
    ActivityLedger::DayTotal y; y.prompt = 9999; second.insert(d2, y);
    TestHarness::check(ActivityLedger::writeDaily(overwritePath, second),
                       "file: 覆写成功");
    const auto after = ActivityLedger::readDaily(overwritePath);
    TestHarness::check(after.size() == 1 && after.contains(d2),
                       "file: 覆写整表替换（旧键不残留）");
}

// ---- 组 7：脏数据降级（需 LITE_TEST_TMPROOT）----

void testDirtyDataDegradation(const QString &dirPath)
{
    const QDate d1(2026, 11, 1);

    // 缺文件 → 空表（不抛、不猜）
    const QString missingPath = QDir(dirPath).filePath(QStringLiteral("nope.json"));
    TestHarness::check(ActivityLedger::readDaily(missingPath).isEmpty(),
                       "dirty: 缺文件返空表");

    // 坏 JSON → 空表
    const QString badJsonPath = QDir(dirPath).filePath(QStringLiteral("bad.json"));
    {
        QFile f(badJsonPath);
        TestHarness::check(f.open(QIODevice::WriteOnly), "dirty: 建坏 JSON 文件");
        f.write("{\"prompt\": 100,}");  // 尾逗号，非法 JSON
        f.close();
    }
    TestHarness::check(ActivityLedger::readDaily(badJsonPath).isEmpty(),
                       "dirty: 坏 JSON 返空表");

    // 顶层非对象 → 空表
    const QString arrayPath = QDir(dirPath).filePath(QStringLiteral("array.json"));
    {
        QFile f(arrayPath);
        f.open(QIODevice::WriteOnly);
        f.write("[1, 2, 3]");
        f.close();
    }
    TestHarness::check(ActivityLedger::readDaily(arrayPath).isEmpty(),
                       "dirty: 顶层数组返空表");

    const QString scalarPath = QDir(dirPath).filePath(QStringLiteral("scalar.json"));
    {
        QFile f(scalarPath);
        f.open(QIODevice::WriteOnly);
        f.write("42");
        f.close();
    }
    TestHarness::check(ActivityLedger::readDaily(scalarPath).isEmpty(),
                       "dirty: 顶层标量返空表");

    // 部分脏条目：坏键 / 坏值 / 负数 → 跳过或归零，好条目照常读回
    const QString mixedPath = QDir(dirPath).filePath(QStringLiteral("mixed.json"));
    {
        QJsonObject root;
        // 好条目
        QJsonObject good;
        good.insert(QStringLiteral("prompt"), 1000);
        good.insert(QStringLiteral("completion"), 100);
        good.insert(QStringLiteral("calls"), 1);
        root.insert(QStringLiteral("2026-11-01"), good);
        // 坏键（非 ISO 日期）
        root.insert(QStringLiteral("not-a-date"), good);
        // 坏值（值是数字不是对象）
        root.insert(QStringLiteral("2026-11-02"), 42);
        // 负数字段
        QJsonObject negative;
        negative.insert(QStringLiteral("prompt"), -500);
        negative.insert(QStringLiteral("completion"), 100);
        negative.insert(QStringLiteral("calls"), -1);
        root.insert(QStringLiteral("2026-11-03"), negative);
        QFile f(mixedPath);
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(root).toJson());
        f.close();
    }
    const auto mixed = ActivityLedger::readDaily(mixedPath);
    TestHarness::check(mixed.contains(d1), "dirty: 好条目照常读回");
    TestHarness::check(mixed.size() == 2, "dirty: 坏键/坏值跳过，好条目 + 负数条目 = 2");
    // 负数条目：prompt 归零、calls 归零、completion 保留
    const QDate d3(2026, 11, 3);
    TestHarness::check(mixed.contains(d3), "dirty: 负数条目仍入账（值归零）");
    TestHarness::check(mixed[d3].prompt == 0 && mixed[d3].calls == 0,
                       "dirty: 负数字段归零");
    TestHarness::check(mixed[d3].completion == 100,
                       "dirty: 负数条目内非负字段保留");
}

} // namespace

int tst_activityledger()
{
    const int before = TestHarness::failCount();

    // 恒跑组（零夹具）
    testLevelThresholds();
    testDateKeyRoundtrip();
    testAccumulateSnapshot();
    testTotalProcessedSemantics();
    testAggregateAndWindow();

    // 夹具组（需 LITE_TEST_TMPROOT）
    ScopedTempRoot root(QStringLiteral("activityledger"));
    if (!root.isValid()) {
        TestHarness::skip(
            "SKIP: LITE_TEST_TMPROOT unavailable, activity ledger file groups skipped\n");
        return TestHarness::failCount() - before;
    }
    testFileRoundtrip(root.path());
    testDirtyDataDegradation(root.path());

    return TestHarness::failCount() - before;
}
