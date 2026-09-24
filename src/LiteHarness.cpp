#include "LiteHarness.h"
#include <FramelessHelper/Core/framelessmanager.h>
#include <FramelessHelper/Widgets/framelesswidgetshelper.h>
#include <FramelessHelper/Widgets/standardsystembutton.h>
#include <FramelessHelper/Widgets/standardtitlebar.h>
#include <FluThemeButton.h>
#include <QIcon>
#include "NewChatPage.h"
#include "ChatSessionPage.h"
#include "MessageBubbleWidget.h"
#include "SettingsPage.h"
#include <FluVNavigationSettingsItem.h>
#include <FluVNavigationIconTextItem.h>
#include <QUuid>
#include "QOpenAi.h"
#include "SessionStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QVector>
#include <QDateTime>
#include <algorithm> // std::stable_sort（Qt6 已移除 qStableSort）


FRAMELESSHELPER_USE_NAMESPACE
LiteHarness::LiteHarness(QWidget *parent) : FluFrameLessWidget(parent)
{
    QOpenAi::initByEnv();
    __initUI();
    __initNavView();
    __connect();
    // 导航骨架就绪后、事件循环前恢复历史会话（不切换当前页，仍停留在 NewChatPage）
    __restoreSessions();
}

void LiteHarness::__initUI()
{
    setWindowTitle("lite-harness");
    setWindowIcon(QIcon(":/res/LiteHarness.ico"));

    m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::black);
    m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::black);
    m_titleBar->setFixedHeight(32);

    auto hLayout = (QHBoxLayout *)m_titleBar->layout();
    auto vLayout = (QVBoxLayout *)hLayout->itemAt(1)->layout();
    auto hButtonLayout = (QHBoxLayout *)vLayout->itemAt(0)->layout();
    auto themeButton = new FluThemeButton;
    hButtonLayout->insertWidget(0, themeButton);

    m_navView = new FluVNavigationView;
    // m_navView->setViewWidth(200);
    m_sLayout = new FluStackedLayout;
    m_contentLayout->addWidget(m_navView);
    m_contentLayout->addLayout(m_sLayout);

    // __initNavView();
    FramelessWidgetsHelper::get(this)->setHitTestVisible(themeButton);
}

void LiteHarness::__initNavView()
{
    m_navView->hideSearchItem();

    auto newChatItem =  m_navView->insertIconTextItem(FluAwesomeType::Pencil, "New Chat", "NewChatPage");
    m_newChatPage = new NewChatPage;
    m_sLayout->addWidget("NewChatPage", m_newChatPage);

    auto sessionsItem = m_navView->insertIconTextItem(FluAwesomeType::List, "Sessions", "SessionsGroup");

    auto settingsItem = new FluVNavigationSettingsItem(FluAwesomeType::Settings, tr("Setting"), this);
    settingsItem->setKey("SettingsPage");
    m_navView->addItemToBottomLayout(settingsItem);

    auto settingsPage = new SettingsPage;
    m_sLayout->addWidget("SettingsPage", settingsPage);

    // setViewWidth 仅对已存在的 item 生效，须在全部 item 插入后调用，否则新增项停留在构造默认宽度 180
    m_navView->setViewWidth(256);

    /// clicked
    // emit m_navView->keyChanged("NewChatPage");
    newChatItem->itemClicked();
}

void LiteHarness::__connect()
{
    /// navView;
    connect(m_navView, &FluVNavigationView::keyChanged, this, [=](QString key) {
        m_sLayout->setCurrentWidget(key);
    });

    /// new chat;
    connect(m_newChatPage, &NewChatPage::newChatRequested, this, &LiteHarness::__createSession);

    /// theme;
    onThemeChanged();
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [=](FluTheme theme) { onThemeChanged(); });
}

