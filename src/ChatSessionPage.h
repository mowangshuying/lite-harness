#pragma once

#include "BasePage.h"
#include "MessageBubbleWidget.h"
#include <QJsonArray>
#include <QPointer>

class FluVScrollView;
class ChatMsgEdit;
class AgentLoop;
class PermissionCard;
class WorkDirPathBar;
class SessionSidebar;
class QToolButton;
class QHBoxLayout;

class ChatSessionPage : public BasePage
{
    Q_OBJECT
public:
    // sessionDataId：会话数据目录短 ID，透传给内部 AgentLoop 实现持久化数据按会话隔离
    // （空则回退全局 .lite-harness）；须经构造注入，先于 AgentLoop 体内首次 durable 装载定值。
    // workDir：会话工作目录，透传给 AgentLoop（空则回落 QDir::currentPath()）；同样须经构造注入，
    // 令会话数据根/技能/进程 cwd 随所选目录解析，先于体内首次 durable 装载定值。
    explicit ChatSessionPage(const QString &sessionDataId = QString(), const QString &workDir = QString(),
                             QWidget *parent = nullptr);

    void addMessage(MessageBubbleWidget::Role role, const QString &content);
    void startConversation(const QString &text);
    // 宿主注入初始模型（同步输入区下拉与 AgentLoop；需在 startConversation 前调用使首轮即用该模型）
    void setModel(const QString &model);
    void scrollToBottom();
    // 从磁盘恢复后重放历史到会话流：按 wire 消息重建气泡，assistant 段用流式气泡
    // （正文 + 工具折叠块）镜像实时链路；messages 应为已剔除 system 的会话主体
    void replayHistory(const QVector<QJsonObject> &messages);
    // 从会话数据根/history.json 恢复：交 AgentLoop 载入内存历史后重放 UI，并同步模型下拉
    void restoreFromDisk();

    // 会话标识/运行态/数据根转发（宿主 LiteHarness 据此定位 index 条目、删除前收尾运行中的循环、
    // 删除时连同会话磁盘数据目录一并清理）
    QString sessionDataId() const;
    QString sessionDataRoot() const;
    bool isRunning() const;
    void stop();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // 构造拆分（纯搬移不改行为）：主布局/滚动区/底部输入组（工作目录条 + ChatMsgEdit）的创建摆位
    void buildLayout();
    // 构造拆分（纯搬移不改行为）：AgentLoop 全部输出信号到 UI 的接线（finished/error/delta/工具/权限/任务/定时）
    void wireAgent();
    // 收口残留待决权限：后端 resolvePermission(false) 放行队列 + 旧卡落"已拒绝"留痕。
    // error 链与运行中 sendMessage 拒绝分支共用同一 staleCard 约定（见定义处注释）
    void dismissPendingPermission();
    void startAssistantStream(const QString &userText);
    // 重放辅助：收尾并清空当前流式气泡（无气泡则 no-op）
    void closeReplayBubble();
    // 侧栏显隐切换：写 settings.ini 键 sidebarVisible + 收起钮摆位 + 重钳消息列宽
    void setSidebarVisible(bool visible);
    // 重算并钳制消息列/输入组宽（min(800, 可用宽 − 侧栏可见时的占位)），resizeEvent 与侧栏切换共用
    void applyColumnWidth();
    // 上下文占用快照 → 侧栏（剔除 system 后交 CompactManager::estimateChars 估算）
    void refreshContextUsage();
    // 登记一次 write_file/edit_file 变更（path 走 toolSummary 的裸路径语义），交侧栏去重展示
    void recordModifiedFile(const QString &toolName, const QString &path);
    // 首条用户消息派生会话标题（抄宿主 LiteHarness 规则）并同步侧栏；已有标题则 no-op
    void maybeCaptureSessionTitle(const QString &userText);

private:
    FluVScrollView *m_scrollView = nullptr;
    // 消息列/输入组各自的居中行容器布局：列宽靠 maximumWidth 钳制，两侧留白由
    // applyColumnWidth 手动写入边距（水平 Ignored 行容器，断开窗口收缩棘轮）
    QHBoxLayout *m_scrollRowLayout = nullptr;
    QHBoxLayout *m_inputRowLayout = nullptr;
    QWidget *m_inputSection = nullptr;      // 底部同栏容器：只读工作目录条(上) + 输入框(下)，宽上限与 ChatMsgEdit 同为 800
    WorkDirPathBar *m_workDirBar = nullptr; // 只读工作目录条（省略/ToolTip 兜底细节见组件；配色见 ChatSessionPage.qss）
    ChatMsgEdit *m_inputEdit = nullptr;
    AgentLoop *m_agentLoop = nullptr;
    // QPointer：气泡若被异常销毁自动置空，
    // 与 m_permissionCard 同一初值纪律；流式槽位的显式清空语义保留（finishStreaming 不销毁气泡）
    QPointer<MessageBubbleWidget> m_currentBubble;
    // 记忆相位保留的气泡引用（异步化 P2，设计文档 §3.5a）：memoryPhaseStarted 时记下
    // 当前气泡、finished 处理中不清空槽位，供记忆结果卡（toolOutputReady "memory"）
    // 与 live 进度卡挂原时间线；memoryChainFinished 收尾定稿后释放。QPointer 纪律同
    // m_currentBubble（异常销毁自动置空）
    QPointer<MessageBubbleWidget> m_memoryBubble;
    QPointer<PermissionCard> m_permissionCard;        // 最近一张权限卡（裁决后化为留痕仍在流中；销毁自动置空）
    QJsonArray m_lastTodoSnapshot;                    // 最近一次已嵌快照的清单内容（全同更新去重防刷屏；空=尚未嵌过任何卡）

    SessionSidebar *m_sidebar = nullptr;   // 右侧信息面板（纯视图，本页单源接线；显隐偏好落 settings.ini sidebarVisible）
    QToolButton *m_restoreBtn = nullptr;   // 侧栏收起后的浮动展开钮（页面右上角，手动摆位随 resize 跟随）
    QString m_sessionTitle;                // 首条用户消息派生的会话标题（抄宿主规则，重命名不回传=已知限制）
};
