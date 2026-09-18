#pragma once

#include "BasePage.h"
#include "MessageBubbleWidget.h"
#include <QPointer>

class FluVScrollView;
class ChatMsgEdit;
class AgentLoop;
class PermissionCard;
class TodoCard;

class ChatSessionPage : public BasePage
{
    Q_OBJECT
public:
    explicit ChatSessionPage(QWidget *parent = nullptr);

    void addMessage(MessageBubbleWidget::Role role, const QString &content);
    void startConversation(const QString &text);
    // 宿主注入初始模型（同步输入区下拉与 AgentLoop；需在 startConversation 前调用使首轮即用该模型）
    void setModel(const QString &model);
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
    QPointer<TodoCard> m_todoCard;                    // 会话流常驻任务清单卡（首次 todoUpdated 挂载，此后就地刷新；clearMessages 销毁后置空）
};
