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
    // sessionDataId：会话数据目录短 ID，透传给内部 AgentLoop 实现持久化数据按会话隔离
    // （空则回退全局 .lite-harness）；须经构造注入，先于 AgentLoop 体内首次 durable 装载定值。
    explicit ChatSessionPage(const QString &sessionDataId = QString(), QWidget *parent = nullptr);

    void addMessage(MessageBubbleWidget::Role role, const QString &content);
    void startConversation(const QString &text);
    // 宿主注入初始模型（同步输入区下拉与 AgentLoop；需在 startConversation 前调用使首轮即用该模型）
    void setModel(const QString &model);
    void scrollToBottom();
    void clearMessages();
    // 从磁盘恢复后重放历史到会话流：按 wire 消息重建气泡，assistant 段用流式气泡
    // （正文 + 工具折叠块）镜像实时链路；messages 应为已剔除 system 的会话主体
    void replayHistory(const QVector<QJsonObject> &messages);
    // 从会话数据根/history.json 恢复：交 AgentLoop 载入内存历史后重放 UI，并同步模型下拉
    void restoreFromDisk();

    void onThemeChanged() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void startAssistantStream(const QString &userText);
    // 重放辅助：收尾并清空当前流式气泡（无气泡则 no-op）
    void closeReplayBubble();

private:
    FluVScrollView *m_scrollView = nullptr;
    ChatMsgEdit *m_inputEdit = nullptr;
    AgentLoop *m_agentLoop = nullptr;
    MessageBubbleWidget *m_currentBubble = nullptr;   // 当前流式气泡
    QPointer<PermissionCard> m_permissionCard;        // 最近一张权限卡（裁决后化为留痕仍在流中；销毁自动置空）
    QPointer<TodoCard> m_todoCard;                    // 会话流常驻任务清单卡（首次 todoUpdated 挂载，此后就地刷新；clearMessages 销毁后置空）
};
