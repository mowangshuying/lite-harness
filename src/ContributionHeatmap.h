#pragma once

#include <QDate>
#include <QHash>
#include <QPair>
#include <QRect>
#include <QWidget>

#include "ActivityLedger.h"

class QMouseEvent;
class QPaintEvent;

// 新会话页每日 token 消耗热力图（53 列 × 7 行 = 371 天滚动窗口，仿 GitHub contribution graph）。
// 网格按「列 = 连续 7 天块」顺序铺满：gridStart = today - 370，末列末格恒为今天——
// 因此网格永远是完整矩形、最后一列永远是满列（旧版按周日对齐，尾列在非周六时留空，破矩形）。
// 每行仍是固定的星期几（gridStart 的星期 + 行号），只是列首不再固定为周日。
// 数据侧见 ActivityLedger.h；本类只做渲染与命中检测。
//
// 覆盖边界（承 UsageTokens.h / ActivityLedger.h 同一条纪律）：账本只统计主循环与队友
// 回合两条 ChatStream 路径的 usage 回读，压缩/记忆/workflow 走 AsyncRequest 无 usage 通道。
// tooltip 必须如实标注这条边界，否则数字会误导成本判断。
class ContributionHeatmap : public QWidget
{
    Q_OBJECT
public:
    explicit ContributionHeatmap(QWidget *parent = nullptr);

    void setData(const QHash<QDate, ActivityLedger::DayTotal> &dayMap);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    // 网格几何（不含月份标签带与卡片 padding）
    QRect gridRect() const;
    // 命中检测：point 落在哪一格；不在网格内或落在间隙返回无效 QDate
    QDate cellDateAt(const QPoint &point) const;
    // 日期 → 网格坐标（列 = 7 天块序号 0..52，行 = 块内日序 0..6）；窗口外返回 {-1, -1}
    QPair<int, int> cellPosition(const QDate &date, const QDate &today) const;
    // 滚动窗口首日：末格（52,6）恒为 today，故窗口首日 = today - (53*7 - 1)
    static QDate gridStart(const QDate &today);

    QHash<QDate, ActivityLedger::DayTotal> m_data;

    static constexpr int kWeeks = 53;
    static constexpr int kDaysPerWeek = 7;
    static constexpr int kCellSize = 12;
    static constexpr int kCellGap = 3;
    static constexpr int kMonthLabelHeight = 16;
    // 卡片页眉带高度：网格上方留一行标题「会话活动」（对齐参考图卡片顶部的页签行；
    // 本仓只有 token 消耗一个口径，故只画单个标题，不造假的第二页签）
    static constexpr int kHeaderHeight = 30;
    static constexpr int kPadding = 10;
};
