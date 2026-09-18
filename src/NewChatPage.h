#pragma once

#include "BasePage.h"

class ChatMsgEdit;

class NewChatPage : public BasePage
{
    Q_OBJECT
public:
    NewChatPage(QWidget *parent = nullptr);

    void onThemeChanged() override;

    // 输入区当前选中的模型（宿主创建会话时读取，注入新会话继承）
    QString currentModel() const;

signals:
    void newChatRequested(const QString &text);

private:
    ChatMsgEdit *m_chatMsgEdit = nullptr;
};
