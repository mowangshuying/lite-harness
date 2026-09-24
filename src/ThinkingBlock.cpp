#include "ThinkingBlock.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextCursor>
#include <QTimer>
#include <QtMath>

#include <FluUtils.h>

ThinkingBlock::ThinkingBlock(QWidget *parent) : CollapsibleBlock(parent)
{
    // 常规展开限高；进行态单行压缩见 expandedHeightCap 覆写
    m_maxExpandedHeight = kMaxThinkingHeight;

    // ---- 标题栏：图标 + 耗时文案 + 折叠箭头 ----
    auto *headerLayout = initHeader("thinkingHeader", 12, 0, 10, 0);

    m_iconLabel = createGlyphLabel("thinkingIcon");
    m_titleLabel = createTitleLabel("thinkingTitle");
    m_arrowLabel = createGlyphLabel("thinkingArrow");

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 思考内容区：纯文本，尺寸固定为 min(自然高度, 上限)，动画期间仅平移 ----
    initContent("thinkingContent");

    // 主题装配必须在子类构造尾：此刻 m_iconLabel/m_arrowLabel 已赋值，
    // initTheme 内虚派发 refreshIcons 才安全（基类构造期调虚函数的经典陷阱）
    initTheme("ThinkingBlock.qss");

    initCollapsed();
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

// ---- 流式进行态 ----

void ThinkingBlock::startLive()
{
    if (m_live)
        return;
    m_live = true;
    m_liveDots = 0;
    m_titleLabel->setText(liveText());

    // 圆点轮播骨架（400ms 懒建定时器）已下沉基类
    startLiveTimer();

    if (!m_userInteracted)
        setExpanded(true);
}

void ThinkingBlock::stopLive(int seconds)
{
    stopLiveTimer();
    setThinkingDuration(seconds);
    if (!m_userInteracted)
    {
        setExpanded(false);   // 折叠动画启动前的重测已按完整限高进行
    }
    else
    {
        scheduleMeasure();    // 用户保持展开：解除单行限高，按完整限高重测
    }
}

void ThinkingBlock::appendLiveText(const QString &delta)
{
    QTextCursor cur = m_content->textCursor();
    cur.movePosition(QTextCursor::End);
    cur.insertText(delta);

    // 展开态钉底跟随最新思考：延迟一帧等 measureContent 更新尺寸后再取最大值
    if (m_expanded)
    {
        QTimer::singleShot(0, this, [this]() {
            QScrollBar *bar = m_content->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    }
}

QString ThinkingBlock::liveText() const
{
    return tr("思考中") + QStringLiteral(".").repeated(m_liveDots);
}

int ThinkingBlock::expandedHeightCap() const
{
    // 进行态且用户未手动操作时压缩为单行，钉底滚动只显示最新一行思考内容；
    // 其余情况用构造期写入 m_maxExpandedHeight 的完整限高
    if (m_live && !m_userInteracted)
        return liveLineHeight();
    return m_maxExpandedHeight;
}

void ThinkingBlock::toggleExpanded()
{
    // 仅用户点击路径经过此处（程序化切换走 setExpanded）：记住用户意愿，
    // 后续进行/终态的自动展开折叠不再覆盖
    m_userInteracted = true;
    setExpanded(!m_expanded);
}

void ThinkingBlock::refreshIcons()
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

int ThinkingBlock::liveLineHeight() const
{
    // 单行思考文本高度：文档默认字体（随 QSS 主题）行高 + 内容区上下 padding
    const QFontMetricsF fm(m_content->document()->defaultFont());
    return qCeil(fm.lineSpacing()) + kVerticalChrome;
}
