#include "BasePage.h"
#include <FluUtils.h>

BasePage::BasePage(QWidget *parent) : FluWidget(parent)
{
    // 构造期不调 onThemeChanged()：基类构造期虚派发只到本类 vtable，派生页 QSS 不会生效，
    // 旧实现靠「派生构造尾再调一次」补救，属时序债。首刷由派生页构造尾的 ThemeAware::bind 承担；
    // 后续主题切换由 FluWidget 基类自动联动虚派发兜底（契约见头文件）
}

BasePage::~BasePage()
{
}

void BasePage::onThemeChanged()
{
    FluStyleSheetUtils::setQssByFileName("BasePage.qss", this, FluThemeUtils::getUtils()->getTheme());
}
