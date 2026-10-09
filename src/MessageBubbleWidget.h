#pragma once

#include <FluWidget.h>
#include <QElapsedTimer>
#include <QTextBrowser>
#include <QVector>

class QTimer;
class QVBoxLayout;
class ThinkingBlock;
class ToolBlock;

class MessageBubbleWidget : public FluWidget
{
    Q_OBJECT
public:
    enum Role { User, Assistant };
    Q_ENUM(Role)

    explicit MessageBubbleWidget(Role role, QWidget *parent = nullptr);

    void setRole(Role role);
    Role role() const { return m_role; }

    void setContent(const QString &markdown);
    QString content() const;
    void refreshSize();

    // 流式渲染（打字机）：增量追加思考/正文，流结束一次性渲染 markdown
    void startStreaming(const QString &placeholder = QString());
    void appendThinkingText(const QString &delta);
    void appendText(const QString &delta);
    void finishStreaming();

    // 工具执行节点（AgentLoop::toolOutputReady）：按到达顺序内嵌到气泡时间线，
    // 将当前流式文本段冻结归档后插入可折叠的 ToolBlock，后续增量另起新段。
    // toolName 为工具名；summary 为关键参数（bash=命令行，文件类=path，glob=pattern）；
    // ok = AgentLoop::isToolFailure 取反的成败判定（B1 单源，透传给卡片驱动
    // 完成/失败词条与 ✓/✕ 字形位）。若此前 appendToolStart 挂过同名 live 卡，
    // 本次调用就地收口该卡而非新建
    void appendToolExecution(const QString &toolName, const QString &summary, const QString &output, bool ok);

    // 工具开始执行（AgentLoop::toolStarted）：B3 事前 live 卡——按到达顺序冻结正文段
    // 后挂「执行中」轮播 ToolBlock（保持折叠一行头部），同名 toolOutputReady 到达时
    // appendToolExecution 就地收口带成败字形；task 走 startTaskLive 进度卡（与
    // appendSubagentProgress 共槽幂等）；memory/compact 无此信号语义，防御跳过。
    // 串行执行契约下同名防重即匹配依据（无 callId 的既有限制，见 .cpp 注释）
    void appendToolStart(const QString &toolName, const QString &summary);

    // 权限确认卡（AgentLoop::permissionRequired）：会话页创建 PermissionCard 并接好
    // 信号后交此挂进气泡时间线（冻结当前正文段后追加），裁决留痕停在对应工具执行
    // 之前，后续工具块/正文另起新段出现在其后。气泡不依赖卡片具体类型，仅接管几何
    void appendPermissionCard(QWidget *card);

    // 外部 widget 嵌入时间线（任务清单快照卡等留痕件）：结算思考、冻结当前正文段后
    // 追加到时间线末尾，后续思考/工具/正文段另起新段出现在其下方——事件定格在
    // 「当时」位置。所有权归气泡（addWidget 自动 reparent，随气泡析构）；
    // 用户气泡/空指针防御性忽略，调用方需自行兜底插位
    void appendTimelineSection(QWidget *section);

    // Replay-only: insert a terminal (static) thinking block. history.json
    // carries no thinking duration, so this shows content only — no startLive,
    // no timer, neutral no-duration title. The round's body text is NOT
    // frozen into a new segment: it keeps rendering into the live main view
    // below this block (first round pins to timeline index 0, later rounds
    // append at the timeline end, matching live arrival order).
    void appendHistoryThinkingText(const QString &text);

    // 正文定稿：冻结当前流式段并一次性渲染 markdown，气泡保持打开（幂等）。
    // 记忆沉淀开始前调用（AgentLoop::memoryPhaseStarted），避免长文本在阻塞
    // 提取期间停留纯文本态；finishStreaming 复用同一套定稿逻辑
    void finalizeStreamedText();

