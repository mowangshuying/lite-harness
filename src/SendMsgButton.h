#pragma once

#include <QPushButton>

class SendMsgButton : public QPushButton
{
    Q_OBJECT
public:
    SendMsgButton(QWidget *parent = nullptr);
    ~SendMsgButton();

protected:
    void onThemeChanged();
};
