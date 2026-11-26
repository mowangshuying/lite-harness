// tst_cronmanager.cpp —— s12 定时任务引擎判据单测（s17 P1 观测面补强批次）
// 被测：src/CronSchedulerManager —— validateCron 7 字段报错族 / cronFieldMatches 逗号列表不命中
//       回归（647b22e）/ cronMatches 秒·年 AND + Vixie 双限定 OR / pollDueJobs 逐秒补匹配
//       / ledgerSnapshot 台账快照（P1 观测面）
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

// validateCron：合法形回空串；七类报错逐字族（"<字段名>: <错误>"，7 字段 second…year）
void testValidateCron()
{
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 */5 9-17 * * 1-5 *")).isEmpty(),
                       "复合合法式回空串");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 9 * *"))
                           == QStringLiteral("Expected 7 fields, got 4"),
                       "段数不足逐字串（7 段硬切，无 5 段兼容通道）");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("60 * * * * * *"))
                           == QStringLiteral("second: Value 60 is outside [0-59]"),
                       "秒位越界报 second 前缀 + 域区间");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 60 * * * * *"))
                           == QStringLiteral("minute: Value 60 is outside [0-59]"),
                       "分钟位越界仍报 minute");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * * 5-1 * * *"))
                           == QStringLiteral("day-of-month: Range start is greater than end: 5-1"),
                       "区间倒挂报错");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * * * * 7 *"))
                           == QStringLiteral("day-of-week: Value 7 is outside [0-6]"),
                       "day-of-week 上界 6（0=周日沿用）");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * * * * * 1969"))
                           == QStringLiteral("year: Value 1969 is outside [1970-2099]"),
                       "year 下界 1970");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("* * * * * * 2100"))
                           == QStringLiteral("year: Value 2100 is outside [1970-2099]"),
                       "year 上界 2099");
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

// cronMatches：双 *（日/周）恒真通道 与 Vixie 双限定 OR 语义（日与周都限定时任一命中即触发）；
// 秒/年位参与 AND 通道，5 段式一律不匹配（7 段硬切）
void testCronMatches()
{
    const QDateTime thuNoon(QDate(2026, 1, 1), QTime(12, 0)); // 2026-01-01 = 周四（Qt dow=4，cron 4）
    TestHarness::check(QDate(2026, 1, 1).dayOfWeek() == 4,
                       "夹具日期自证：2026-01-01 是周四（本套件其余断言依赖此事实）");
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 * 4 *"), thuNoon),
                       "双限定且双命中 → 真");
    // 同表达式对 2026-01-08（周四、非 1 号）：日不中但周中 → Vixie OR 仍触发
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 * 4 *"),
                                                         QDateTime(QDate(2026, 1, 8), QTime(12, 0))),
                       "双限定单侧命中 → 真（Vixie OR 语义）");
    // 2026-01-02 周五：日、周皆不中 → 假
    TestHarness::check(!CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 * 4 *"),
                                                          QDateTime(QDate(2026, 1, 2), QTime(12, 0))),
                       "双限定全不中 → 假");
    // 单限定（只有日）：只看限定侧
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 13 * * *"),
                                                         QDateTime(QDate(2026, 8, 13), QTime(0, 0))),
                       "日限定 + 周 * → 只看日");
    TestHarness::check(!CronSchedulerManager::cronMatches(QStringLiteral("0 */15 * * * * *"),
                                                          QDateTime(QDate(2026, 8, 13), QTime(7, 14))),
                       "分钟不中 → 假（秒/分/时/月/年 AND 通道）");
    // 7 字段新增：秒位参与匹配（同一分钟不同秒可区分）
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("30 * * * * * *"),
                                                         QDateTime(QDate(2026, 8, 13), QTime(7, 14, 30)))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("30 * * * * * *"),
                                                                 QDateTime(QDate(2026, 8, 13), QTime(7, 14, 29))),
                       "秒位命中与不命中（秒 30 vs 29）");
    // 年位参与匹配：同月日、不同年
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 1 * 2026"),
                                                         QDateTime(QDate(2026, 1, 1), QTime(12, 0)))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 1 * 2026"),
                                                                 QDateTime(QDate(2027, 1, 1), QTime(12, 0))),
                       "年位命中与不命中（2026 vs 2027）");
    // 旧 5 段式不再被接受：段数不符 → 假
    TestHarness::check(!CronSchedulerManager::cronMatches(QStringLiteral("0 12 1 * 4"), thuNoon),
                       "5 段式不再匹配（无兼容通道）");
}