void LiteHarness::__createSession(const QString &text)
{
    const int sessionId = ++m_sessionCount;
    const QString key = QString("Session_%1").arg(sessionId);

    auto title = text.simplified();
    if (title.length() > 12)
        title = title.left(12) + "...";
    if (title.isEmpty())
        title = tr("新会话");

    // 会话数据目录短 ID：一次性 uuid8（不随重启复用，避免串写），与仓内 task_<hex8> 命名风格一致；
    // 与导航 UI key（Session_%1，仅进程内自增）解耦——后者不能直接当数据目录名。
    const QString sessionDataId = QString::fromLatin1(
        QUuid::createUuid().toRfc4122().toHex().left(8));

    // 新建会话页所选工作目录（默认取设置页默认目录或进程当前目录，可临时改选）
    const QString newWorkDir = m_newChatPage->currentWorkDir();

    auto sessionPage = new ChatSessionPage(sessionDataId, newWorkDir);
    // 新会话继承新建会话页选择的模型（须在 startConversation 前注入，使首轮请求即用该模型）
    sessionPage->setModel(m_newChatPage->currentModel());
    sessionPage->startConversation(text);
    m_sessions.insert(key, sessionPage);
    m_sLayout->addWidget(key, sessionPage);

    // 登记全局会话索引：dataId/标题/模型/工作目录 + 创建/活跃时间，供下次启动恢复定位。
    // 紧随 setModel 之后，故 currentModel() 已是本会话最终模型。
    // 索引「根」固定为进程当前目录下 .lite-harness（即 __restoreSessions 启动读取处），
    // 只有条目「工作目录字段」记所选目录——若把根也改到所选目录，切换工作目录后旧会话将无法被发现。
    SessionStore::upsertEntry(
        QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness")),
        sessionDataId, title, m_newChatPage->currentModel(), newWorkDir);

    auto sessionsItem = (FluVNavigationIconTextItem *)m_navView->getItemByKey("SessionsGroup");
    auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, "SessionsGroup");
    if (childItem == nullptr)
        return;

    // 子项构造默认宽 180，addItem 不会继承父宽；借 setItemWidth 的递归语义将全部子项对齐到父项当前宽
    // （与 Gallery "先建 item 后 setViewWidth" 的启动期对齐语义一致）
    sessionsItem->setItemWidth(sessionsItem->width());

    if (m_navView->isLong())
    {
        if (sessionsItem->getItems().size() == 1)
            sessionsItem->onItemClicked();
        else
            sessionsItem->adjustItemHeight(sessionsItem);
    }
    childItem->onItemClicked();
}

void LiteHarness::__restoreSessions()
{
    const QString root = QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness"));
    const QJsonArray index = SessionStore::loadIndex(root);
    if (index.isEmpty())
        return;

    // 收集并按创建时间升序稳定排序，使恢复后导航顺序与历史创建顺序一致
    QVector<QJsonObject> entries;
    entries.reserve(index.size());
    for (const QJsonValue &v : index)
        entries.append(v.toObject());
    std::stable_sort(entries.begin(), entries.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("createdMs")).toDouble()
             < b.value(QStringLiteral("createdMs")).toDouble();
    });

    auto sessionsItem = (FluVNavigationIconTextItem *)m_navView->getItemByKey("SessionsGroup");
    for (const QJsonObject &e : std::as_const(entries))
    {
        const QString dataId = e.value(QStringLiteral("dataId")).toString();
        if (dataId.isEmpty())
            continue;
        // 导航 key 用 "Session_" + 十六进制 dataId：hex 永不等于新会话的十进制自增，避免键冲突
        const QString key = QStringLiteral("Session_") + dataId;
        if (m_sessions.contains(key))
            continue;

        // 恢复时沿用条目记录的工作目录；目录已不存在则静默回退进程当前目录，
        // 避免因外部删/移目录导致会话无法打开（数据仍在其原 sessions/<dataId> 下按所选根解析）
        const QString entryWork = e.value(QStringLiteral("workDir")).toString();
        const QString finalWork = (!entryWork.isEmpty() && QFileInfo(entryWork).isDir())
                                      ? entryWork
                                      : QDir::currentPath();

        auto page = new ChatSessionPage(dataId, finalWork);
        page->restoreFromDisk(); // 无 history.json 则为空会话页
        m_sessions.insert(key, page);
        m_sLayout->addWidget(key, page);

        const QString title = e.value(QStringLiteral("title")).toString();
        auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, "SessionsGroup");
        if (childItem == nullptr)
            continue;
        // 同 __createSession：子项默认宽 180 不继承父宽，恢复后统一对齐到父项当前宽
        sessionsItem->setItemWidth(sessionsItem->width());
        // 恢复不切换当前页（不调 childItem->onItemClicked），仅按需展开/调高保持导航视觉一致
        if (m_navView->isLong())
        {
            if (sessionsItem->getItems().size() == 1)
                sessionsItem->onItemClicked();
            else
                sessionsItem->adjustItemHeight(sessionsItem);
        }
    }
}

void LiteHarness::onThemeChanged()
{
    if (FluThemeUtils::isLightTheme())
    {
        m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::black);
        m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::black);
        m_titleBar->minimizeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->closeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->maximizeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->show();
    }
    else
    {
        m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::white);
        m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::white);

        m_titleBar->minimizeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->closeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->maximizeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->show();
    }
    FluStyleSheetUtils::setQssByFileName("LiteHarness.qss", this, FluThemeUtils::getUtils()->getTheme());   
}
