#include "SettingsPage.h"
#include <FluUtils.h>
#include <QVBoxLayout>

SettingsPage::SettingsPage(QWidget *parent) : BasePage(parent)
{
    auto vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);
}

void SettingsPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("SettingsPage.qss", this, FluThemeUtils::getUtils()->getTheme());
}