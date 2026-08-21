#pragma once

#include "BasePage.h"
#include "MessageBubbleWidget.h"

class FluVScrollView;
class ChatMsgEdit;

class ChatSessionPage : public BasePage
{
    Q_OBJECT
public:
    explicit ChatSessionPage(QWidget *parent = nullptr);

    void addMessage(MessageBubbleWidget::Role role, const QString &content);
    void scrollToBottom();
    void clearMessages();

    void onThemeChanged() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    FluVScrollView *m_scrollView = nullptr;
    ChatMsgEdit *m_inputEdit = nullptr;
};
