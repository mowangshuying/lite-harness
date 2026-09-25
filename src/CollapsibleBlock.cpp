#include "CollapsibleBlock.h"
#include "ThemeAware.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QStyle>
#include <QTimer>
#include <QtMath>

CollapsibleBlock::CollapsibleBlock(QWidget *parent) : FluWidget(parent)
{
    // 构造期不做任何虚调用：装配全部经 initXxx 管线由子类构造函数驱动
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

CollapsibleBlock::~CollapsibleBlock() = default;

QHBoxLayout *CollapsibleBlock::initHeader(const QString &objectName,
                                          int left, int top, int right, int bottom)
{
    m_header = new QWidget(this);
    m_header->setObjectName(objectName);
    m_header->setFixedHeight(kHeaderHeight);
    m_header->setCursor(Qt::PointingHandCursor);

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(left, top, right, bottom);
    headerLayout->setSpacing(8);
    return headerLayout;
}

QLabel *CollapsibleBlock::createGlyphLabel(const QString &objectName)
{
    auto *label = new QLabel(m_header);
    label->setObjectName(objectName);
    label->setFixedSize(16, 16);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QLabel *CollapsibleBlock::createTitleLabel(const QString &objectName)
{
    auto *label = new QLabel(m_header);
    label->setObjectName(objectName);
    return label;
}

void CollapsibleBlock::initContent(const QString &objectName)
{
    // 与 FluExpander 同源：无外层布局、纯手动几何；内容 stackUnder 到头部之下，
    // 展开时从头部背后滑出，收起时藏回头部下方（头部不透明底色负责遮挡）。
    m_content = new QTextBrowser(this);
    m_content->setObjectName(objectName);
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(true);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_content->setLineWrapMode(QTextEdit::WidgetWidth);
    attachContentArea(m_content);

    // 内容写入后延迟测量完整高度（等文档内部布局完成）
    connect(m_content->document(), &QTextDocument::contentsChanged,
            this, &CollapsibleBlock::scheduleMeasure);
}

QScrollArea *CollapsibleBlock::initScrollContent(const QString &objectName)
{
    // 列表形态内容区（TodoCard）：QScrollArea 承载子类自建的行容器，
    // 自身不做文档测量——自然高度由子类覆写 measureContent 按行几何公式计算；
    // 与文本形态共用同一套滑动几何/动画骨架（定位统一操作 m_contentArea）。
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(objectName);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    attachContentArea(scroll);
    return scroll;
}

void CollapsibleBlock::attachContentArea(QWidget *area)
{
    area->stackUnder(m_header);   // 对齐 FluExpander.cpp:22：头部绘制在内容之上
    m_contentArea = area;
    setMinimumHeight(m_header->height());
    m_header->installEventFilter(this);
}

void CollapsibleBlock::initTheme(const QString &qssFileName)
{
    // 由子类构造尾显式调用：此刻派生成员已就绪，extraRefresh（refreshIcons 虚派发）可安全执行；
    // 基类构造期做同样的事会在成员未构造时访问空指针（BasePage 同类债务已整改，勿复制）。
    // QSS 首刷/重载、themeChanged 订阅与 polish 样板收敛到 ThemeAware::bind。
    ThemeAware::bind(qssFileName, this, [this] { refreshIcons(); });
}

void CollapsibleBlock::initCollapsed()
{
    m_expanded = false;
    m_contentHeight = 0;
    m_contentArea->hide();
}

QString CollapsibleBlock::liveText() const
{
    // 无进行态的子类（TodoCard）不覆写：轮播定时器只在 startLiveTimer 之后取此文案，
    // 默认空串不会被任何路径写入标题
    return QString();
}

int CollapsibleBlock::expandedHeightCap() const
{
    return m_maxExpandedHeight;
}

void CollapsibleBlock::toggleExpanded()
{
    setExpanded(!m_expanded);
}

void CollapsibleBlock::startLiveTimer()
{
    if (m_liveTimer == nullptr)
    {
        // 克制的进行时态指示：标题尾部圆点轮播（0→3），不动用 QSS 新状态
        m_liveTimer = new QTimer(this);
        m_liveTimer->setInterval(400);
        connect(m_liveTimer, &QTimer::timeout, this, [this]() {
            m_liveDots = (m_liveDots + 1) % 4;
            m_titleLabel->setText(liveText());
        });
    }
    m_liveTimer->start();
}

void CollapsibleBlock::stopLiveTimer()
{
    m_live = false;
    if (m_liveTimer)
        m_liveTimer->stop();
}

QPropertyAnimation *CollapsibleBlock::ensureHeightAnimation()
{
    if (m_anim == nullptr)
    {
        // 与 FluExpander 一致：驱动 contentHeight 属性，300ms OutCubic
        m_anim = new QPropertyAnimation(this, "contentHeight", this);
        m_anim->setDuration(300);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QPropertyAnimation::finished, this, [this]() {
            m_animating = false;
        });
    }
    return m_anim;
}

void CollapsibleBlock::startHeightAnimation(int target)
{
    m_animating = true;
    QPropertyAnimation *anim = ensureHeightAnimation();
    anim->stop();   // 动画中途重定向：从当前中途高度续起，不跳变
    anim->setStartValue(m_contentHeight);
    anim->setEndValue(target);
    anim->start();
}

void CollapsibleBlock::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;

    // 展开期禁止 resizeEvent 触发重测，避免动画终点随测量结果跳变
    m_animating = true;

    // 先停旧动画并按旧测量值预置区间：快速连点时从半程高度即停，
    // 下一帧 startExpandAnimation 重测后再以准确终点重启
    ensureHeightAnimation();
    m_anim->stop();
    m_anim->setStartValue(m_contentHeight);
    m_anim->setEndValue(m_expanded ? m_fullContentHeight : 0);

    // 延迟到下一帧再测量并启动动画：此刻布局已稳定，宽度确定，测量高度准确
    QTimer::singleShot(0, this, &CollapsibleBlock::startExpandAnimation);

    refreshIcons();   // 刷新箭头方向
    emit expandedChanged(m_expanded);
}

