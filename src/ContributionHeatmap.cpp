#include "ContributionHeatmap.h"

#include "ThemeAware.h"

#include <FluThemeUtils.h>

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace {

// 三主题色板（档 0 空色 + 档 1-5 强调色 alpha 阶梯 + 卡片背景/边框）。
// 与 LiteHarness.cpp:590 同法用 isLightTheme/isDarkTheme/isAtomOneDarkTheme 分支取色——
// 本仓 FluentUI fork 无 getFluentColor/FluThemeColorType API（全库 grep 零命中），
// 色阶只能在此硬编码；主题切换由 ThemeAware::bind 的 extraRefresh 触发 update() 重绘。
struct Palette {
    QColor empty;
    QColor accent;
    QColor monthText;
    QColor cardBg;
    QColor cardBorder;
};

Palette currentPalette()
{
    // 三主题色板一律跟齐本仓「主题/取色」规范（AGENTS.md「主题 / QSS」节 · docs/doc.md P1 主题节）：
    //  - light 内容页 249 / 导航带 243 / 描边 229
    //  - dark  内容页 40,40,40 / 导航带 32,32,32 / 描边 69,69,69（刻意不取 FluentUI 示例页 55,55,55——
    //          它会和 #jumpBottomBtn:hover 合成实色撞值，本卡片无 hover 语义但仍按同一档保四层次）
    //  - atomOneDark 内容页 40,44,52 / 导航带 33,37,43 / 描边 71,74,79
    // 卡片底取导航带色（恒比内容页暗一档，与「导航带分层」的既有决策同源）；
    // 空格略高于卡片、低于档 1；描边取各主题原生控件描边色。
    // 强调色保留 GitHub 惯例绿（light 40,167,69 / dark 52,216,113 / aod 152,195,121），
    // 这是本视图唯一的彩色，不参与上述分层归一。
    if (FluThemeUtils::isLightTheme())
        return {QColor(235, 237, 240), QColor(40, 167, 69), QColor(90, 90, 90),
                QColor(255, 255, 255), QColor(229, 229, 229)};
    if (FluThemeUtils::isDarkTheme())
        return {QColor(50, 50, 50), QColor(52, 216, 113), QColor(170, 170, 170),
                QColor(32, 32, 32), QColor(69, 69, 69)};
    // atomOneDark
    return {QColor(55, 60, 72), QColor(152, 195, 121), QColor(171, 178, 191),
            QColor(33, 37, 43), QColor(71, 74, 79)};
}

// 强调色按档做 alpha 阶梯（1=30% / 2=55% / 3=75% / 4=90% / 5=100%）；空档用 empty 色。
QColor levelColor(const Palette &p, int level)
{
    if (level <= 0)
        return p.empty;
    static const int kAlpha[] = {0, 77, 140, 191, 230, 255};
    QColor c = p.accent;
    c.setAlpha(kAlpha[qBound(0, level, 5)]);
    return c;
}

// 周内日索引：Qt dayOfWeek 1=Mon..7=Sun → 0=Sun..6=Sat（GitHub 惯例，周日为列首）。
int dayOfWeekIndex(const QDate &d)
{
    return d.dayOfWeek() % 7;
}

QDate weekStart(const QDate &d)
{
    return d.addDays(-dayOfWeekIndex(d));
}

} // namespace

ContributionHeatmap::ContributionHeatmap(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    // QSS 只负责本体透明；卡片背景与色阶走 paintEvent 与主题分支同源。
    // extraRefresh 触发 update() 让主题切换即时重绘。
    ThemeAware::bind(QStringLiteral("ContributionHeatmap.qss"), this,
                     [this] { update(); });
}

void ContributionHeatmap::setData(const QHash<QDate, ActivityLedger::DayTotal> &dayMap)
{
    m_data = dayMap;
    update();
}

QSize ContributionHeatmap::sizeHint() const
{
    const int w = kPadding * 2 + kWeeks * (kCellSize + kCellGap) - kCellGap;
    const int h = kPadding * 2 + kDaysPerWeek * (kCellSize + kCellGap) - kCellGap
                  + kMonthLabelHeight;
    return QSize(w, h);
}

QSize ContributionHeatmap::minimumSizeHint() const
{
    return sizeHint();
}

QRect ContributionHeatmap::gridRect() const
{
    return QRect(kPadding, kPadding,
                 kWeeks * (kCellSize + kCellGap) - kCellGap,
                 kDaysPerWeek * (kCellSize + kCellGap) - kCellGap);
}

QPair<int, int> ContributionHeatmap::cellPosition(const QDate &date, const QDate &today) const
{
    const QDate todayWeek = weekStart(today);
    const QDate gridStart = todayWeek.addDays(-(kWeeks - 1) * 7);
    const QDate dateWeek = weekStart(date);
    const int col = gridStart.daysTo(dateWeek) / 7;
    const int row = dayOfWeekIndex(date);
    return {col, row};
}

