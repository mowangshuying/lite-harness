#pragma once

#include "BasePage.h"
#include "MessageBubbleWidget.h"
#include <QHash>
#include <QJsonArray>
#include <QPointer>

class FluVScrollView;
class ChatMsgEdit;
class AgentLoop;
class PermissionCard;
class WorkDirPathBar;
class SessionSidebar;
class ChatStatusStrip;
class FluInfoBar;
class QToolButton;
class QHBoxLayout;
class QTimer;

// P1-9 非阻塞通知条的三档语义（本页自有枚举，刻意不暴露 FluentUI 的 FluInfoBarSeverity
// 到头文件——页面 API 面只说「这是什么级别」，取色/图标由 .cpp 单点映射到 FluentUI 控件）。
// Info＝状态变更告知（换模型等）；Warning＝进行中且会自愈（退避重试）；
// Error＝终局失败（需用户看见但不打断，气泡仍留痕）。
enum class NoticeLevel { Info, Warning, Error };

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
    // 宿主回传会话标题（右键重命名 / 启动恢复读 index 条目）：更新本页标题单源并推给右侧信息面板，
    // 使导航子项、会话索引（config.json 键 app/sessions）、会话页面板三处同源一致；空串由面板回退占位「新会话」。
    void setSessionTitle(const QString &title);
    // 立即滚到底（不节流）：一次性路径专用——用户发送/新气泡、历史回放收尾、回底钮点击，
    // 以及外部宿主调用。同时清除「用户上翻离开」旗并恢复跟随。
    // 流式高频路径请改走 requestScrollToBottom()（合并窗口 + 跟随抑制）
    void scrollToBottom();
    // 键盘可达：宿主 Ctrl+L / 回合终局把焦点交还本页输入框
    void focusInput();    // 惰性恢复（评审 C1）：启动路径只建占位页，此处置挂起旗，
    // 首次 showEvent 消费后才 restoreFromDisk——数百气泡不再随主窗口同栈重建
    void scheduleRestoreFromDisk();
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

signals:
    // 导航徽章联动（评审 B3）：回合运行态广播给宿主，用于会话子项标题前缀 ●/✓
    void navRunningChanged(bool running);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    // 滚轮上翻=用户主动离开底部（抑制流式跟随）；滚回近底=恢复跟随
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // 构造拆分（纯搬移不改行为）：主布局/滚动区/底部输入组（工作目录条 + ChatMsgEdit +
    // 其下的 ChatStatusStrip 状态条）的创建摆位
    void buildLayout();
    // P1-9 通知条：消息列顶部的非阻塞 FluInfoBar（默认隐藏，隐藏件不占布局空间）。
    // autoDismissMs<=0＝常驻直到下一次 showNotice/hideNotice（重试态用，成功即由首个
    // 增量事件收口）；>0＝到时自动淡出（错误/换模等一次性告知用）。
    void showNotice(NoticeLevel level, const QString &title, const QString &message,
                    int autoDismissMs = 0);
    void hideNotice();
    // 重试通知的专用收口：仅当当前条是重试态点亮时才关（错误条正挂着时来增量事件，
    // 不能把错误通知顺手关掉）
    void clearRetryNotice();
    // P1-9 空态引导：消息列里除引导件本身外没有任何内容时显示，任何内容入列即永久收起
    // （气泡一旦产生就不会再全部消失——goalNotice 路径删的是占位泡，同批必有用户气泡），
    // 故用「首次入列即 hide」的 O(1) 口径，不在每次 addMessage 上扫全列（那是 O(n2)）
    void dismissEmptyHint();
    // 构造拆分（纯搬移不改行为）：AgentLoop 全部输出信号到 UI 的接线（finished/error/delta/工具/权限/任务/定时）
    void wireAgent();
    // 收口残留待决权限：后端 resolvePermission(false) 放行队列 + 旧卡落"已拒绝"留痕。
    // error 链与运行中 sendMessage 拒绝分支共用同一 staleCard 约定（见定义处注释）
    void dismissPendingPermission();
    void startAssistantStream(const QString &userText);
    // 重放辅助：收尾并清空当前流式气泡（无气泡则 no-op）
    void closeReplayBubble();
    // 侧栏显隐切换：写 config.json 键 sidebarVisible + 收起钮摆位 + 重钳消息列宽
    void setSidebarVisible(bool visible);
    // 重算并钳制消息列/输入组宽（min(800, 可用宽 − 侧栏可见时的占位)），resizeEvent 与侧栏切换共用
    void applyColumnWidth();
    // 上下文占用快照 → 底部状态条（AgentLoop::estimatedContextTokens 的 ≈token 锚定估算，
    // 预算 = contextCharLimit/4 派生，见规格修4）
    void refreshContextUsage();
    // s13 观测面：队友活卡按名收口（置终态 + 从活卡表除名，幂等——重复 settle no-op）
    void settleTeammateCard(const QString &teammateName, const QString &outcome);
    // 首条用户消息派生会话标题（抄宿主 LiteHarness 规则）并同步侧栏；已有标题则 no-op
    void maybeCaptureSessionTitle(const QString &userText);
    // 滚动合并入口（效率 P5）：流式高频/在途内容事件（每个 SSE delta 的思考与正文增量、
    // 工具起止与子代理进度行、权限卡、清单快照卡、记忆进度卡）改走此口——30ms singleShot
    // 窗口内多次请求只执行一次真实滚动，避免每条 delta 都对整个消息列做完整布局 pass
    // （scrollToBottom 内 vlayout->activate()）+ 滚动条写值。语义＝滚动始终最终贴底，流式中最多晚 30ms
    void requestScrollToBottom();
    // 终态强制结算：取消在途合并窗口并按跟随态结算（finished/error/记忆链收口/用户 stop 处调用），
    // 防止最后一个合并窗口迟到或残留导致停在半截
    void flushPendingScroll();
    // 流式跟随入口（评审 B1）：用户上翻离开底部时不拽回视野、只保持回底钮；
    // 合并窗口到期与终态 flush 均走此口（区别于强制贴底的公开 scrollToBottom）
    void followScrollToBottom();
    // 真实贴底动作（布局结算 + 滚动条写值），公开与跟随两条入口共用
    void doScrollToBottom();
    // 依当前滚动位置重算跟随态（近底=恢复跟随；离底=抑制自动滚动）
    void evaluateFollowState();
    // 回底钮显隐与摆位（滚动区视口底部居中，随 resize/侧栏切换重算）
    void updateJumpBottomBtn();