// 动画属性写入点：逐条对齐 FluExpander::setContentHeight（FluExpander.cpp:138-159）
void CollapsibleBlock::setContentHeight(int h)
{
    if (m_contentHeight == h)
        return;
    const int dy = h - m_contentHeight;
    m_contentHeight = h;

    // 完全收起时隐藏内容：头部 border 为半透明 rgba，
    // 内容末行底边恰与头部底缘重合，透过去会漏出一线文字
    m_contentArea->setVisible(m_expanded || h > 0);

    // 自身：布局管理的控件，靠 minimumHeight 驱动布局，同时同步 resize 立即生效
    resize(width(), m_header->height() + h);
    setMinimumHeight(m_header->height() + h);

    // 同步向上遍历父链逐帧 resize（复制 FluExpander 循环）：
    //  ancestors 在同一帧内跟随高度变化，避免布局延迟生效造成的逐帧抖动；
    // 到窗口或滚动区 viewport 为止（viewport 之上由滚动条机制接管）
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

bool CollapsibleBlock::eventFilter(QObject *watched, QEvent *event)
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

void CollapsibleBlock::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);

    // 手动几何定位（照抄 FluExpander.cpp:79-89 的公式）：
    // 头部铺满宽度固定在顶部；内容满尺寸，顶部锚定在 32 + contentHeight - full，
    // 即 contentHeight 增大时内容从头部背后向下滑出，缩小时无可见区变化导致的重排
    m_header->resize(event->size().width(), kHeaderHeight);
    m_header->move(0, 0);
    m_contentArea->resize(event->size().width(), m_fullContentHeight);
    m_contentArea->move(0, kHeaderHeight + m_contentHeight - m_fullContentHeight);

    // 块宽变化后子类副效应（ToolBlock：关键参数省略宽度随块宽重算）
    onGeometryApplied();

    // 动画进行中不重测，避免"测量→动画→重排→再测量"循环
    if (m_animating)
        return;
    // 仅宽度变化才需要重测（高度变化来自自身动画/布局，重测会引发递归）
    if (event->size().width() == event->oldSize().width())
        return;
    scheduleMeasure();
}

void CollapsibleBlock::scheduleMeasure()
{
    // 动画期间不安排测量：singleShot(0) 会延迟到动画结束后执行，
    // 此时 m_animating 已复位，测量结果会覆盖动画终点高度导致回弹
    if (m_animating)
        return;
    QTimer::singleShot(0, this, &CollapsibleBlock::measureContent);
}

void CollapsibleBlock::measureContent()
{
    // 宽度未确定时延后重试
    const int w = width();
    if (w <= 0)
    {
        scheduleMeasure();
        return;
    }

    // 展开高度上限：超过后内容区内部滚动（配置滚动条出现与否在动画前确定）；
    // 具体策略（如 ThinkingBlock 进行态压缩为单行）由子类 expandedHeightCap 决定
    const int maxExpandedHeight = expandedHeightCap();

    QTextDocument *doc = m_content->document();
    QSignalBlocker blocker(doc);

    // 内容区尺寸固定（min(自然高度, 上限)，滚动条宽度已计入），动画期间不变化
    doc->setTextWidth(w);
    const int naturalHeight = qCeil(doc->size().height()) + kVerticalChrome;

    int contentHeight = qMin(naturalHeight, maxExpandedHeight);
    if (naturalHeight > maxExpandedHeight)
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

    // 按当前展开状态刷新目标：已展开（非动画期，宽度变化触发重测）跳到新满高；
    // 折叠态保持 0，等展开动画按最新 m_fullContentHeight 启动
    // 折叠态无需定位：内容完全藏于不透明头部背后（其底边恒在头部底缘）
    if (!m_animating && m_contentHeight > 0)
        setContentHeight(m_fullContentHeight);
    updateGeometry();
}

int CollapsibleBlock::scrollbarExtentWidth() const
{
    const QScrollBar *bar = m_content->verticalScrollBar();
    if (bar->isVisible())
        return bar->width();
    return m_content->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_content);
}

void CollapsibleBlock::startExpandAnimation()
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
