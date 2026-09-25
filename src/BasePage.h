#pragma once

#include <FluWidget.h>

/// 所有导航页基类。
/// 主题契约：构造期不做首刷——基类构造期调虚函数只会停在基类 vtable，
/// 派生页 QSS 无法生效（旧实现靠派生构造尾「再调一次」补救，属时序债）。
/// 现由派生页在构造尾部显式 ThemeAware::bind("<页面>.qss", this) 完成
/// 首载与 themeChanged 联动；本类 onThemeChanged 仅在首刷缺失时由
/// FluWidget 基类的自动联动兜底派发 BasePage.qss（对已 bind 的页面，
/// 页面级 QSS 随后整体覆盖同一控件样式表，终态不变）。
class BasePage : public FluWidget
{
    Q_OBJECT
public:
    BasePage(QWidget* parent = nullptr);

    virtual ~BasePage();

    void onThemeChanged() override;
};
