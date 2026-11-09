#include "ContributionHeatmap.h"

#include "ThemeAware.h"
#include "UsageTokens.h" // 紧凑读数单源 formatCompact（热力图 tooltip 的四个 token 字段）

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

} // namespace

// 滚动窗口首日：网格恒为 kWeeks 列 × kDaysPerWeek 行 = 371 格，末格（52,6）恒为 today，
// 故首日 = today - 370。列 = 连续 7 天块（不再按周日对齐），行 = 块内日序——每行仍是固定的
// 星期几（只是列首随 today 滚动），换来「网格永远是满矩形 + 最后一列满列 + 最后一格是今天」。
// 旧口径 gridStart = weekStart(today) - 52*7 在非周六时尾列只有部分格，矩形右下有破口。
QDate ContributionHeatmap::gridStart(const QDate &today)
{
    return today.addDays(-(kWeeks * kDaysPerWeek - 1));
}

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
    // 顶部多留一行页眉带（kHeaderHeight）放「会话活动」标题，与参考图卡片顶部页签行对齐；
    // 底部仍留月份标签带高度。
    const int h = kHeaderHeight + kPadding + kDaysPerWeek * (kCellSize + kCellGap) - kCellGap
                  + kMonthLabelHeight + kPadding;
    return QSize(w, h);
}

QSize ContributionHeatmap::minimumSizeHint() const
{
    return sizeHint();
}

QRect ContributionHeatmap::gridRect() const
{
    // 顶部下移 kHeaderHeight 让出页眉带；网格高度不变，故 sizeHint 相应增长。
    return QRect(kPadding, kPadding + kHeaderHeight,
                 kWeeks * (kCellSize + kCellGap) - kCellGap,
                 kDaysPerWeek * (kCellSize + kCellGap) - kCellGap);
}

QPair<int, int> ContributionHeatmap::cellPosition(const QDate &date, const QDate &today) const
{
    const int offset = gridStart(today).daysTo(date);
    if (offset < 0 || offset >= kWeeks * kDaysPerWeek)
        return {-1, -1}; // 窗口外（早于首日或晚于今天）无坐标
    return {offset / kDaysPerWeek, offset % kDaysPerWeek};
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
    return gridStart(QDate::currentDate()).addDays(col * kDaysPerWeek + row);
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

    // 页眉带标题「会话活动」：本网格唯一口径（token 消耗），故单个左对齐标题，
    // 不复制参考图「会话 / Credits」双页签——本仓没有第二个数据源，造假页签更糟。
    // 用 monthText 中性灰（非强调绿），字号 13 半粗，基线落在 header 带垂直居中。
    p.setPen(palette.monthText);
    QFont titleFont = p.font();
    titleFont.setPixelSize(13);
    titleFont.setWeight(QFont::DemiBold);
    p.setFont(titleFont);
    p.drawText(QRect(kPadding, 0, width() - 2 * kPadding, kHeaderHeight),
               Qt::AlignLeft | Qt::AlignVCenter, tr("会话活动"));

    // 网格：列 = 连续 7 天块（0..52），行 = 块内日序（0..6）。整幅 53×7 恒满格——
    // 窗口首日 = today - 370，故最后一格（52,6）就是今天，不再有「未来日期留空」的破口。
    const QDate windowStart = gridStart(today);
    p.setPen(Qt::NoPen);
    for (int col = 0; col < kWeeks; ++col) {
        const QDate colStart = windowStart.addDays(col * kDaysPerWeek);
        for (int row = 0; row < kDaysPerWeek; ++row) {
            const QDate date = colStart.addDays(row);
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

    // 月份标签（底部）：标签贴在「含该月 1 号的那一列」——列首不再对齐周日，
    // 月界常落在列中段，只看列首月份会让标签晚 1-6 天。首列不含 1 号时补它自己的月份，
    // 否则左端空白。月长 ≥28 天 = 4 列，标签间距天然 ≥3 列，不会互叠。
    p.setPen(palette.monthText);
    QFont monthFont = p.font();
    monthFont.setPixelSize(11);
    p.setFont(monthFont);
    const int labelY = grid.bottom() + kMonthLabelHeight - 2;
    for (int col = 0; col < kWeeks; ++col) {
        const QDate colStart = windowStart.addDays(col * kDaysPerWeek);
        int labelMonth = -1;
        for (int row = 0; row < kDaysPerWeek; ++row) {
            const QDate date = colStart.addDays(row);
            if (date.day() == 1)
                labelMonth = date.month();
        }
        if (labelMonth < 0 && col > 0)
            continue;
        if (labelMonth < 0)
            labelMonth = colStart.month(); // 首列：窗口起点所在月（可能是该月的残余几天）
        // 末列标签按列左缘起画会溢出卡片右缘，按文本宽度回推
        const int textW = p.fontMetrics().horizontalAdvance(tr("%1月").arg(labelMonth));
        const int x = qMin(grid.left() + col * step, grid.right() - textW + 1);
        p.drawText(x, labelY, tr("%1月").arg(labelMonth));
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
        // 四个 token 字段走紧凑口径：热力图没有行内读数，tooltip 就是唯一的展示面，
        // 长数字在这里同样刺眼（跨会话合并后的单日 prompt 可达七位）。calls 是次数
        // 不是 token，量级小且用户会拿它对账「今天回读了几次」，保持精确整数。
        tip = tr("%1\n输入 %2 · 输出 %3\n缓存命中 %4 · 思考 %5\n回读 %6 次")
                  .arg(date.toString(QStringLiteral("yyyy-MM-dd")),
                       UsageTokens::formatCompact(day.prompt),
                       UsageTokens::formatCompact(day.completion),
                       UsageTokens::formatCompact(day.cached),
                       UsageTokens::formatCompact(day.reasoning))
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
