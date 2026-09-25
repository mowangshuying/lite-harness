#include "NewChatPage.h"
#include <QLabel>
#include <QPixmap> // 英雄区图标直接使用 QPixmap（原经 FluUtils.h 间接引入，收敛后显式化）
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QDir>
#include <QResizeEvent>
#include "ChatMsgEdit.h"
#include "LayoutConstants.h"
#include "ThemeAware.h"
#include "WorkDirPathBar.h"

NewChatPage::NewChatPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    setLayout(vMainLayout);
    vMainLayout->setContentsMargins(LayoutConst::kSideMargin, 24,
                                    LayoutConst::kSideMargin, 24);
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

    // 工作目录路径条（与 ChatSessionPage 共用 WorkDirPathBar）：
    // 「工作目录  <中间省略路径>          浏览」，12px 次要灰字 + 透明扁平按钮，
    // 弱化不抢焦点；左侧缩进 4 与下方输入框内文字起点同列（圆角视觉对齐）等
    // 构图细节已随组件收敛，配色仍由本页 QSS 的 workDir* 后代选择器命中
    m_workDirBar = new WorkDirPathBar(m_inputDock);
    m_workDirBar->enableBrowse();
    dockLayout->addWidget(m_workDirBar);

    m_chatMsgEdit = new ChatMsgEdit(m_inputDock);
    dockLayout->addWidget(m_chatMsgEdit);

    // 初值：设置页默认工作目录（存在且为目录时）优先，否则进程当前目录
    {
        QSettings settings; // 组织/应用名已在 App.cpp 全局设定，默认构造命中同一注册表键
        const QString stored = settings.value(QStringLiteral("defaultWorkDir")).toString();
        m_workDirBar->setPath(!stored.isEmpty() && QFileInfo(stored).isDir()
                                  ? stored
                                  : QDir::currentPath());
    }

    connect(m_workDirBar, &WorkDirPathBar::browseRequested, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择工作目录"), m_workDirBar->path());
        if (dir.isEmpty())
            return;                                   // 取消选择：保持原目录
        m_workDirBar->setPath(dir);                   // 临时改选，不落盘（持久化入口在设置页）
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

    // 页面级 QSS：bind 完成首载与 themeChanged 联动（BasePage 构造期不再虚调用，
    // 见其注释；旧「构造尾 onThemeChanged() + 派生链调 BasePage」由这一行等价取代）
    ThemeAware::bind("NewChatPage.qss", this);
}

void NewChatPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);
    if (m_inputDock)
        m_inputDock->setFixedWidth(qMin(LayoutConst::kColumnMaxWidth,
                                        width() - 2 * LayoutConst::kSideMargin));
}

QString NewChatPage::currentModel() const
{
    // 直读输入区下拉（无后端，初值即选项首项 qwen3.8-flash）
    return m_chatMsgEdit->currentModel();
}

QString NewChatPage::currentWorkDir() const
{
    // 初值来自设置页默认目录或进程当前目录，用户可经「浏览」临时改选（不落盘）
    return m_workDirBar->path();
}
