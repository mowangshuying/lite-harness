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
#include <TongYiOpenAi/TongYiOpenAi.hpp>


FRAMELESSHELPER_USE_NAMESPACE
LiteHarness::LiteHarness(QWidget *parent) : FluFrameLessWidget(parent)
{
    TongYiOpenAi::__initByEnv();
    __initUI();
    __initNavView();
    __connect();
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
    m_navView->setViewWidth(200);
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

    auto sessionPage = new ChatSessionPage;
    sessionPage->addMessage(MessageBubbleWidget::Role::User, text);
    m_sessions.insert(key, sessionPage);
    m_sLayout->addWidget(key, sessionPage);

    auto sessionsItem = (FluVNavigationIconTextItem *)m_navView->getItemByKey("SessionsGroup");
    auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, "SessionsGroup");
    if (childItem == nullptr)
        return;

    if (sessionsItem->getItems().size() == 1)
        sessionsItem->onItemClicked();
    else
        sessionsItem->adjustItemHeight(sessionsItem);

    childItem->onItemClicked();
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
