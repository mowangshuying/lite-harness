#pragma once

// 新会话页热力图的网格算术单源（header-only、纯 QDate、零 GUI 依赖，纪律同 CompactTokens.h /
// NavKeys.h）：窗口长度、日期↔格子坐标的互逆映射。
//
// 为什么单独成头（而非留在 ContributionHeatmap.cpp）：该 TU 是 QWidget（paintEvent/命中检测
// 都要 QPainter 与像素几何），按 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了
// ctest，于是这套算术此前**零直测**。而它恰恰是本仓出过一次用户可见故障的地方——旧口径按周日
// 对齐日历周、绘制时跳过 `date > today`，导致非周六时尾列留空、矩形右下破口（s17.9 修）。
// 那类 off-by-one 全靠肉眼看不出来，只能靠断言钉死，故把算术抽出来、像素映射留在类内。
//
// 不变量（tst_heatmapgrid 逐条钉）：
//   ① 网格恒为 kWeeks × kDaysPerWeek = 371 格的**满矩形**，绘制循环不得跳过任何格；
//   ② 末格 (52,6) 恒为 today，故末列恒满 7 格；
//   ③ cellOf 与 dateOfCell 互为逆运算（窗口内）；
//   ④ 窗口外（早于首日 / 晚于 today）返回 {-1,-1}，不钳位、不回绕；
//   ⑤ 月份标签按**月号**去重、首列补标让位给同号 1 号锚点（monthLabels）——窗口长 371 天
//      恒覆盖 ≥13 个月份实例而月号只有 12 个，重复**必然**存在（起点月与终点月同号跨年，
//      如 today=11-18 时窗口从去年 11-13 起，两端都是「11月」）；按年+月去重抓不到这种
//      重复，而保留首列补标又会与相邻月标签挤压（s18 两阶段修）。
// 列 = 连续 7 天块（序号 0..52），行 = 块内日序（0..6）；每行仍是固定的星期几（随 today 滚动），
// 只是列首不再固定为周日——这是「满矩形 + 末格即今天」换来的取舍，勿改回日历周对齐。

#include <QDate>
#include <QList>
#include <QPair>

namespace HeatmapGrid {

inline constexpr int kWeeks = 53;
inline constexpr int kDaysPerWeek = 7;
inline constexpr int kCellCount = kWeeks * kDaysPerWeek; // 371

// 滚动窗口首日：末格恒为 today，故首日 = today - (371 - 1)。
inline QDate windowStart(const QDate &today)
{
    return today.addDays(-(kCellCount - 1));
}

// 日期 → 格子坐标；窗口外返回 {-1, -1}。
// offset < 0 的负数整除在 C++ 里向零截断，但本函数先判负再除，故不会把「窗口外」误算成第 0 列。
inline QPair<int, int> cellOf(const QDate &date, const QDate &today)
{
    const int offset = windowStart(today).daysTo(date);
    if (offset < 0 || offset >= kCellCount)
        return {-1, -1};
    return {offset / kDaysPerWeek, offset % kDaysPerWeek};
}

// 格子坐标 → 日期（cellOf 的逆运算）；越界返回无效 QDate。
inline QDate dateOfCell(int col, int row, const QDate &today)
{
    if (col < 0 || col >= kWeeks || row < 0 || row >= kDaysPerWeek)
        return QDate();
    return windowStart(today).addDays(col * kDaysPerWeek + row);
}

// 底部月份标签的选列判据（视图只拿结果画像素，判据本身可 ctest 直测）：
//  - 标签贴在「含该月 1 号的那一列」——列首不对齐周日，月界常落在列中段，
//    只看列首月份会让标签晚 1-6 天；
//  - 按**月号**去重、同号保留最左的 1 号锚点：371 天恒覆盖 ≥13 个月份实例（12 个连续
//    月份最多 366 天 < 371），月号只有 12 个，重复必然存在——起点月与终点月同号但跨年
//    （today=11-18 → 窗口从去年 11-13 起，两端都是「11月」），极端形态（如 4-30 起窗）
//    同时撞两个月号。按年+月去重抓不到这种跨年同号重复，用户看到的正是月号重复；
//  - **首列补标让位**：首列不含 1 号时本欲补窗口首日所在月防左端空白，但补标贴 col 0
//    会与下一个 1 号标签挤压（月界距首日最近仅 7 天 = 1 列，字宽 25px > 列距 15px，
//    极端时两标签同列重叠）。而补标被使用 ⟺ 首日是月中残余几天 ⟺ 窗口必跨回同号月
//    （370 天后日序 ≤ 月末-1），该月必在末段以 1 号再现——故仅当该月**没有** 1 号标签
//    时才补标（防御分支，现行窗口长度下不可达）。让位后所有标签都是 1 号锚点，
//    月长 ≥28 天保证相邻标签 ≥4 列，挤压与重叠一并消失。
// 不变量：任意 today 恰返回 12 个标签、月号互异且覆盖 1..12、col 严格递增且相邻 ≥4 列。
struct MonthLabel {
    int col;
    QDate anchor; // 该月 1 号；首列补标时为窗口首日
};

inline QList<MonthLabel> monthLabels(const QDate &today)
{
    QList<MonthLabel> labels;
    QList<int> seenMonths;
    bool firstColHasDayOne = false;
    for (int col = 0; col < kWeeks; ++col) {
        QDate anchor;
        for (int row = 0; row < kDaysPerWeek; ++row) {
            const QDate d = dateOfCell(col, row, today);
            if (d.day() == 1)
                anchor = d;
        }
        if (!anchor.isValid())
            continue;
        if (col == 0)
            firstColHasDayOne = true;
        if (seenMonths.contains(anchor.month()))
            continue;
        seenMonths.append(anchor.month());
        labels.append({col, anchor});
    }
    if (!firstColHasDayOne) {
        const QDate first = windowStart(today);
        if (!seenMonths.contains(first.month())) {
            labels.prepend({0, first});
            seenMonths.append(first.month());
        }
    }
    return labels;
}

} // namespace HeatmapGrid
