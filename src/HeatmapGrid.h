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
//   ④ 窗口外（早于首日 / 晚于 today）返回 {-1,-1}，不钳位、不回绕。
// 列 = 连续 7 天块（序号 0..52），行 = 块内日序（0..6）；每行仍是固定的星期几（随 today 滚动），
// 只是列首不再固定为周日——这是「满矩形 + 末格即今天」换来的取舍，勿改回日历周对齐。

#include <QDate>
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

} // namespace HeatmapGrid
