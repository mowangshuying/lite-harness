#include "TodoCard.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <FluUtils.h>

// 行几何常量与 QSS #todoRow/#todoList 保持耦合：
//   行高 26px、列表上下边距各 4px、行间距 2px（改动需同步 stylesheet/*/TodoCard.qss 注释）
static constexpr int kRowHeight = 26;
static constexpr int kListVerticalMargin = 4;
static constexpr int kListSpacing = 2;

TodoCard::TodoCard(QWidget *parent) : FluWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // ---- 标题栏：[任务清单] …… [done/total] [▼]，与 ToolBlock 头部同款实色底与圆角 ----
    m_header = new QWidget(this);
    m_header->setObjectName("todoHeader");
    m_header->setFixedHeight(32);
    m_header->setCursor(Qt::PointingHandCursor);

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(12, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(m_header);
    m_titleLabel->setObjectName("todoTitle");
    m_titleLabel->setText(tr("任务清单"));

    m_countLabel = new QLabel(m_header);
    m_countLabel->setObjectName("todoCount");

    m_arrowLabel = new QLabel(m_header);
    m_arrowLabel->setObjectName("todoArrow");
    m_arrowLabel->setFixedSize(16, 16);
    m_arrowLabel->setAlignment(Qt::AlignCenter);

    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_countLabel);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 列表区：QScrollArea 承载行容器，超上限内部滚动 ----
    // 与 ThinkingBlock/ToolBlock 同源：无外层布局参与、纯手动几何；
    // stackUnder 到头部之下，展开时从头部背后滑出（头部不透明底色遮挡）。
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("todoScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->stackUnder(m_header);

    m_list = new QWidget(m_scroll);
    m_list->setObjectName("todoList");
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, kListVerticalMargin, 0, kListVerticalMargin);
    m_listLayout->setSpacing(kListSpacing);
    m_scroll->setWidget(m_list);
    m_scroll->setWidgetResizable(true); // 行容器随视口取宽，自然高度超出视口时纵向滚动

    setMinimumHeight(m_header->height());
    m_header->installEventFilter(this);

    // 主题：箭头图标 + QSS 随主题切换刷新（三态颜色全部由 QSS 属性选择器控制）
    updateThemeIcons();
    FluStyleSheetUtils::setQssByFileName("TodoCard.qss", this, FluThemeUtils::getUtils()->getTheme());
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [this](FluTheme theme) {
        updateThemeIcons();
        FluStyleSheetUtils::setQssByFileName("TodoCard.qss", this, theme);
    });

    // 初始：无任务即无高度，列表隐藏，等待第一次 setTodos
    m_contentHeight = 0;
    m_scroll->hide();
}

void TodoCard::setTodos(const QJsonArray &todos)
{
    setVisible(!todos.isEmpty()); // 空列表整卡收起：保留挂载位，等待下次出现

    rebuildRows(todos);

    // 头部完成计数（done/total），朴素呈现，语义在 tooltip
    m_countLabel->setText(QStringLiteral("%1/%2").arg(m_doneCount).arg(m_totalCount));
    m_countLabel->setToolTip(tr("已完成 %1/%2").arg(m_doneCount).arg(m_totalCount));

    scheduleMeasure(); // 下一帧按最终宽度/行高同步高度并平滑跟到位移动画
}

void TodoCard::rebuildRows(const QJsonArray &todos)
{
    clearRows();

    m_totalCount = todos.size();
    m_doneCount = 0;

    for (const QJsonValue &value : todos)
    {
        const QJsonObject obj = value.toObject();
        const QString text = obj.value(QLatin1String("content")).toString();
        const QString status = obj.value(QLatin1String("status")).toString();

        // 三态归一：in_progress→active（最强权重）、completed→done（降级留痕）、
        // 其余/未知一律按 pending 渲染（对后端扩展状态前向兼容）
        const bool active = status == QLatin1String("in_progress");
        const bool done = status == QLatin1String("completed");
        if (done)
            m_doneCount++;
        const QLatin1String rowStatus = active ? QLatin1String("active")
                                               : (done ? QLatin1String("done") : QLatin1String("pending"));

        auto *row = new QWidget(m_list);
        row->setObjectName("todoRow");
        row->setProperty("status", QString::fromLatin1(rowStatus.data(), rowStatus.size()));
        row->setFixedHeight(kRowHeight);
        row->setToolTip(text);

        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 0, 10, 0);
        rowLayout->setSpacing(8);

        // 状态记号：文本字形（○ 待办 / ● 进行中 / ✓ 完成），颜色由 QSS 行状态选择器控制
        auto *mark = new QLabel(row);
        mark->setObjectName("todoMark");
        mark->setFixedSize(16, 16);
        mark->setAlignment(Qt::AlignCenter);
        mark->setText(active ? QStringLiteral("\u25CF")   // ●
                             : (done ? QStringLiteral("\u2713")  // ✓
                                     : QStringLiteral("\u25CB"))); // ○

        auto *textLabel = new QLabel(row);
        textLabel->setObjectName("todoText");
        textLabel->setTextInteractionFlags(Qt::NoTextInteraction);

        rowLayout->addWidget(mark);
        rowLayout->addWidget(textLabel, 1);
        m_listLayout->addWidget(row);

        m_rows.append(Row{row, textLabel, text});
    }

    refreshRowTexts();
    updateGeometry();
}