    // 记忆沉淀进度 live 卡（AgentLoop::memoryPhaseStarted）：定稿正文后在时间线
    // 末尾挂「记忆整理中...」轮播卡；提取结果卡（toolName="memory"）到达时
    // 就地切换为终态留痕；无新增（stored=0 无卡）则 finishStreaming 时收口删除
    void appendMemoryProgress();

    // task 子代理实时进度行（AgentLoop::subagentProgress）：首行到达时按
    // appendToolExecution 同款冻结-建卡链挂 task live 卡（保证与前后块因果顺序），
    // 后续行同卡追加；task 终态 toolOutputReady 到达时 appendToolExecution 就地收口，
    // stop/error 终局由 finishStreaming 兜底切「已中断」保留日志
    void appendSubagentProgress(int turnNo, const QString &toolName, const QString &summary);

    // s13 观测面：按队友名建活卡（冻结-建卡链与 appendSubagentProgress 同款）。
    // 与子代理卡的区别：返回卡片指针交由页面按名登记（队友回合跨 Lead 回合存续，
    // 后续活动回挂原卡、终态由页面驱动收口），不占本页 m_liveTaskBlock 单槽，
    // finishStreaming/appendToolExecution 均不清扫它。仅助手气泡承载，用户气泡返回 nullptr
    ToolBlock *ensureTeammateCard(const QString &teammateName);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSize();
    void scheduleStreamResize();
    void scheduleSizeUpdate();  // 去抖版尺寸测量排队（见 .cpp 注释）

    // 统一配置的文段视图（主视图 + 工具块之后的新段），登记到 m_textViews 供测量
    QTextBrowser *makeTextView();
    // 首个工具块/思考块到达时，将气泡重建为纵向时间线布局（幂等）
    void rebuildAsTimeline();
    // 冻结后按需新建当前流式段视图（懒加载，未冻结时即主视图）
    QTextBrowser *ensureLiveView();
    // 思考计时：每轮独立思考区间计时（工具执行/正文打断即停，下一轮重新起表）
    void stopThinkingInterval();
    // B2 占位清除：占位文案（"处理中…"）仅是首事件到达前的观饰，绝不能随正文追加
    // 或段冻结流入正文归档——各写入/冻结点前调用，命中则清空当前段视图
    void dropPlaceholder();

private:
    // 时间线中的一个文本段：正文 markdown 原文 + 渲染视图（工具块到达时冻结）
    struct TextRun
    {
        QString markdown;
        QTextBrowser *view = nullptr;
    };

private:
    Role m_role = Assistant;
    QTextBrowser *m_content = nullptr;
    bool m_updatingSize = false;
    bool m_sizeUpdatePending = false;  // scheduleSizeUpdate 排队守卫
    bool m_streaming = false;
    ThinkingBlock *m_liveThinking = nullptr; // 当前轮思考块：每轮思考区间新建，按到达顺序插入时间线
    QVector<TextRun> m_textRuns;          // 已冻结的文本段（按到达顺序）
    QString m_liveText;                   // 当前段正文原文（不含思考）
    QTextBrowser *m_liveView = nullptr;   // 当前流式段视图（nullptr = 待新建下一段）
    QVector<QTextBrowser *> m_textViews;  // 全部文段视图（含 m_content，逐段测量尺寸）
    QVBoxLayout *m_timeline = nullptr;    // 时间线布局（出现工具块/思考块后非空）
    QElapsedTimer m_thinkingTimer;        // 当前轮思考计时器
    bool m_thinkingRunning = false;       // 当前思考区间计时进行中
    ToolBlock *m_liveMemoryBlock = nullptr; // 记忆沉淀进度卡（live 态），结果卡到达就地切换
    ToolBlock *m_liveTaskBlock = nullptr;   // task 子代理进度卡（live 态），首行进度创建、终态收口置空
    ToolBlock *m_liveToolBlock = nullptr;   // 常规工具事前 live 卡（toolStarted 创建），同名终态就地收口
    QString m_liveToolName;                 // 在途 live 卡工具名（串行契约下的匹配依据）
    QTimer *m_streamResizeTimer = nullptr;   // 流式期间测量节流
};