// pollDueJobs 逐秒补匹配：1s tick 漂移跨过整秒时，秒位任务不能漏触发（7 字段新增能力）
void testPollCatchUp()
{
    CronSchedulerManager mgr([] { return QStringLiteral("unused"); });
    CronSchedulerManager::CronJob out;
    const QString err = mgr.scheduleJob(QStringLiteral("2 * * * * * *"), QStringLiteral("at :02"),
                                        true, false, &out);
    TestHarness::check(err.isEmpty() && !mgr.hasQueue(), "秒位表达式登记成功且未到期不入队");

    const QDateTime t00(QDate(2026, 1, 1), QTime(12, 0, 0));
    mgr.pollDueJobs(t00); // 秒 0：不命中
    TestHarness::check(!mgr.hasQueue(), "秒位未到不入队");

    // 模拟 tick 漂移：下一次轮询直接落到 :03，秒 2 被跨过 → 逐秒补匹配须仍触发
    mgr.pollDueJobs(QDateTime(QDate(2026, 1, 1), QTime(12, 0, 3)));
    const QList<CronSchedulerManager::CronJob> fired = mgr.consumeQueue();
    TestHarness::check(fired.size() == 1 && fired.at(0).lastFired.endsWith(QLatin1String(":02")),
                       "跨过整秒仍补匹配入队（marker 落在 :02）");

    // 未 ack 前 pendingDelivery 挡住后续秒，不重复入队
    mgr.pollDueJobs(QDateTime(QDate(2026, 1, 1), QTime(12, 0, 10)));
    TestHarness::check(!mgr.hasQueue(), "在途未 ack 时不重复入队");

    mgr.acknowledgeCronJobs(fired);
    mgr.pollDueJobs(QDateTime(QDate(2026, 1, 1), QTime(12, 1, 2)));
    TestHarness::check(mgr.hasQueue() && mgr.consumeQueue().size() == 1,
                       "ack 后下一分钟同秒位再次触发");
}

// 亚秒余数节拍漏秒回归钉：宿主 QTimer 1s 节拍带毫秒余数，且事件循环追平后相邻两拍
// 墙钟差可略小于 1s。现状 from = m_lastPoll.addSecs(1) 把上一拍的 .500 余数一路带进
// 补匹配窗口，窗口末端永远够不到当前拍的 :00 整秒 → 秒位限定表达式被永久跳过。
void testPollSubSecondDrift()
{
    CronSchedulerManager mgr([] { return QStringLiteral("unused"); });
    CronSchedulerManager::CronJob out;
    TestHarness::check(mgr.scheduleJob(QStringLiteral("0 * * * * * *"),
                                       QStringLiteral("every minute at :00"),
                                       true, false, &out).isEmpty(),
                       "每分 0 秒表达式登记成功");

    const QDate d(2026, 1, 1);
    mgr.pollDueJobs(QDateTime(d, QTime(12, 0, 0, 500))); // 首拍 from=moment，命中 12:00:00
    TestHarness::check(mgr.hasQueue(), "首拍命中 :00 入队");
    mgr.acknowledgeCronJobs(mgr.consumeQueue());

    // 每拍毫秒余数递减（节拍略快于 1s）：真实 QTimer 追平即此形态
    mgr.pollDueJobs(QDateTime(d, QTime(12, 1, 0, 400)));
    mgr.pollDueJobs(QDateTime(d, QTime(12, 2, 0, 300)));
    mgr.pollDueJobs(QDateTime(d, QTime(12, 3, 0, 200)));
    TestHarness::check(mgr.hasQueue(),
                       "亚秒余数节拍不得漏掉 :00 整秒（三拍内至少触发一次）");
}