QDate ContributionHeatmap::cellDateAt(const QPoint &point) const
{
    const QRect grid = gridRect();
    if (!grid.contains(point))
        return QDate();
    const int step = kCellSize + kCellGap;
    const int col = (point.x() - grid.left()) / step;
    const int row = (point.y() - grid.top()) / step;
    if (col < 0 || col >= kWeeks || row < 0 || row >= kDaysPerWeek)
        return QDate();
    // 落在格子间隙（gap 区）判未命中，避免相邻格边界抖动
    const int rowInCell = (point.y() - grid.top()) - row * step;
    const int colInCell = (point.x() - grid.left()) - col * step;
    if (rowInCell >= kCellSize || colInCell >= kCellSize)
        return QDate();
    const QDate today = QDate::currentDate();
    const QDate todayWeek = weekStart(today);
    const QDate gridStart = todayWeek.addDays(-(kWeeks - 1) * 7);
    return gridStart.addDays(col * 7 + row);
}

void ContributionHeatmap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const Palette palette = currentPalette();
    const QDate today = QDate::currentDate();
    const QRect grid = gridRect();
    const int step = kCellSize + kCellGap;

    // 卡片背景（QSS 只负责本体透明，卡片视觉走 paintEvent 与色阶同源）
    p.setPen(palette.cardBorder);
    p.setBrush(palette.cardBg);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);

    // 网格：列 = 周（0..52），行 = 周内日（0=Sun..6=Sat）
    p.setPen(Qt::NoPen);
    for (int col = 0; col < kWeeks; ++col) {
        const QDate colWeekStart = weekStart(today).addDays(-(kWeeks - 1 - col) * 7);
        for (int row = 0; row < kDaysPerWeek; ++row) {
            const QDate date = colWeekStart.addDays(row);
            if (date > today)
                continue; // 未来日期不画（GitHub 同法，本周未过部分留空）
            const int level = m_data.contains(date)
                                  ? ActivityLedger::levelForTokens(m_data[date].totalProcessed())
                                  : 0;
            p.setBrush(levelColor(palette, level));
            const QRect cell(grid.left() + col * step,
                             grid.top() + row * step,
                             kCellSize, kCellSize);
            p.drawRoundedRect(cell, 2, 2);
        }
    }

    // 月份标签（底部）：只在月份首列画，避免同月多列重复
    p.setPen(palette.monthText);
    QFont monthFont = p.font();
    monthFont.setPixelSize(11);
    p.setFont(monthFont);
    int lastMonth = -1;
    const int labelY = grid.bottom() + kMonthLabelHeight - 2;
    for (int col = 0; col < kWeeks; ++col) {
        const QDate colWeekStart = weekStart(today).addDays(-(kWeeks - 1 - col) * 7);
        const int m = colWeekStart.month();
        if (m != lastMonth) {
            const int x = grid.left() + col * step;
            p.drawText(x, labelY, tr("%1月").arg(m));
            lastMonth = m;
        }
    }
}

void ContributionHeatmap::mouseMoveEvent(QMouseEvent *event)
{
    const QDate date = cellDateAt(event->pos());
    if (!date.isValid() || date > QDate::currentDate()) {
        QToolTip::hideText();
        event->ignore();
        return;
    }
    // 承 UsageTokens.h / ActivityLedger.h 的覆盖面纪律：tooltip 必须写明这条边界，
    // 否则数字会误导成本判断（用户以为热力图反映全部消耗，实际不含压缩/记忆/workflow）。
    const QString boundary = tr("\n（口径：仅主循环与队友回合，不含压缩/记忆/workflow）");
    QString tip;
    if (m_data.contains(date)) {
        const ActivityLedger::DayTotal &day = m_data.value(date);
        tip = tr("%1\n输入 %2 · 输出 %3\n缓存命中 %4 · 思考 %5\n回读 %6 次")
                  .arg(date.toString(QStringLiteral("yyyy-MM-dd")))
                  .arg(day.prompt).arg(day.completion)
                  .arg(day.cached).arg(day.reasoning)
                  .arg(day.calls);
    } else {
        tip = tr("%1\n无消耗记录").arg(date.toString(QStringLiteral("yyyy-MM-dd")));
    }
    // Qt 6.9 弃用 globalPos()（返 QPoint），改 globalPosition()（返 QPointF）——QToolTip::showText
    // 两个重载都吃，取 QPointF 版本消除 C4996。event->pos() 保持本地坐标不变。
    QToolTip::showText(event->globalPosition().toPoint(), tip + boundary, this);
    QWidget::mouseMoveEvent(event);
}

void ContributionHeatmap::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}
