#pragma once

#include <FLuFrameLessWidget.h>
#include <FluStackedLayout.h>
#include <FluVNavigationView.h>

class LiteHarness : public FluFrameLessWidget
{
    Q_OBJECT
public:
    LiteHarness(QWidget *parent = nullptr);

    void __initUI();
    void __initNavView();

    void __connect();

/// slots;
    void onThemeChanged();
protected:
    FluStackedLayout *m_sLayout;
    FluVNavigationView *m_navView;
};