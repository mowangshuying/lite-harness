#include "NewChatPage.h"
#include <FluUtils.h>
#include <QLabel>
#include <QVBoxLayout>

NewChatPage::NewChatPage(QWidget *parent) : BasePage(parent)
{
    auto welcomeLabel = new QLabel("New Chat", this);
    welcomeLabel->setObjectName("welcomeLabel");
    welcomeLabel->setAlignment(Qt::AlignCenter);

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->addWidget(welcomeLabel, 1);

    onThemeChanged();
}

void NewChatPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("NewChatPage.qss", this, FluThemeUtils::getUtils()->getTheme());
}