// 缺陷 2（*/N 锚定域下界）+ 缺陷 3 无歧义部分（Quartz 惯例 `?` 与周/月英文名）。
// 两者都必须走 cronMatches/cronFieldMatches 出口，否则「校验放行、匹配不中」的静默失效
// 测不出来——那比直接报错更坏。
void testStepAnchorAndQuartzAliases()
{
    // 日位 */2：Vixie 语义 = 1,3,5…31（从域下界 1 步进）。旧 value%N 得 2,4,6…30，
    // 「每两天」整体偏一天——用户设的 1 号永远不触发。
    const QTime nine(9, 0, 0);
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 */2 * * *"),
                                                         QDateTime(QDate(2026, 1, 1), nine))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 */2 * * *"),
                                                                 QDateTime(QDate(2026, 1, 2), nine))
                           && CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 */2 * * *"),
                                                                QDateTime(QDate(2026, 1, 3), nine)),
                       "日位 */2 从下界 1 步进（1/3 命中、2 不命中）");
    // 月位 */3：1,4,7,10（旧实现给 3,6,9,12）
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 1 */3 * *"),
                                                         QDateTime(QDate(2026, 1, 1), QTime(0, 0)))
                           && CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 1 */3 * *"),
                                                                QDateTime(QDate(2026, 4, 1), QTime(0, 0)))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 1 */3 * *"),
                                                                 QDateTime(QDate(2026, 3, 1), QTime(0, 0))),
                       "月位 */3 从下界 1 步进（1/4 命中、3 不命中）");
    // 年位 */10：1970,1980…2020,2030（旧实现给 2000,2010,2020——下界 1970 从未被锚过）
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 1 1 * */10"),
                                                         QDateTime(QDate(2020, 1, 1), QTime(0, 0)))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 0 1 1 * */10"),
                                                                 QDateTime(QDate(2026, 1, 1), QTime(0, 0))),
                       "年位 */10 以 1970 为锚（2020 命中、2026 不命中）");
    // 逗号项里的步长同样继承锚（fieldMin 必须透传，否则 `1,*/10` 的步长项又退回 0 起算）
    TestHarness::check(CronSchedulerManager::cronFieldMatches(QStringLiteral("2,*/4"), 2, 1)
                           && CronSchedulerManager::cronFieldMatches(QStringLiteral("2,*/4"), 5, 1)
                           && !CronSchedulerManager::cronFieldMatches(QStringLiteral("2,*/4"), 4, 1),
                       "逗号项内 */4 仍按域下界 1 锚定（2/5 命中、4 不命中）");
    // 下界 0 的字段两种锚法逐字等价——旧判据不得因此翻红
    TestHarness::check(CronSchedulerManager::cronFieldMatches(QStringLiteral("*/15"), 30, 0)
                           && !CronSchedulerManager::cronFieldMatches(QStringLiteral("*/15"), 7, 0),
                       "下界 0 时 */N 行为与旧实现等价（回归护栏）");

    // `?`（Quartz「不指定」）：日/周位等价 `*`，且校验与匹配两侧同时成立
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 0 9 ? * MON *")).isEmpty(),
                       "`?` 占位在日位通过校验（Quartz 惯例）");
    const QDateTime mon9(QDate(2026, 1, 5), nine); // 2026-01-05 = 周一
    TestHarness::check(QDate(2026, 1, 5).dayOfWeek() == 1
                           && CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 ? * MON *"), mon9)
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 ? * MON *"),
                                                                 QDateTime(QDate(2026, 1, 6), nine)),
                       "`? * MON` 匹配周一不匹配周二（校验/匹配同源）");
    // `?` 只日/周位合法，挪到秒位仍须报错（勿全局吞 `?`）
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("? 0 9 * * * *"))
                           == QStringLiteral("second: Invalid field: ?"),
                       "`?` 出现在秒位仍逐字拒绝");

    // 周/月英文名：大小写不敏感，且与数字写法同形命中
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 * * sun *"),
                                                         QDateTime(QDate(2026, 1, 4), nine))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 * * SUN *"),
                                                                 QDateTime(QDate(2026, 1, 5), nine)),
                       "周名 SUN 命中周日（2026-01-04）、小写同效，且不命中周一");
    TestHarness::check(CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 * * MON-FRI *"),
                                                         QDateTime(QDate(2026, 1, 7), nine))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 9 * * MON-FRI *"),
                                                                 QDateTime(QDate(2026, 1, 4), nine)),
                       "周名区间 MON-FRI 在区间内展开（周三命中、周日不命中）");
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 0 12 1 DEC * *")).isEmpty()
                           && CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 DEC * *"),
                                                                QDateTime(QDate(2026, 12, 1), QTime(12, 0)))
                           && !CronSchedulerManager::cronMatches(QStringLiteral("0 0 12 1 DEC * *"),
                                                                 QDateTime(QDate(2026, 11, 1), QTime(12, 0))),
                       "月名 DEC 校验通过且只命中 12 月");
    // 名字表外的字母仍须拒绝（勿把「未知字母」折叠成恒真）
    TestHarness::check(CronSchedulerManager::validateCron(QStringLiteral("0 0 9 * * XYZ *"))
                           == QStringLiteral("day-of-week: Invalid field: XYZ"),
                       "未知周名逐字拒绝（不静默放行）");

    // 端到端收口：模型按 Quartz 写法登记的任务**必须真能触发**。validateCron 与 cronMatches
    // 各调一次 preprocessCronField，任一侧漏掉都会变成「登记成功、永不触发」——纯静态判据
    // 两侧各自都成立，测不出来；只有走 scheduleJob + pollDueJobs 全链路才钉得住同源契约。
    {
        CronSchedulerManager mgr([] { return QStringLiteral("unused"); });
        CronSchedulerManager::CronJob out;
        const QString err = mgr.scheduleJob(QStringLiteral("0 30 9 ? * MON *"),
                                            QStringLiteral("weekly sync"), true, false, &out);
        TestHarness::check(err.isEmpty(), "Quartz 写法（`?` + 周名）登记成功");

        // 2026-01-05 = 周一；节拍带毫秒余数，同一拍顺带覆盖漏秒修复
        mgr.pollDueJobs(QDateTime(QDate(2026, 1, 5), QTime(9, 30, 0, 400)));
        TestHarness::check(mgr.hasQueue(), "Quartz 写法在整秒轴轮询下真触发（同源契约）");
        const QList<CronSchedulerManager::CronJob> fired = mgr.consumeQueue();
        TestHarness::check(fired.size() == 1
                               && fired.at(0).lastFired.endsWith(QStringLiteral("09:30:00")),
                           "触发 marker 落在整秒 09:30:00，非亚秒余数时刻");

        mgr.acknowledgeCronJobs(fired);
        mgr.pollDueJobs(QDateTime(QDate(2026, 1, 6), QTime(9, 30, 0, 400))); // 周二
        TestHarness::check(!mgr.hasQueue(), "周二同时分不触发（周名限定确实生效，非恒真）");
    }
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
    const QString noPrompt = mgr.scheduleJob(QStringLiteral("* * * * * * *"), QStringLiteral("  "),
                                             true, false, &out);
    TestHarness::check(noPrompt == QStringLiteral("prompt cannot be empty"),
                       "空 prompt 逐字拒绝串");

    const QString err = mgr.scheduleJob(QStringLiteral("0 0 9 * * 1 *"),
                                        QStringLiteral("weekly standup"),
                                        true, false, &out);
    TestHarness::check(err.isEmpty() && !out.id.isEmpty(), "登记成功回空串并经 out 回填副本");

    const QJsonArray snap = mgr.ledgerSnapshot();
    TestHarness::check(snap.size() == 1, "一条任务快照一行");
    const QJsonObject row = snap.at(0).toObject();
    TestHarness::check(row.value(QStringLiteral("id")).toString().startsWith(QLatin1String("cron_")),
                       "id 形如 cron_<hex8>");
    TestHarness::check(row.value(QStringLiteral("cron")).toString() == QStringLiteral("0 0 9 * * 1 *")
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
    testValidateCron();      // 8
    testFieldMatches();      // 5
    testCronMatches();       // 9
    testPollCatchUp();       // 5
    testPollSubSecondDrift();// 3
    testStepAnchorAndQuartzAliases(); // 12
    testLedgerSnapshot();    // 10
    return TestHarness::failCount() - before;
}
