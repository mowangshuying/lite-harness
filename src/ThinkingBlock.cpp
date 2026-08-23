#include "ThinkingBlock.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <FluUtils.h>

ThinkingBlock::ThinkingBlock(QWidget *parent) : FluWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // ---- 标题栏：图标 + 耗时文案 + 折叠箭头 ----
    m_header = new QWidget(this);
    m_header->setObjectName("thinkingHeader");
    m_header->setFixedHeight(32);
    m_header->setCursor(Qt::PointingHandCursor);

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(12, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_iconLabel = new QLabel(m_header);
    m_iconLabel->setObjectName("thinkingIcon");
    m_iconLabel->setFixedSize(16, 16);
    m_iconLabel->setAlignment(Qt::AlignCenter);

    m_titleLabel = new QLabel(m_header);
    m_titleLabel->setObjectName("thinkingTitle");

    m_arrowLabel = new QLabel(m_header);
    m_arrowLabel->setObjectName("thinkingArrow");
    m_arrowLabel->setFixedSize(16, 16);
    m_arrowLabel->setAlignment(Qt::AlignCenter);

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 思考内容区：纯文本，折叠时高度为 0 不参与文档布局 ----
    m_content = new QTextBrowser(this);
    m_content->setObjectName("thinkingContent");
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(true);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setLineWrapMode(QTextEdit::WidgetWidth);
    m_content->setFixedHeight(0);
    m_content->setVisible(false);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(6);
    m_layout->addWidget(m_header);
    m_layout->addWidget(m_content);
    setLayout(m_layout);

    m_header->installEventFilter(this);

    // 思考内容写入后延迟测量完整高度（等文档内部布局完成）
    connect(m_content->document(), &QTextDocument::contentsChanged,
            this, &ThinkingBlock::scheduleMeasure);

    // 主题：图标 + QSS 随主题切换刷新
    updateThemeIcons();
    FluStyleSheetUtils::setQssByFileName("ThinkingBlock.qss", this, FluThemeUtils::getUtils()->getTheme());
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [this](FluTheme theme) {
        updateThemeIcons();
        FluStyleSheetUtils::setQssByFileName("ThinkingBlock.qss", this, theme);
    });

    // 默认折叠
    m_expanded = false;
    m_expandProgress = 0;
}

void ThinkingBlock::setThinkingContent(const QString &thinkingText)
{
    // 思考内容走纯文本路径（性能优于 HTML），文本颜色由 QSS 控制
    m_content->setPlainText(thinkingText);
}

void ThinkingBlock::setThinkingDuration(int seconds)
{
    m_durationSeconds = qMax(0, seconds);
    m_titleLabel->setText(durationText());
}

void ThinkingBlock::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;

    // 先保证高度已测量，动画才有正确的目标高度
    if (m_fullContentHeight <= 0)
        measureContent();

    if (m_anim == nullptr)
    {
        m_anim = new QPropertyAnimation(this, "expandProgress", this);
        m_anim->setDuration(200);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
    }
    m_anim->stop();
    m_anim->setStartValue(m_expandProgress);
    m_anim->setEndValue(m_expanded ? 100 : 0);
    m_anim->start();

    updateThemeIcons();   // 刷新箭头方向
    emit expandedChanged(m_expanded);
}

void ThinkingBlock::setExpandProgress(int progress)
{
    if (m_expandProgress == progress)
        return;
    m_expandProgress = progress;
    applyProgress();
    emit sizeChanged();
}

bool ThinkingBlock::eventFilter(QObject *watched, QEvent *event)
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

void ThinkingBlock::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);
    // 仅宽度变化才需要重测（高度变化来自自身动画/布局，重测会引发递归）
    if (event->size().width() == event->oldSize().width())
        return;
    scheduleMeasure();
}

void ThinkingBlock::toggleExpanded()
{
    setExpanded(!m_expanded);
}

void ThinkingBlock::updateThemeIcons()
{
    const FluTheme theme = FluThemeUtils::getUtils()->getTheme();
    m_iconLabel->setPixmap(FluIconUtils::getFluentIconPixmap(FluAwesomeType::Lightbulb, theme, 16, 16));
    m_arrowLabel->setPixmap(FluIconUtils::getFluentIconPixmap(
        m_expanded ? FluAwesomeType::ChevronUp : FluAwesomeType::ChevronDown, theme, 14, 14));
}

QString ThinkingBlock::durationText() const
{
    if (m_durationSeconds < 1)
        return tr("思考了 < 1 秒");
    if (m_durationSeconds < 60)
        return tr("思考了 %1 秒").arg(m_durationSeconds);
    return tr("思考了 %1 分 %2 秒").arg(m_durationSeconds / 60).arg(m_durationSeconds % 60);
}

void ThinkingBlock::scheduleMeasure()
{
    QTimer::singleShot(0, this, &ThinkingBlock::measureContent);
}

void ThinkingBlock::measureContent()
{
    // 宽度未确定时延后重试（与气泡 availableContentWidth 的兜底一致）
    const int w = width();
    if (w <= 0)
    {
        scheduleMeasure();
        return;
    }

    QTextDocument *doc = m_content->document();
    QSignalBlocker blocker(doc);
    doc->setTextWidth(w);
    m_fullContentHeight = qCeil(doc->size().height());

    applyProgress();
}

void ThinkingBlock::applyProgress()
{
    const int contentH = qRound(m_fullContentHeight * m_expandProgress / 100.0);
    m_content->setFixedHeight(contentH);
    m_content->setVisible(contentH > 0);
}