private:
    FluVScrollView *m_scrollView = nullptr;
    // 滚动合并定时器（效率 P5，singleShot 30ms；构造期创建，风格对齐 MessageBubbleWidget
    // 的 50ms 流式测量节流）：requestScrollToBottom 在窗口内首次请求时启动，到期执行一次
    // scrollToBottom；flushPendingScroll 在终态 stop 后立即执行。超时直连 followScrollToBottom，
    // 所有权随本页（QTimer parent=this），零所有权语义同其余子件
    QTimer *m_scrollCoalesceTimer = nullptr;
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
    // s13 观测面：队友名 → 活卡（活卡跨 Lead 回合存续，所有权随所挂气泡；QPointer
    // 自动跟销毁。三路终态（completed/exited/settled）+ 回合末名册兜底统一经
    // settleTeammateCard 除表收口，幂等）
    QHash<QString, QPointer<ToolBlock>> m_liveTeammateCards;

    SessionSidebar *m_sidebar = nullptr;   // 右侧信息面板（纯视图，本页单源接线；显隐偏好落 config.json sidebarVisible）
    ChatStatusStrip *m_statusStrip = nullptr; // 输入框下方状态条（轮步/消耗/速度三段文字；占用条+数字与状态灯在侧栏，见 SessionSidebar.h）
    // P1-9 通知条三件：条本体 + 自动淡出定时器 + 与消息列同栏宽的居中行布局。
    // 走「页面内常驻条」而非 FluentUI 的 FluInfoBarMgr 浮层：管理器把条做成父件上的
    // 绝对定位 toast，与本页「列宽手动钳制 + Ignored 行容器」的构图纪律冲突（会脱栏、
    // 且随窗口缩放压在侧栏上）；常驻条进布局即天然同栏，隐藏时不占位。
    FluInfoBar *m_noticeBar = nullptr;
    QWidget *m_noticeRow = nullptr;   // 通知条的居中行容器（与消息列同栏宽；条关闭时整行收起不留空档）
    QTimer *m_noticeDismissTimer = nullptr;
    QHBoxLayout *m_noticeRowLayout = nullptr;
    // 当前通知条是否由「退避重试」点亮：重试态是**自愈型**状态，首个增量到达即作废，
    // 而错误/换模通知要留到用户裁决或下一条通知覆盖——两者共用一条，靠此位区分收口时机
    bool m_noticeIsRetry = false;
    // 用户主动停止旗标：AgentLoop::stop() 的终局也走 error("已停止。")，若照原样点亮
    // 红色通知条，会把「我按了停止」报成「本轮失败」。置位/消费单点见 stop() 与 error 接线
    bool m_stopRequested = false;
    // P1-9 空态引导卡（消息列首个件）：空会话时给出输入/停止/侧栏三条用法锚点，
    // 任何内容入列即整块隐藏
    QWidget *m_emptyHint = nullptr;
    QToolButton *m_restoreBtn = nullptr;   // 侧栏收起后的浮动展开钮（页面右上角，手动摆位随 resize 跟随）
    QToolButton *m_jumpBottomBtn = nullptr; // 「回到底部」浮动钮（挂滚动区，上翻抑制跟随时出现）
    bool m_userScrolledAway = false;        // 用户主动上翻离开底部（流式跟随抑制位，回近底清除）
    bool m_restorePending = false;          // 惰性恢复挂起旗：scheduleRestoreFromDisk 置位、首次 showEvent 消费
    bool m_replaying = false;               // 回放进行中：逐泡贴底 no-op，replayHistory 尾部单次结算
    QString m_sessionTitle;                // 会话标题单源：首条用户消息派生，宿主重命名/恢复经 setSessionTitle 回传覆盖
};