void TodoCard::clearRows()
{
    for (const Row &row : m_rows)
        delete row.widget; // 同时从列表布局摘除
    m_rows.clear();
}

QString TodoCard::elidedFor(const Row &row, int avail) const
{
    QFontMetrics fm(row.textLabel->font());
    return fm.elidedText(row.fullText, Qt::ElideRight, avail);
}

void TodoCard::refreshRowTexts()
{
    // 行可用宽 = 视口宽 - 行左侧透明描边 2 - 行边距 10+10 - 记号 16 - 间距 8 - 余量 2
    const int avail = qMax(40, m_scroll->viewport()->width() - (2 + 10 + 16 + 8 + 10 + 2));
    for (const Row &row : m_rows)
        row.textLabel->setText(elidedFor(row, avail));
}

void TodoCard::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    updateThemeIcons(); // 刷新箭头方向
    scheduleMeasure();   // 下一帧测量并按展开态启动滑出/收回动画
    emit expandedChanged(m_expanded);
}

void TodoCard::scheduleMeasure()
{
    QTimer::singleShot(0, this, &TodoCard::syncHeight);
}

// 依据当前展开态把内容高度平滑跟到新值（数量变化时的"就地刷新"动效仅此一处，克制）
void TodoCard::syncHeight()
{
    measureContent();

    const int target = m_expanded ? m_fullContentHeight : 0;
    if (m_contentHeight == target)
    {
        updateGeometry();
        return;
    }

    m_animating = true;
    if (m_anim == nullptr)
    {
        // 与 ThinkingBlock / ToolBlock 一致：驱动 contentHeight，300ms OutCubic
        m_anim = new QPropertyAnimation(this, "contentHeight", this);
        m_anim->setDuration(300);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QPropertyAnimation::finished, this, [this]() {
            m_animating = false;
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_contentHeight);
    m_anim->setEndValue(target);
    m_anim->start();
}

// 列表自然高度纯由行数决定（单行等高，无需文档测量）；封顶后滚动条占据的宽度
// 由 QScrollArea 自行处理，行省略宽在 resizeEvent 中按视口宽刷新
void TodoCard::measureContent()
{
    const int n = m_rows.size();
    const int naturalHeight = n == 0
                                  ? 0
                                  : kListVerticalMargin * 2 + n * kRowHeight + (n - 1) * kListSpacing;
    m_fullContentHeight = qMin(naturalHeight, kMaxListHeight);
}

// 动画属性写入点：与 ToolBlock::setContentHeight 同一机制
void TodoCard::setContentHeight(int h)
{
    if (m_contentHeight == h)
        return;
    const int dy = h - m_contentHeight;
    m_contentHeight = h;

    // 完全收起时隐藏列表：头部 border 为半透明 rgba，
    // 列表末行底边恰与头部底缘重合，透过去会漏出一线文字
    m_scroll->setVisible(m_expanded || h > 0);

    // 自身：布局管理的控件，靠 minimumHeight 驱动布局，同时同步 resize 立即生效
    resize(width(), m_header->height() + h);
    setMinimumHeight(m_header->height() + h);

    // 同步向上遍历父链逐帧 resize：ancestors 在同一帧内跟随高度变化，
    // 避免布局延迟生效造成的逐帧抖动；到窗口或滚动区 viewport 为止
    QWidget *p = parentWidget();
    while (p && p != window())
    {
        p->resize(p->width(), p->height() + dy);
        if (p->objectName() == QStringLiteral("qt_scrollarea_viewport"))
            break;
        p = p->parentWidget();
    }
    emit sizeChanged();
}

bool TodoCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_header->rect().contains(me->position().toPoint()))
        {
            setExpanded(!m_expanded);
            return true;
        }
    }
    return FluWidget::eventFilter(watched, event);
}

void TodoCard::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);

    // 手动几何定位（与 ToolBlock 同一公式）：
    // 头部铺满宽度固定在顶部；列表满尺寸，顶部锚定在 32 + contentHeight - full，
    // 即 contentHeight 增大时列表从头部背后向下滑出
    constexpr int kHeaderHeight = 32;
    m_header->resize(event->size().width(), kHeaderHeight);
    m_header->move(0, 0);
    m_scroll->resize(event->size().width(), m_fullContentHeight);
    m_scroll->move(0, kHeaderHeight + m_contentHeight - m_fullContentHeight);

    // 行文本省略宽随视口变化（高度不随宽度变，无需重测/重排）
    refreshRowTexts();
}

void TodoCard::updateThemeIcons()
{
    const FluTheme theme = FluThemeUtils::getUtils()->getTheme();
    m_arrowLabel->setPixmap(FluIconUtils::getFluentIconPixmap(
        m_expanded ? FluAwesomeType::ChevronUp : FluAwesomeType::ChevronDown, theme, 14, 14));
}
