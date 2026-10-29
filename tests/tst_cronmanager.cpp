// tst_cronmanager.cpp —— s12 定时任务引擎判据单测（s17 P1 观测面补强批次）
// 被测：src/CronSchedulerManager —— validateCron 报错族 / cronFieldMatches 逗号列表不命中
//       回归（647b22e）/ cronMatches Vixie 双限定 OR 语义 / ledgerSnapshot 台账快照（P1 观测面）
//
// 本套件零夹具：全部走 durable=false 路径（不触盘），QDateTime 构造输入，任何环境全量执行。
// 此前 cron 引擎判据直测为零（AGENTS「下一批最小切口」在册项），本次以观测面快照为引补齐。

#include "TestHarness.h"

#include "CronSchedulerManager.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTime>
#include <QString>

namespace {

// validateCron：合法形回空串；五类报错逐字族（"<字段名>: <错误>"，字段名与 lcc field_rules 同源）
void testValidateCron()
{
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("*/5 9-17 * * 1-5")).isEmpty(),
                       "复合合法式回空串");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 9 * *"))
                           == QStringLiteral("Expected 5 fields, got 4"),
                       "段数不足逐字串（lcc 同文）");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("60 * * * *"))
                           == QStringLiteral("minute: Value 60 is outside [0-59]"),
                       "越界报字段名前缀 + 域区间");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * 5-1 * *"))
                           == QStringLiteral("day-of-month: Range start is greater than end: 5-1"),
                       "区间倒挂报错");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * * * 7"))
                           == QStringLiteral("day-of-week: Value 7 is outside [0-6]"),
                       "day-of-week 上界 6");
}

// cronFieldMatches：逗号列表全不中必为假——647b22e 关键修复（勿回退"默认命中"）；
// */0 与非数字步长在 lite 静默 false（lcc 抛异常由 poll 捕获，登记偏差）
void testFieldMatches()
{
    TestHarness::check(!CronSchedulerManager::cronFieldMatches(QStringLiteral("1,2,3"), 5),
                       "逗号列表全不中 → false（647b22e 回归钉）");
    TestHarness::check(CronSchedulerManager::cronFieldMatches(QStringLiteral("1,2,3"), 2),
                       "逗号列表任一命中 → true");
    TestHarness::check(CronSchedulerManager::cronFieldMatches(QStringLiteral("*"), 42),
                       "星号恒真");
    TestHarness::check(CronSchedulerManager::cronFieldMatches(QStringLiteral("*/15"), 30)
                           && !CronSchedulerManager::cronFieldMatches(QStringLiteral("*/15"), 7),
                       "步长命中与不命中");
    TestHarness::check(!CronSchedulerManager::cronFieldMatches(QStringLiteral("*/0"), 0),
                       "*/0 折叠为不命中（lcc ZeroDivisionError 位，lite 静默 false）");
}

// cronMatches：双 *（日/周）恒真通道 与 Vixie 双限定 OR 语义（日与周都限定时任一命中即触发）
void testCronMatches()
{
    const QDateTime thuNoon(QDate(2026, 1, 1), QTime(12, 0)); // 2026-01-01 = 周四（Qt dow=4，cron 4）
    TestHarness::check(QDate(2026, 1, 1).dayOfWeek() == 4,
                       "夹具日期自证：2026-01-01 是周四（本套件其余断言依赖此事实）");
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 12 1 * 4"), thuNoon),
                       "双限定且双命中 → 真");
    // 同表达式对 2026-01-08（周四、非 1 号）：日不中但周中 → Vixie OR 仍触发
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 12 1 * 4"),
                                                         QDateTime(QDate(2026, 1, 8), QTime(12, 0))),
                       "双限定单侧命中 → 真（Vixie OR 语义）");
    // 2026-01-02 周五：日、周皆不中 → 假
    TestHarness::check(!CronSchedulerManager::cronMatches(QStringLiteral("0 12 1 * 4"),
                                                          QDateTime(QDate(2026, 1, 2), QTime(12, 0))),
                       "双限定全不中 → 假");
    // 单限定（只有日）：只看限定侧
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 13 * *"),
                                                         QDateTime(QDate(2026, 8, 13), QTime(0, 0))),
                       "日限定 + 周 * → 只看日");
    TestHarness::check(!CronSchedulerManager::cronMatches(QStringLiteral("*/15 * * * *"),
                                                          QDateTime(QDate(2026, 8, 13), QTime(7, 14))),
                       "分钟不中 → 假（分/时/月 AND 通道）");
}

// ledgerSnapshot（s17 P1 观测面）：durable=false 全程不触盘；字段齐形、取消即离账、
// 拒绝形不入账
void testLedgerSnapshot()
{
    CronSchedulerManager mgr([] { return QStringLiteral("unused"); });
    TestHarness::check(mgr.ledgerSnapshot().isEmpty(), "空台账快照为空数组");

    CronSchedulerManager::CronJob out;
    const QString bad = mgr.scheduleJob(QStringLiteral("bad"), QStringLiteral("p"), true, false, &out);
    TestHarness::check(!bad.isEmpty() && mgr.ledgerSnapshot().isEmpty(),
                       "坏表达式被拒且不入台账（快照先于入账即干净）");
    const QString noPrompt = mgr.scheduleJob(QStringLiteral("* * * * *"), QStringLiteral("  "),
                                             true, false, &out);
    TestHarness::check(noPrompt == QStringLiteral("prompt cannot be empty"),
                       "空 prompt 逐字拒绝串");

    const QString err = mgr.scheduleJob(QStringLiteral("0 9 * * 1"),
                                        QStringLiteral("weekly standup"),
                                        true, false, &out);
    TestHarness::check(err.isEmpty() && !out.id.isEmpty(), "登记成功回空串并经 out 回填副本");

    const QJsonArray snap = mgr.ledgerSnapshot();
    TestHarness::check(snap.size() == 1, "一条任务快照一行");
    const QJsonObject row = snap.at(0).toObject();
    TestHarness::check(row.value(QStringLiteral("id")).toString().startsWith(QLatin1String("cron_")),
                       "id 形如 cron_<hex8>");
    TestHarness::check(row.value(QStringLiteral("cron")).toString() == QStringLiteral("0 9 * * 1")
                           && row.value(QStringLiteral("prompt")).toString()
                                  == QStringLiteral("weekly standup"),
                       "cron/prompt 全文原样回交（截断是视图层职责）");
    TestHarness::check(row.value(QStringLiteral("recurring")).toBool()
                           && !row.value(QStringLiteral("durable")).toBool()
                           && !row.value(QStringLiteral("pendingDelivery")).toBool()
                           && row.value(QStringLiteral("lastFired")).toString().isEmpty(),
                       "三个布尔旗与 lastFired 齐形（未触发=空串）");

    const QString cancel = mgr.cancelJob(out.id);
    TestHarness::check(cancel == QStringLiteral("Cancelled %1").arg(out.id)
                           && mgr.ledgerSnapshot().isEmpty(),
                       "取消后逐字回执且快照即刻干净（观测面与台账同源）");
    TestHarness::check(mgr.cancelJob(QStringLiteral("cron_deadbeef"))
                           == QStringLiteral("Job cron_deadbeef not found"),
                       "取消不存在任务逐字串");
}

} // namespace

int tst_cronmanager()
{
    const int before = TestHarness::failCount();
    testValidateCron();      // 5
    testFieldMatches();      // 5
    testCronMatches();       // 7
    testLedgerSnapshot();    // 8
    return TestHarness::failCount() - before;
}
