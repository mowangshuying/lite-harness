#include "BasePage.h"
#include <FluUtils.h>

BasePage::BasePage(QWidget *parent) : FluWidget(parent)
{
    onThemeChanged();
}

BasePage::~BasePage()
{
}

void BasePage::onThemeChanged()
{
    FluStyleSheetUtils::setQssByFileName("BasePage.qss", this, FluThemeUtils::getUtils()->getTheme());
}
