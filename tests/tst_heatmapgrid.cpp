// tst_heatmapgrid —— 新会话页热力图网格算术（HeatmapGrid.h）的直测。
//
// 为什么值得单独一套：这套算术出过两次用户可见故障——① 旧口径按周日对齐日历周 + 绘制时
// 跳过未来日期 → 非周六时尾列留空、矩形右下有破口；② 月份标签不去重 → 371 天窗口首尾同月号
// 跨年，底部出现两个相同月份；且首列补标与相邻月标签挤压（s18 两阶段修：月号去重 +
// 补标让位给同号 1 号锚点，判据抽成 monthLabels）。它错起来是「画得出但格子数/
// 日期/标签不对」，肉眼只能看出「好像少了几个」，看不出「第 N 格显示的是第 N±1 天」。
// 全部断言显式传入 today，不读墙钟（口径同 tst_cronmanager），故任何日期/时区下结果稳定。

#include "HeatmapGrid.h"

#include "TestHarness.h"

#include <QDate>

namespace {

QDate fixedToday()
{
    // 刻意取一个非周六的日子：旧口径正是这类日子会在尾列留空。
    return QDate(2026, 11, 9); // 星期一
}

// ---- ① 窗口长度与首末格 ----
void testWindowShape()
{
    const QDate today = fixedToday();
    TestHarness::check(HeatmapGrid::kCellCount == 371,
                       "网格恒 53×7 = 371 格（满矩形的前提）");
    TestHarness::check(HeatmapGrid::kWeeks == 53 && HeatmapGrid::kDaysPerWeek == 7,
                       "列数 53、行数 7");
    TestHarness::check(HeatmapGrid::windowStart(today).daysTo(today) == 370,
                       "首日距 today 恰 370 天（= kCellCount - 1）");
    // 末格 (52,6) 必须恒为 today —— 这是「最后一个日期是当前日期」的用户可见契约
    TestHarness::check(HeatmapGrid::dateOfCell(HeatmapGrid::kWeeks - 1,
                                               HeatmapGrid::kDaysPerWeek - 1, today) == today,
                       "末格 (52,6) 恒为 today");
    TestHarness::check(HeatmapGrid::cellOf(today, today)
                           == qMakePair(HeatmapGrid::kWeeks - 1, HeatmapGrid::kDaysPerWeek - 1),
                       "today 落在末格 (52,6)");
}

// ---- ② 每一列都是满列 ----
void testEveryColumnIsFull()
{
    const QDate today = fixedToday();
    bool allValid = true;
    for (int col = 0; col < HeatmapGrid::kWeeks; ++col) {
        for (int row = 0; row < HeatmapGrid::kDaysPerWeek; ++row) {
            const QDate d = HeatmapGrid::dateOfCell(col, row, today);
            if (!d.isValid() || d > today) {
                allValid = false;
            }
        }
    }
    TestHarness::check(allValid, "371 格全部有效且无未来日期（旧口径此处会缺格）");

    // 末列的 7 天必须是 today 往前数的连续 7 天
    const int lastCol = HeatmapGrid::kWeeks - 1;
    for (int row = 0; row < HeatmapGrid::kDaysPerWeek; ++row) {
        const QDate d = HeatmapGrid::dateOfCell(lastCol, row, today);
        TestHarness::check(d == today.addDays(row - (HeatmapGrid::kDaysPerWeek - 1)),
                           "末列逐格连续且以 today 收尾");
    }
}

// ---- ③ cellOf 与 dateOfCell 互逆 ----
void testRoundTrip()
{
    const QDate today = fixedToday();
    const QDate start = HeatmapGrid::windowStart(today);
    bool ok = true;
    for (int i = 0; i < HeatmapGrid::kCellCount; ++i) {
        const QDate d = start.addDays(i);
        const QPair<int, int> cell = HeatmapGrid::cellOf(d, today);
        if (HeatmapGrid::dateOfCell(cell.first, cell.second, today) != d) {
            ok = false;
        }
    }
    TestHarness::check(ok, "窗口内 371 天逐格 dateOfCell(cellOf(d)) == d");

    // 列号必须严格递增、行号在列内递增（绘制顺序契约：paintEvent 按 col/row 双层循环铺格）
    TestHarness::check(HeatmapGrid::cellOf(start, today) == qMakePair(0, 0),
                       "首日 = 左上角 (0,0)");
    TestHarness::check(HeatmapGrid::cellOf(start.addDays(1), today) == qMakePair(0, 1),
                       "第二日 = (0,1)，行内递增");
    TestHarness::check(HeatmapGrid::cellOf(start.addDays(HeatmapGrid::kDaysPerWeek), today)
                           == qMakePair(1, 0),
                       "第 8 日换列 = (1,0)");
}

// ---- ④ 窗口外判定：不钳位、不回绕 ----
void testOutOfRange()
{
    const QDate today = fixedToday();
    const QDate start = HeatmapGrid::windowStart(today);

    TestHarness::check(HeatmapGrid::cellOf(start.addDays(-1), today) == qMakePair(-1, -1),
                       "早于首日 → {-1,-1}（负 offset 不得被整除截断成第 0 列）");
    TestHarness::check(HeatmapGrid::cellOf(start.addDays(-400), today) == qMakePair(-1, -1),
                       "远早于窗口同样返回 {-1,-1}");
    TestHarness::check(HeatmapGrid::cellOf(today.addDays(1), today) == qMakePair(-1, -1),
                       "晚于 today → {-1,-1}（未来日期无坐标，但绘制仍铺满格）");
    TestHarness::check(!HeatmapGrid::dateOfCell(-1, 0, today).isValid(),
                       "col 越负 → 无效日期");
    TestHarness::check(!HeatmapGrid::dateOfCell(HeatmapGrid::kWeeks, 0, today).isValid(),
                       "col 越上界 → 无效日期");
    TestHarness::check(!HeatmapGrid::dateOfCell(0, HeatmapGrid::kDaysPerWeek, today).isValid(),
                       "row 越界 → 无效日期");
}

// ---- ⑤ 跨年/跨月边界：addDays 而非「同月日序」的隐含假设 ----
void testCrossBoundary()
{
    // 窗口横跨元旦：旧口径若按「月内日」算格序会在此断裂
    const QDate newYearEve(2026, 12, 31);
    TestHarness::check(HeatmapGrid::dateOfCell(HeatmapGrid::kWeeks - 1,
                                               HeatmapGrid::kDaysPerWeek - 1, newYearEve)
                           == newYearEve,
                       "跨年 today 末格仍为 today");
    TestHarness::check(HeatmapGrid::windowStart(newYearEve).year() == 2025,
                       "跨年窗口首日回落到上一年");

    // 闰日：2028-02-29 存在，窗口长度仍须 371
    const QDate leap(2028, 2, 29);
    TestHarness::check(leap.isValid()
                           && HeatmapGrid::windowStart(leap).daysTo(leap) == 370,
                       "闰日窗口跨度仍恒 370 天");

    // today 为 1 月 1 日：窗口长度 370 天 > 一个回归年，故首日会**跨过两个年份边界**
    // （2026-01-01 − 370 = 2024-12-27，不是 2025）。这里钉死具体日期而非只判「年份更小」，
    // 因为 off-by-one 恰好会落在同一年内、只判大小抓不到。
    const QDate jan1(2026, 1, 1);
    TestHarness::check(HeatmapGrid::windowStart(jan1) == QDate(2024, 12, 27),
                       "年初 today 的窗口首日跨两个年份边界（2026-01-01 → 2024-12-27）");
}

// ---- ⑥ 月份标签：按月号去重、首列补标让位给同号 1 号锚点（monthLabels） ----
// 371 天窗口恒覆盖 ≥13 个月份实例（12 个连续月份最多 366 天），月号只有 12 个，
// 故重复**必然**存在：起点月与终点月同号跨年（today=11-18 → 窗口从去年 11-13 起，
// 首列补标与末段 1 号列都是「11月」）。这是用户可见故障（底部两个相同月份），
// 去重键必须是月号而非年+月——后者对跨年同号重复恒不命中。第二层故障：保留首列
// 补标会与下一个 1 号标签挤压（月界距首日最近仅 1 列），故补标让位给同号 1 号锚点，
// 让位后所有标签都是 1 号锚点、相邻 ≥4 列（月长 ≥28 天），挤压与重叠一并消失。
void testMonthLabels()
{
    // 用户报障形态的具体日期
    const QDate complaint(2026, 11, 18);
    TestHarness::check(HeatmapGrid::windowStart(complaint) == QDate(2025, 11, 13),
                       "报障形态前提：窗口首日 2025-11-13（与终点同月号、跨年）");
    const QList<HeatmapGrid::MonthLabel> labels = HeatmapGrid::monthLabels(complaint);
    int novCount = 0;
    QDate novAnchor;
    int novCol = -1;
    for (const HeatmapGrid::MonthLabel &l : labels) {
        if (l.anchor.month() == 11) {
            ++novCount;
            novAnchor = l.anchor;
            novCol = l.col;
        }
    }
    TestHarness::check(novCount == 1, "「11月」只出现一次（首列补标与 1 号列标签去重）");
    TestHarness::check(novAnchor == QDate(2026, 11, 1) && novCol > 0,
                       "首列补标让位：保留的是末段 2026-11-01 锚点而非 col 0 补标（否则与 12月标签挤压）");

    // 极端形态：4-30 起窗同时撞两个月号（4 月与 5 月各出现两次）
    const QDate doubleDup(2026, 5, 5);
    TestHarness::check(HeatmapGrid::windowStart(doubleDup) == QDate(2025, 4, 30),
                       "极端形态前提：窗口首日 2025-04-30");
    TestHarness::check(HeatmapGrid::monthLabels(doubleDup).size() == 12,
                       "两个月号同时撞车，去重后仍恰 12 个标签");

    // 扫掠：月首 / 月初 ≤5 号（首日落上月）/ 跨年 / 闰日，逐 today 验不变量
    const QList<QDate> todays = {
        QDate(2026, 1, 1),  QDate(2026, 1, 5),  QDate(2026, 2, 28),
        QDate(2026, 3, 1),  QDate(2026, 6, 15), QDate(2026, 11, 9),
        QDate(2026, 12, 31), QDate(2027, 1, 1), QDate(2028, 2, 29),
        QDate(2028, 3, 1),  QDate(2026, 5, 5),  QDate(2026, 11, 18),
    };
    bool sizeOk = true;
    bool uniqOk = true;
    bool orderOk = true;
    bool gapOk = true;
    bool anchorOk = true;
    bool coverageOk = true;
    for (const QDate &today : todays) {
        const QList<HeatmapGrid::MonthLabel> ls = HeatmapGrid::monthLabels(today);
        if (ls.size() != 12)
            sizeOk = false;
        QList<int> months;
        int prevCol = -1;
        for (int i = 0; i < ls.size(); ++i) {
            const HeatmapGrid::MonthLabel &l = ls[i];
            if (months.contains(l.anchor.month()))
                uniqOk = false;
            months.append(l.anchor.month());
            if (l.col <= prevCol)
                orderOk = false;
            // 相邻标签 ≥4 列（月长 ≥28 天）：首列补标若不让位，此处会以 1~2 列间距转红
            if (prevCol >= 0 && l.col - prevCol < 4)
                gapOk = false;
            prevCol = l.col;
            // 锚点必须落在自己那一列：col>0 必为该月 1 号；首列要么 1 号要么窗口首日
            if (HeatmapGrid::cellOf(l.anchor, today).first != l.col)
                anchorOk = false;
            if (l.col > 0 && l.anchor.day() != 1)
                anchorOk = false;
            if (l.col == 0 && l.anchor.day() != 1
                && l.anchor != HeatmapGrid::windowStart(today))
                anchorOk = false;
        }
        for (int m = 1; m <= 12; ++m) {
            if (!months.contains(m))
                coverageOk = false;
        }
    }
    TestHarness::check(sizeOk, "任意 today 恰 12 个标签（371 天恒覆盖 ≥13 个月份实例）");
    TestHarness::check(uniqOk, "12 个标签月号互异（底部不再出现相同月份）");
    TestHarness::check(orderOk, "标签列号严格递增（绘制从左到右）");
    TestHarness::check(gapOk, "相邻标签间距 ≥4 列（月长 ≥28 天；补标不让位即 1~2 列挤压）");
    TestHarness::check(anchorOk, "锚点日期落在标签列内：col>0 必为 1 号，首列补标为窗口首日");
    TestHarness::check(coverageOk, "12 个标签恰覆盖月号 1..12（去重不丢月份）");

    // 去重只丢重复、不丢首次：每个含 1 号的列要么有标签，要么其月号已在更左出现过
    bool noLost = true;
    for (const QDate &today : todays) {
        const QList<HeatmapGrid::MonthLabel> ls = HeatmapGrid::monthLabels(today);
        for (int col = 0; col < HeatmapGrid::kWeeks; ++col) {
            for (int row = 0; row < HeatmapGrid::kDaysPerWeek; ++row) {
                const QDate d = HeatmapGrid::dateOfCell(col, row, today);
                if (d.day() != 1)
                    continue;
                bool labeled = false;
                bool seenLeft = false;
                for (const HeatmapGrid::MonthLabel &l : ls) {
                    if (l.col == col)
                        labeled = true;
                    if (l.col < col && l.anchor.month() == d.month())
                        seenLeft = true;
                }
                if (!labeled && !seenLeft)
                    noLost = false;
            }
        }
    }
    TestHarness::check(noLost, "含 1 号的列未被静默跳过（无标签即其月号已在更左出现）");
}

} // namespace

int tst_heatmapgrid()
{
    const int before = TestHarness::failCount();
    // 括号内为**实跑条数**（不是静态点数）：testEveryColumnIsFull 的循环含 7 次判定、
    // 只占 1 个静态点，两者本套件分别是 39 与 33，别混用。
    testWindowShape();        // 5
    testEveryColumnIsFull();  // 8（1 静态点 + 循环 7 次）
    testRoundTrip();          // 4
    testOutOfRange();         // 6
    testCrossBoundary();      // 4
    testMonthLabels();        // 12（循环折进布尔旗，静态点=实跑数）
    return TestHarness::failCount() - before;
}
