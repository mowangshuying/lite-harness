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
    // 常规展开限高：进行态恒折叠不可见，故无压缩分支（见 expandedHeightCap 覆写）
    m_maxExpandedHeight = kMaxThinkingHeight;

    // ---- 标题栏：图标 + 耗时文案 + 折叠箭头 ----
    auto *headerLayout = initHeader("thinkingHeader", 12, 0, 10, 0);

    m_iconLabel = createGlyphLabel("thinkingIcon");
    m_titleLabel = createTitleLabel("thinkingTitle");
    m_arrowLabel = createGlyphLabel("thinkingArrow");
    // 进行态预览行（P1 思考块）：思考期间块保持折叠，最新输出行显示在这里 ——
    // 替代原先「自动展开 + 单行限高 + 钉底滚动」那套（块会张开，用户读成「一直在输出」）。
    // 占 stretch 位：空文本时也不让布局跳动；文本按可用宽度 ElideRight。
    m_previewLabel = createTitleLabel("thinkingPreview");
    m_previewLabel->setTextFormat(Qt::PlainText);

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_previewLabel, 1);
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

void ThinkingBlock::finishWithoutDuration()
{
    // Neutral terminal title without a duration claim; collapsed state is the
    // constructor default (initCollapsed), user expansion shows full content
    m_titleLabel->setText(tr("已完成思考"));
}

// ---- 流式进行态 ----

void ThinkingBlock::startLive()
{
    if (m_live)
        return;
    m_live = true;
    m_liveDots = 0;
    m_liveTail.clear();
    m_previewText.clear();
    m_titleLabel->setText(liveText());
    m_previewLabel->clear();

    // 圆点轮播骨架（400ms 懒建定时器）已下沉基类；预览行经 onLiveTick 同表刷新
    startLiveTimer();

    // 进行态不允许展开（用户裁决）：思考期间只看顶部预览行，结束后再手动展开看全文。
    // 若该块此前被展开过（同块复用 / 异常路径），这里强制收起，避免「张开」观感回归。
    if (m_expanded)
        setExpanded(false);
}

void ThinkingBlock::stopLive(int seconds)
{
    stopLiveTimer();
    setThinkingDuration(seconds);
    // 预览让位给终态标题；块保持折叠（进行态从未展开过，无需再折叠一次）。
    // 此后用户点头部即可展开看全文（toggleExpanded 不再受进行态闸门限制）。
    m_previewLabel->clear();
    m_previewText.clear();
    m_liveTail.clear();
}

void ThinkingBlock::appendLiveText(const QString &delta)
{
    QTextCursor cur = m_content->textCursor();
    cur.movePosition(QTextCursor::End);
    cur.insertText(delta);

    // 预览行只取「当前输出行」，故只保留文本末尾窗口：整段思考不常驻内存，
    // 取当前行也不需要全文。
    m_liveTail += delta;
    if (m_liveTail.size() > 512)
        m_liveTail = m_liveTail.right(512);

    // 不在此刷新预览：每 delta 刷标签会让头部每秒重排几十次，统一由 400ms 轮播 tick
    // （onLiveTick）刷新。钉底跟随同理删除 —— 进行态不允许展开，内容区不可见。
}

void ThinkingBlock::onLiveTick()
{
    updatePreview();
}

void ThinkingBlock::updatePreview()
{
    if (m_previewLabel == nullptr)
        return;
    // 当前输出行 = 末尾窗口里最后一个换行之后的部分
    const int nl = m_liveTail.lastIndexOf(QLatin1Char('\n'));
    QString line = (nl >= 0) ? m_liveTail.mid(nl + 1) : m_liveTail;
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    line = line.trimmed();
    if (!line.isEmpty())
        m_previewText = line;
    // 刚换行、当前行尚无内容时保留上一行，避免预览闪空。
    // 宽度在布局完成前可能为 0：先放完整行，下一次 tick 再按真实宽度省略。
    const int avail = m_previewLabel->width();
    m_previewLabel->setText(avail > 0
                                ? m_previewLabel->fontMetrics().elidedText(m_previewText, Qt::ElideRight, avail)
                                : m_previewText);
}

void ThinkingBlock::measureContent()
{
    // 折叠态跳过测量（效率）：流式期间文档每次变更都触发 contentsChanged→scheduleMeasure，
    // 而折叠态内容不可见，测出的高度只服务展开动画起点。跳过可省掉流式期间最重的
    // 一次文档布局 pass；展开时 startExpandAnimation 会重新 measureContent 补测。
    if (!m_expanded)
        return;
    CollapsibleBlock::measureContent();
}

QString ThinkingBlock::liveText() const
{
    // 进行态同步显示已耗时：让用户随时看到本轮思考跑了多久（不足 1 秒不占位，
    // 首帧标题早于起表，天然走此分支）。轮播每 400ms 重取文案，秒数随之跳动
    const QString dots = QStringLiteral(".").repeated(m_liveDots);
    const int elapsed = liveElapsedSeconds();
    if (elapsed < 1)
        return tr("思考中") + dots;
    return tr("思考中 · %1").arg(durationPhrase(elapsed)) + dots;
}

int ThinkingBlock::expandedHeightCap() const
{
    // 恒用构造期写入的完整限高：进行态已不允许展开（startLive 强制折叠），
    // 原先「进行态压缩为单行」的分支随之失效，删掉以免留一个走不到的口径
    return m_maxExpandedHeight;
}

void ThinkingBlock::toggleExpanded()
{
    // 进行态不允许展开（用户裁决）：思考期间只看顶部预览行，点头部无反应。
    // 终态（stopLive 之后 m_live 已复位）才允许展开看全文。
    if (m_live)
        return;
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
    return tr("思考了 %1").arg(durationPhrase(m_durationSeconds));
}

QString ThinkingBlock::durationPhrase(int seconds) const
{
    // 时长短语单源：终态「思考了 …」与进行态「思考中 · …」共用同一分支，
    // 避免两处各自演化出不同的秒/分口径
    if (seconds < 1)
        return tr("< 1 秒");
    if (seconds < 60)
        return tr("%1 秒").arg(seconds);
    return tr("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
}
