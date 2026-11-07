#include "NewChatPage.h"
#include <QLabel>
#include <QPixmap> // 英雄区图标直接使用 QPixmap（原经 FluUtils.h 间接引入，收敛后显式化）
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include "AppSettings.h"
#include "ChatMsgEdit.h"
#include "ContributionHeatmap.h"
#include "LayoutConstants.h"
#include "ThemeAware.h"
#include "WorkDirPathBar.h"

// 居中行容器：列 widget 以 stretch 1 铺满行，两侧留白由 resizeEvent 手动写入布局
// 边距，列宽靠 maximumWidth 封顶（宽窗定宽居中、窄窗吃满）。水平 Ignored 策略令
// 本行不参与父布局 minimumSize 计算，断开「边距/列固有最小宽 → 上传窗口最小宽」
// 的收缩棘轮（与会话页 ChatSessionPage 同款机制）
static QWidget *makeCenteringRow(QWidget *column, QWidget *parent, QHBoxLayout **outLayout)
{
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    rowLayout->addWidget(column, 1);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    *outLayout = rowLayout;
    return row;
}

NewChatPage::NewChatPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    setLayout(vMainLayout);
    vMainLayout->setContentsMargins(LayoutConst::kSideMargin, LayoutConst::kHeroVMargin,
                                    LayoutConst::kSideMargin, LayoutConst::kHeroVMargin);
    vMainLayout->setSpacing(0);

    // —— 英雄区：图标 + 问候语 + 灰色副标题（参考图两行大字构图），整组顶部锚定 ——
    auto heroLayout = new QVBoxLayout();
    heroLayout->setSpacing(6);

    auto iconLabel = new QLabel(this);
    iconLabel->setFixedSize(LayoutConst::kNewChatIconSize, LayoutConst::kNewChatIconSize);
    QPixmap pixmap(":/res/LiteHarness.ico");
    iconLabel->setPixmap(pixmap.scaled(45, 45));
    heroLayout->addWidget(iconLabel, 0, Qt::AlignHCenter);

    m_welcomeLabel = new QLabel(tr("开始新对话"), this);
    m_welcomeLabel->setObjectName("welcomeLabel"); // 颜色/字号见 stylesheet/<theme>/NewChatPage.qss
    heroLayout->addWidget(m_welcomeLabel, 0, Qt::AlignHCenter);

    m_subtitleLabel = new QLabel(tr("选择一个工作目录，开始与 lite-harness 对话"), this);
    m_subtitleLabel->setObjectName("subtitleLabel"); // 比问候语低一档的次要灰字
    heroLayout->addWidget(m_subtitleLabel, 0, Qt::AlignHCenter);

    // —— 输入区（贴底）：ChatMsgEdit 在上、工作目录路径条在下，同栏同宽 ——
    // 栏宽由 resizeEvent 钳 maximumWidth(min(800, 可用宽))，经居中行容器手动留白
    // （修窗口收缩棘轮）；栏内子控件铺满栏宽，无需再单独居中
    m_inputDock = new QWidget(this);
    auto dockLayout = new QVBoxLayout(m_inputDock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->setSpacing(8); // 输入框与路径条间距略收紧，两者读作同一组件

    m_chatMsgEdit = new ChatMsgEdit(m_inputDock);
    dockLayout->addWidget(m_chatMsgEdit);

    // 工作目录路径条（与 ChatSessionPage 共用 WorkDirPathBar）：
    // 「工作目录  <中间省略路径>          浏览」，12px 次要灰字 + 透明扁平按钮，
    // 弱化不抢焦点；参考图把它放在输入框正下方，作为输入区的一行附属元信息
    m_workDirBar = new WorkDirPathBar(m_inputDock);
    m_workDirBar->enableBrowse();
    dockLayout->addWidget(m_workDirBar);

    // 初值：设置页默认工作目录（存在且为目录时）优先，否则进程当前目录
    applyStoredWorkDir();

    connect(m_workDirBar, &WorkDirPathBar::browseRequested, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择工作目录"), m_workDirBar->path());
        if (dir.isEmpty())
            return;                                   // 取消选择：保持原目录
        m_workDirBar->setPath(dir);                   // 临时改选，不落盘（持久化入口在设置页）
        m_workDirTouched = true;                      // 本会话内尊重手动改选，showEvent 不再覆盖
    });

    connect(m_chatMsgEdit, &ChatMsgEdit::sendMessage, this, &NewChatPage::newChatRequested);

    // 纵向构图（对齐参考图）：英雄区 + 热力图整组居中于「输入框之上」的可视空间，
    // 输入 dock 贴底。上下各挂一份等权 stretch，组心落在这段空间的中点（读作页面中部）；
    // 窗口拉高时两组仍居中+贴底，不飘散。底部再留 24 固定间隙，输入框不紧贴视口下沿。
    vMainLayout->addStretch(1);
    vMainLayout->addLayout(heroLayout);
    vMainLayout->addSpacing(28);

    // 会话活动热力图：跨会话合并后由宿主 push（本页面不直读磁盘）。尺寸走 sizeHint 定宽
    // 定高、直接 AlignHCenter，不占用居中行容器（那是给会随栏宽伸缩的输入 dock 用的）
    m_heatmap = new ContributionHeatmap(this);
    vMainLayout->addWidget(m_heatmap, 0, Qt::AlignHCenter);

    // 下方弹性空白与上方等权：共同把英雄+热力图这一组钳在输入框之上空间的中部
    vMainLayout->addStretch(1);

    vMainLayout->addWidget(makeCenteringRow(m_inputDock, this, &m_dockRowLayout), 0);
    vMainLayout->addSpacing(24);

    // 页面级 QSS：bind 完成首载与 themeChanged 联动（BasePage 构造期不再虚调用，
    // 见其注释；旧「构造尾 onThemeChanged() + 派生链调 BasePage」由这一行等价取代）
    ThemeAware::bind("NewChatPage.qss", this);
}

void NewChatPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);
    if (!m_inputDock)
        return;
    // 栏宽只设 maximumWidth + 行容器手动居中边距（Ignored 行容器不上传最小宽，
    // 断开旧 setFixedWidth 造成的窗口收缩棘轮）；与会话页 applyColumnWidth 同法
    const int avail = qMax(0, width() - 2 * LayoutConst::kSideMargin);
    const int columnWidth = qMin(LayoutConst::kColumnMaxWidth, avail);
    // 两侧留白对称居中；奇数富余 1px 落在右侧，与旧 AlignHCenter 结算一致
    const int pad = qMax(0, (avail - columnWidth) / 2);
    const int padRight = qMax(0, avail - columnWidth - pad);
    if (m_dockRowLayout)
        m_dockRowLayout->setContentsMargins(pad, 0, padRight, 0);
    m_inputDock->setMaximumWidth(columnWidth);
}

void NewChatPage::changeEvent(QEvent *event)
{
    // i18n 第八轮：欢迎语为常驻文案，LanguageChange（applyLanguage 重装 translator
    // 触发广播）时重取 tr()；当前链路启动前已装载，本钩子兜底同进程演进场景
    if (event->type() == QEvent::LanguageChange) {
        if (m_welcomeLabel)
            m_welcomeLabel->setText(tr("开始新对话"));
        if (m_subtitleLabel)
            m_subtitleLabel->setText(tr("选择一个工作目录，开始与 lite-harness 对话"));
    }
    BasePage::changeEvent(event);
}

void NewChatPage::showEvent(QShowEvent *event)
{
    BasePage::showEvent(event);
    // 每次进入发起页重读 config.json 的默认工作目录：设置页保存后无需重启即生效；
    // 用户本会话内经「浏览」手动改选过则尊重其选择，不再覆盖。
    // （首次显示会紧随构造多读一次，值同源、无害）
    if (!m_workDirTouched)
        applyStoredWorkDir();
}

void NewChatPage::applyStoredWorkDir()
{
    // 读 config.json 默认工作目录（存在且为目录时）优先，否则进程当前目录
    const QString stored = AppSettings::value(QStringLiteral("defaultWorkDir")).toString();
    m_workDirBar->setPath(!stored.isEmpty() && QFileInfo(stored).isDir()
                              ? stored
                              : QDir::currentPath());
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

void NewChatPage::setActivityData(const QHash<QDate, ActivityLedger::DayTotal> &dayMap)
{
    // 宿主 push 入口：本页面从不直读磁盘、不订阅 AgentLoop（承 SessionSidebar 的 setter-push 纪律）。
    // heatmap 空指针卫兵兜住「构造尚未完成即被调用」——启动扫描发生在主窗口构造里，
    // 早于本方法第一次可能到达；理论上到得了这里 heatmap 已就位，防御只是保守。
    if (m_heatmap)
        m_heatmap->setData(dayMap);
}
