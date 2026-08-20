#include "LiteHarness.h"
#include <FramelessHelper/Core/framelessmanager.h>
#include <FramelessHelper/Widgets/framelesswidgetshelper.h>
#include <FramelessHelper/Widgets/standardsystembutton.h>
#include <FramelessHelper/Widgets/standardtitlebar.h>
#include <FluThemeButton.h>
#include <QIcon>
#include "NewChatPage.h"
#include "SettingsPage.h"
#include <FluVNavigationSettingsItem.h>


FRAMELESSHELPER_USE_NAMESPACE
LiteHarness::LiteHarness(QWidget *parent) : FluFrameLessWidget(parent)
{
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
    
    m_navView->insertIconTextItem(FluAwesomeType::Pencil, "New Chat", "NewChatPage");
    
    auto newChatPage = new NewChatPage;
    m_sLayout->addWidget("NewChatPage", newChatPage);


    auto settingsItem = new FluVNavigationSettingsItem(FluAwesomeType::Settings, tr("Setting"), this);
    settingsItem->setKey("SettingsPage");
    m_navView->addItemToBottomLayout(settingsItem);

    auto settingsPage = new SettingsPage;
    m_sLayout->addWidget("SettingsPage", settingsPage);
}

void LiteHarness::__connect()
{
    /// navView;
    connect(m_navView, &FluVNavigationView::keyChanged, this, [=](QString key) {
        m_sLayout->setCurrentWidget(key);
    });

    /// theme;
    onThemeChanged();
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [=](FluTheme theme) { onThemeChanged(); });
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
