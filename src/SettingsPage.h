#pragma once

#include "BasePage.h"

class SettingsPage : public BasePage
{
    Q_OBJECT
public:
    SettingsPage(QWidget* parent = nullptr);

    void onThemeChanged() override;
};