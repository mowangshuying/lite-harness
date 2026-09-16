#pragma once

#include "BasePage.h"
#include "MessageBubbleWidget.h"
#include <QPointer>

class FluVScrollView;
class ChatMsgEdit;
class AgentLoop;
class PermissionCard;

class ChatSessionPage : public BasePage
{
    Q_OBJECT
public:
    explicit ChatSessionPage(QWidget *parent = nullptr);

    void addMessage(MessageBubbleWidget::Role role, const QString &content);
    void startConversation(const QString &text);
    void scrollToBottom();
    void clearMessages();

    void onThemeChanged() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void startAssistantStream(const QString &userText);

private:
    FluVScrollView *m_scrollView = nullptr;
    ChatMsgEdit *m_inputEdit = nullptr;
    AgentLoop *m_agentLoop = nullptr;
    MessageBubbleWidget *m_currentBubble = nullptr;   // 当前流式气泡
    QPointer<PermissionCard> m_permissionCard;        // 最近一张权限卡（裁决后化为留痕仍在流中；销毁自动置空）
};
