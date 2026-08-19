#pragma once

#include "BasePage.h"

class NewChatPage : public BasePage
{
    Q_OBJECT
public:
    NewChatPage(QWidget *parent = nullptr);

    void onThemeChanged() override;
};