#include "ToolBlock.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QStyle>
#include <QTimer>
#include <QtMath>

#include <FluUtils.h>

ToolBlock::ToolBlock(QWidget *parent) : FluWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // ---- 标题栏：$ 提示符 + "已执行" + 命令 + 折叠箭头 ----
    m_header = new QWidget(this);
    m_header->setObjectName("toolHeader");
    m_header->setFixedHeight(32);
    m_header->setCursor(Qt::PointingHandCursor);

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(12, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_iconLabel = new QLabel(m_header);
    m_iconLabel->setObjectName("toolIcon");
    m_iconLabel->setFixedSize(16, 16);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setText(QStringLiteral("$"));

    m_titleLabel = new QLabel(m_header);
    m_titleLabel->setObjectName("toolTitle");
    m_titleLabel->setText(tr("已执行"));

    m_commandLabel = new QLabel(m_header);
    m_commandLabel->setObjectName("toolCommand");
    m_commandLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    m_arrowLabel = new QLabel(m_header);
    m_arrowLabel->setObjectName("toolArrow");
    m_arrowLabel->setFixedSize(16, 16);
    m_arrowLabel->setAlignment(Qt::AlignCenter);

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_commandLabel, 1);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 输出内容区：纯文本，尺寸固定为 min(自然高度, 上限)，动画期间仅平移 ----
    // 与 ThinkingBlock 同源：无外层布局、纯手动几何；内容 stackUnder 到头部之下，
    // 展开时从头部背后滑出，收起时藏回头部下方（头部不透明底色负责遮挡）。
    m_content = new QTextBrowser(this);
    m_content->setObjectName("toolOutput");
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(true);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_content->setLineWrapMode(QTextEdit::WidgetWidth);
    m_content->stackUnder(m_header);

    setMinimumHeight(m_header->height());

    m_header->installEventFilter(this);

    // 输出写入后延迟测量完整高度（等文档内部布局完成）
    connect(m_content->document(), &QTextDocument::contentsChanged,
            this, &ToolBlock::scheduleMeasure);

    // 主题：箭头图标 + QSS 随主题切换刷新（$ 提示符颜色由 QSS 控制）
    updateThemeIcons();
    FluStyleSheetUtils::setQssByFileName("ToolBlock.qss", this, FluThemeUtils::getUtils()->getTheme());
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [this](FluTheme theme) {
        updateThemeIcons();
        FluStyleSheetUtils::setQssByFileName("ToolBlock.qss", this, theme);
    });

    // 默认折叠：内容直接隐藏，避免末行文字透过头部半透明 border 渗出
    m_expanded = false;
    m_contentHeight = 0;
    m_content->hide();
}

void ToolBlock::setToolExecution(const QString &command, const QString &output)
{
    m_command = command;
    refreshCommandLabel();

    // 输出走纯文本路径（50k 字符内性能可控），颜色/等宽字体由 QSS 控制
    m_content->setPlainText(output.isEmpty() ? tr("(无输出)") : output);
}

QString ToolBlock::singleLineCommand() const
{
    // 头部单行显示：换行/制表压成空格，连续空白折叠（tooltip 与展开输出保留原文）
    QString line = m_command;
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    line.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return line.simplified();
}

void ToolBlock::refreshCommandLabel()
{
    if (m_command.isEmpty())
    {
        m_commandLabel->setText(QString());
        m_commandLabel->setToolTip(QString());
        return;
    }

    const QString text = singleLineCommand();

    // 头部可用宽度 = 块宽 - 固定装饰（左右边距 12+10、图标 16、箭头 16、3 段间距 8*3）- 标题宽
    static constexpr int kHeaderChrome = 12 + 10 + 16 + 16 + 8 * 3;
    const int avail = qMax(40, width() - kHeaderChrome - m_titleLabel->sizeHint().width());

    QFontMetrics fm(m_commandLabel->font());
    m_commandLabel->setText(fm.elidedText(text, Qt::ElideMiddle, avail));
    m_commandLabel->setToolTip(text);
    m_header->setToolTip(text);
}

void ToolBlock::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;

    // 展开期禁止 resizeEvent 触发重测，避免动画终点随测量结果跳变
    m_animating = true;

    if (m_anim == nullptr)
    {
        // 与 ThinkingBlock / FluExpander 一致：驱动 contentHeight 属性，300ms OutCubic
        m_anim = new QPropertyAnimation(this, "contentHeight", this);
        m_anim->setDuration(300);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QPropertyAnimation::finished, this, [this]() {
            m_animating = false;
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_contentHeight);
    m_anim->setEndValue(m_expanded ? m_fullContentHeight : 0);

    // 延迟到下一帧再测量并启动动画：此刻布局已稳定，宽度确定，测量高度准确
    QTimer::singleShot(0, this, &ToolBlock::startExpandAnimation);

    updateThemeIcons();   // 刷新箭头方向
    emit expandedChanged(m_expanded);
}

