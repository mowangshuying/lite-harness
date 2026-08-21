#pragma once

#include "BasePage.h"

class ChatMsgEdit;

class NewChatPage : public BasePage
{
    Q_OBJECT
public:
    NewChatPage(QWidget *parent = nullptr);

    void onThemeChanged() override;

signals:
    void newChatRequested(const QString &text);

private:
    ChatMsgEdit *m_chatMsgEdit = nullptr;
};
