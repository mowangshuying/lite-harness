#include "WorkDirPathBar.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include "UiScale.h" // 字号走代码设定，故须自行接 uiFontScale（见 secondaryPixelSize）

namespace {

// 次要文字基准字号（题注 / 路径 / 浏览钮三处共用，原各抄一份裸 12）。
// 本组件字号刻意在代码里设定而非写进 QSS——elide 的 QFontMetrics 量纲必须与实际字体
// 同源，写进 QSS 就得等 polish 完成才能量。代价是它绕开了 ThemeAware 的 QSS 字号缩放，
// 于是这里自己接上：读**启动快照**（非现取配置文件），与正文面（QApplication 字体）、
// 卡片面（QSS 重放）钉在同一个值上，避免「放大后路径条不跟着变」的半生效。
constexpr int kSecondaryBasePx = 12;
inline int secondaryPixelSize()
{
    return UiScale::scaledSize(kSecondaryBasePx, UiScale::runtimeFontScalePercent());
}

} // namespace

WorkDirPathBar::WorkDirPathBar(QWidget *parent) : QWidget(parent)
{
    auto row = new QHBoxLayout(this);
    row->setContentsMargins(4, 0, 0, 0); // 与下方输入框内文字起点同列（继承两页原布局的左缩进结论）
    row->setSpacing(8);

    m_caption = new QLabel(tr("工作目录"), this);
    m_caption->setObjectName("workDirCaption");

    m_pathLabel = new QLabel(this);
    m_pathLabel->setObjectName("workDirPath");
    m_pathLabel->setMinimumWidth(0);              // 允许被压缩，配合中间省略截断超长路径
    m_pathLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse); // 仅可选中复制，无修改交互
    m_pathLabel->installEventFilter(this);        // Resize 时按新宽度重新省略

    // 字号在代码里设定（与 elide 的 QFontMetrics 量纲一致），配色交给三主题 QSS 跟随变色。
    // 走代码设定就意味着绕开了 ThemeAware 的 QSS 字号缩放，故这里必须自己接 uiFontScale：
    // 读启动快照（非现取配置文件）保证与正文面/卡片面同一个值，否则放大后路径条不跟着变。
    // elide 量纲不受影响——refreshElidedText 读的是 m_pathLabel->font() 自身，字体与度量同源。
    QFont secondaryFont = m_caption->font();
    secondaryFont.setPixelSize(secondaryPixelSize());
    m_caption->setFont(secondaryFont);
    m_pathLabel->setFont(secondaryFont);

    row->addWidget(m_caption);
    row->addWidget(m_pathLabel);
}

void WorkDirPathBar::enableBrowse()
{
    // 幂等守卫（第六轮审计 C8）：本接口公开可重复调用，无守卫会向行布局增殖多个
    // 「浏览」按钮并重复透传 browseRequested
    if (m_browseButton)
        return;
    // 用原生 QToolButton 而非 FluPushButton：后者自带 QSS 会盖掉页面级弱化样式
    // （继承 NewChatPage 原结论）
    auto browse = new QToolButton(this);
    m_browseButton = browse;
    browse->setObjectName("workDirBrowse");
    browse->setText(tr("浏览"));
    browse->setAutoRaise(true);
    browse->setToolButtonStyle(Qt::ToolButtonTextOnly);
    QFont secondaryFont = browse->font();
    // 与题注/路径同一 helper：三处字号单源，且同步吃 uiFontScale
    // （本钮是文字钮，字号不一致会在同一行里读出两套尺度）
    secondaryFont.setPixelSize(secondaryPixelSize());
    browse->setFont(secondaryFont);
    connect(browse, &QToolButton::clicked, this, &WorkDirPathBar::browseRequested);

    // 构造时行布局已含 caption/path，此处追加即保持「工作目录 <路径> [浏览]」原行序
    static_cast<QHBoxLayout *>(layout())->addWidget(browse);
}

void WorkDirPathBar::setPath(const QString &path)
{
    m_path = path;
    refreshDisplay();
}

bool WorkDirPathBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pathLabel && event->type() == QEvent::Resize)
        refreshDisplay();
    return QWidget::eventFilter(watched, event);
}

void WorkDirPathBar::changeEvent(QEvent *event)
{
    // i18n 第八轮：题注/浏览钮为常驻文案，LanguageChange 时重取 tr()；
    // 路径值是数据非文案、省略文本随 setPath/resize 重算，不在此处理
    if (event->type() == QEvent::LanguageChange) {
        m_caption->setText(tr("工作目录"));
        if (m_browseButton)
            m_browseButton->setText(tr("浏览"));
    }
    QWidget::changeEvent(event);
}

void WorkDirPathBar::refreshDisplay()
{
    m_pathLabel->setToolTip(m_path); // 全路径经 ToolTip 兜底
    const int w = m_pathLabel->width();
    m_pathLabel->setText(w > 0
                         ? QFontMetrics(m_pathLabel->font()).elidedText(m_path, Qt::ElideMiddle, w)
                         : m_path);
}
