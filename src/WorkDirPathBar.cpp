#include "WorkDirPathBar.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

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

    // 字号在代码里设定（与 elide 的 QFontMetrics 量纲一致），配色交给三主题 QSS 跟随变色
    QFont secondaryFont = m_caption->font();
    secondaryFont.setPixelSize(12);
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
    secondaryFont.setPixelSize(12);
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

void WorkDirPathBar::refreshDisplay()
{
    m_pathLabel->setToolTip(m_path); // 全路径经 ToolTip 兜底
    const int w = m_pathLabel->width();
    m_pathLabel->setText(w > 0
                         ? QFontMetrics(m_pathLabel->font()).elidedText(m_path, Qt::ElideMiddle, w)
                         : m_path);
}