// 动画属性写入点：与 ThinkingBlock::setContentHeight 同一机制
void ToolBlock::setContentHeight(int h)
{
    if (m_contentHeight == h)
        return;
    const int dy = h - m_contentHeight;
    m_contentHeight = h;

    // 完全收起时隐藏内容：头部 border 为半透明 rgba，
    // 内容末行底边恰与头部底缘重合，透过去会漏出一线文字
    m_content->setVisible(m_expanded || h > 0);

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

bool ToolBlock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_header->rect().contains(me->position().toPoint()))
        {
            toggleExpanded();
            return true;
        }
    }
    return FluWidget::eventFilter(watched, event);
}

void ToolBlock::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);

    // 手动几何定位（与 ThinkingBlock 同一公式）：
    // 头部铺满宽度固定在顶部；内容满尺寸，顶部锚定在 32 + contentHeight - full，
    // 即 contentHeight 增大时内容从头部背后向下滑出
    constexpr int kHeaderHeight = 32;
    m_header->resize(event->size().width(), kHeaderHeight);
    m_header->move(0, 0);
    m_content->resize(event->size().width(), m_fullContentHeight);
    m_content->move(0, kHeaderHeight + m_contentHeight - m_fullContentHeight);

    // 命令省略宽度随块宽变化
    refreshCommandLabel();

    // 动画进行中不重测，避免"测量→动画→重排→再测量"循环
    if (m_animating)
        return;
    // 仅宽度变化才需要重测（高度变化来自自身动画/布局，重测会引发递归）
    if (event->size().width() == event->oldSize().width())
        return;
    scheduleMeasure();
}

void ToolBlock::toggleExpanded()
{
    setExpanded(!m_expanded);
}

void ToolBlock::updateThemeIcons()
{
    const FluTheme theme = FluThemeUtils::getUtils()->getTheme();
    m_arrowLabel->setPixmap(FluIconUtils::getFluentIconPixmap(
        m_expanded ? FluAwesomeType::ChevronUp : FluAwesomeType::ChevronDown, theme, 14, 14));
}

void ToolBlock::scheduleMeasure()
{
    // 动画期间不安排测量：singleShot(0) 会延迟到动画结束后执行，
    // 此时 m_animating 已复位，测量结果会覆盖动画终点高度导致回弹
    if (m_animating)
        return;
    QTimer::singleShot(0, this, &ToolBlock::measureContent);
}

void ToolBlock::measureContent()
{
    // 宽度未确定时延后重试
    const int w = width();
    if (w <= 0)
    {
        scheduleMeasure();
        return;
    }

    // 展开高度上限：超过后内容区内部滚动（滚动条宽度在测量前扣除）
    const int kMaxExpandedHeight = kMaxOutputHeight;

    QTextDocument *doc = m_content->document();
    QSignalBlocker blocker(doc);

    // 内容区尺寸固定（min(自然高度, 上限)）；kVerticalChrome = QSS #toolOutput 上下 padding 各 4px
    static constexpr int kVerticalChrome = 8;
    doc->setTextWidth(w);
    const int naturalHeight = qCeil(doc->size().height()) + kVerticalChrome;

    int contentHeight = qMin(naturalHeight, kMaxExpandedHeight);
    if (naturalHeight > kMaxExpandedHeight)
    {
        // 超出上限：滚动条会占用水平宽度，按扣除后的宽度重新测量，
        // 避免文档比 viewport 宽导致右侧内容被裁剪
        const int scrollbarWidth = scrollbarExtentWidth();
        if (scrollbarWidth > 0)
            doc->setTextWidth(qMax(1, w - scrollbarWidth));
    }

    m_fullContentHeight = contentHeight;
    // 固定内容区尺寸：动画期间内容不被 resize，仅移动（底部藏于头部背后）
    m_content->setFixedSize(w, contentHeight);

    // 已展开（非动画期，宽度变化触发重测）跳到新满高；折叠态保持 0，
    // 等展开动画按最新 m_fullContentHeight 启动
    if (!m_animating && m_contentHeight > 0)
        setContentHeight(m_fullContentHeight);
    updateGeometry();
}

int ToolBlock::scrollbarExtentWidth() const
{
    const QScrollBar *bar = m_content->verticalScrollBar();
    if (bar->isVisible())
        return bar->width();
    return m_content->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_content);
}

void ToolBlock::startExpandAnimation()
{
    // 此时布局已稳定，先按最终宽度测量，再按当前状态决定终点无缝启动
    measureContent();
    if (m_anim && m_anim->state() == QPropertyAnimation::Stopped)
    {
        m_anim->setStartValue(m_contentHeight);
        m_anim->setEndValue(m_expanded ? m_fullContentHeight : 0);
        m_anim->start();
    }
}
