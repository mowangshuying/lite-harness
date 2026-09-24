#include "NewChatPage.h"
#include <FluUtils.h>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QDir>
#include <QToolButton>
#include <QFontMetrics>
#include <QResizeEvent>
#include "ChatMsgEdit.h"

// 英雄页构图常量：输入区栏宽上限（与 ChatMsgEdit::setMaximumWidth(800) 对齐）、
// 页面水平留白（与下方 setContentsMargins 对齐），resizeEvent 复用同一数值
static constexpr int kColumnMaxWidth = 800;
static constexpr int kSideMargin = 35;

NewChatPage::NewChatPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    setLayout(vMainLayout);
    vMainLayout->setContentsMargins(kSideMargin, 24, kSideMargin, 24);
    vMainLayout->setSpacing(0);

    // —— 英雄区：图标 + 问候语，页面唯一的大字，与输入栏成组整体垂直居中 ——
    auto heroLayout = new QVBoxLayout();
    heroLayout->setSpacing(10);

    auto iconLabel = new QLabel(this);
    iconLabel->setFixedSize(45, 45);
    QPixmap pixmap(":/res/LiteHarness.ico");
    iconLabel->setPixmap(pixmap.scaled(45, 45));
    heroLayout->addWidget(iconLabel, 0, Qt::AlignHCenter);

    auto welcomeLabel = new QLabel(tr("开始新对话"), this);
    welcomeLabel->setObjectName("welcomeLabel"); // 颜色/字号见 stylesheet/<theme>/NewChatPage.qss
    heroLayout->addWidget(welcomeLabel, 0, Qt::AlignHCenter);

    // —— 输入区（视觉中心，略低于页心）：工作目录路径条在上、ChatMsgEdit 在下，同栏同宽 ——
    // 栏宽由 resizeEvent 钳制为 min(800, 可用宽) 并居中；栏内子控件铺满栏宽，无需再单独居中
    m_inputDock = new QWidget(this);
    auto dockLayout = new QVBoxLayout(m_inputDock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->setSpacing(8); // 路径条与输入框间距略收紧，两者读作同一组件

    // 工作目录路径条：「工作目录  <中间省略路径>          浏览」，置于输入框上方，
    // 像组件的路径页眉；12px 次要灰字 + 透明扁平按钮，弱化不抢焦点；
    // 左侧缩进 4 与下方输入框内文字起点保持同列（圆角视觉对齐）
    auto workDirRow = new QHBoxLayout();
    workDirRow->setContentsMargins(4, 0, 0, 0);
    workDirRow->setSpacing(8);

    auto workDirCaption = new QLabel(tr("工作目录"), m_inputDock);
    workDirCaption->setObjectName("workDirCaption");

    m_workDirLabel = new QLabel(m_inputDock);
    m_workDirLabel->setObjectName("workDirPath");
    m_workDirLabel->setMinimumWidth(0);               // 允许被压缩，配合中间省略截断超长路径
    m_workDirLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_workDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_workDirLabel->installEventFilter(this);         // Resize 时按新宽度重新省略

    auto browseButton = new QToolButton(m_inputDock); // 而非 FluPushButton：其自带 QSS 会盖过页面级弱化样式
    browseButton->setObjectName("workDirBrowse");
    browseButton->setText(tr("浏览"));
    browseButton->setAutoRaise(true);
    browseButton->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // 字号在代码里设定（与 elide 的 QFontMetrics 量纲一致），配色交给三主题 QSS 跟随变色
    QFont secondaryFont = workDirCaption->font();
    secondaryFont.setPixelSize(12);
    workDirCaption->setFont(secondaryFont);
    m_workDirLabel->setFont(secondaryFont);
    browseButton->setFont(secondaryFont);

    workDirRow->addWidget(workDirCaption);
    workDirRow->addWidget(m_workDirLabel);
    workDirRow->addWidget(browseButton);
    dockLayout->addLayout(workDirRow);

    auto chatMsgEdit = new ChatMsgEdit(m_inputDock);
    m_chatMsgEdit = chatMsgEdit;
    dockLayout->addWidget(chatMsgEdit);

    // 初值：设置页默认工作目录（存在且为目录时）优先，否则进程当前目录
    {
        QSettings settings; // 组织/应用名已在 App.cpp 全局设定，默认构造命中同一注册表键
        const QString stored = settings.value(QStringLiteral("defaultWorkDir")).toString();
        if (!stored.isEmpty() && QFileInfo(stored).isDir())
            m_workDir = stored;
        else
            m_workDir = QDir::currentPath();
    }
    updateWorkDirDisplay();

    connect(browseButton, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择工作目录"), m_workDir);
        if (dir.isEmpty())
            return;                                   // 取消选择：保持原目录
        m_workDir = dir;
        updateWorkDirDisplay();
    });

    connect(m_chatMsgEdit, &ChatMsgEdit::sendMessage, this, &NewChatPage::newChatRequested);

    // 纵向构图：图标 + 问候语 + 输入栏（路径条在上）整组垂直居中，
    // 上下留白比例 9 : 10（组心略低于页心，与初版居中观感一致）；
    // 标题与输入栏之间用固定 36px 间距成组，拉伸窗口时组内不飘散，只整体平移
    vMainLayout->addStretch(9);
    vMainLayout->addLayout(heroLayout);
    vMainLayout->addSpacing(36);
    vMainLayout->addWidget(m_inputDock, 0, Qt::AlignHCenter);
    vMainLayout->addStretch(10);

    onThemeChanged();
}

void NewChatPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("NewChatPage.qss", this, FluThemeUtils::getUtils()->getTheme());
}

void NewChatPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);
    if (m_inputDock)
        m_inputDock->setFixedWidth(qMin(kColumnMaxWidth, width() - 2 * kSideMargin));
}

bool NewChatPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_workDirLabel && event->type() == QEvent::Resize)
        updateWorkDirDisplay();
    return BasePage::eventFilter(watched, event);
}

void NewChatPage::updateWorkDirDisplay()
{
    if (!m_workDirLabel)
        return;
    m_workDirLabel->setToolTip(m_workDir);            // 全路径经 ToolTip 兜底
    const int w = m_workDirLabel->width();
    m_workDirLabel->setText(w > 0
                            ? QFontMetrics(m_workDirLabel->font()).elidedText(m_workDir, Qt::ElideMiddle, w)
                            : m_workDir);
}

QString NewChatPage::currentModel() const
{
    // 直读输入区下拉（无后端，初值即选项首项 qwen3.8-flash）
    return m_chatMsgEdit->currentModel();
}

QString NewChatPage::currentWorkDir() const
{
    // 初值来自设置页默认目录或进程当前目录，用户可经「浏览」临时改选（不落盘）
    return m_workDir;
}
