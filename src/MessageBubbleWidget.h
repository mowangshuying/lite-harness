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
    // toolName 为工具名；summary 为关键参数（bash=命令行，文件类=path，glob=pattern）
    void appendToolExecution(const QString &toolName, const QString &summary, const QString &output);

    // 权限确认卡（AgentLoop::permissionRequired）：会话页创建 PermissionCard 并接好
    // 信号后交此挂进气泡时间线（冻结当前正文段后追加），裁决留痕停在对应工具执行
    // 之前，后续工具块/正文另起新段出现在其后。气泡不依赖卡片具体类型，仅接管几何
    void appendPermissionCard(QWidget *card);

    // 正文定稿：冻结当前流式段并一次性渲染 markdown，气泡保持打开（幂等）。
    // 记忆沉淀开始前调用（AgentLoop::memoryPhaseStarted），避免长文本在阻塞
    // 提取期间停留纯文本态；finishStreaming 复用同一套定稿逻辑
    void finalizeStreamedText();

    // 记忆沉淀进度 live 卡（AgentLoop::memoryPhaseStarted）：定稿正文后在时间线
    // 末尾挂「记忆整理中...」轮播卡；提取结果卡（toolName="memory"）到达时
    // 就地切换为终态留痕；无新增（stored=0 无卡）则 finishStreaming 时收口删除
    void appendMemoryProgress();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSize();
    void scheduleStreamResize();

    // 统一配置的文段视图（主视图 + 工具块之后的新段），登记到 m_textViews 供测量
    QTextBrowser *makeTextView();
    // 首个工具块/思考块到达时，将气泡重建为纵向时间线布局（幂等）
    void rebuildAsTimeline();
    // 冻结后按需新建当前流式段视图（懒加载，未冻结时即主视图）
    QTextBrowser *ensureLiveView();
    // 思考计时：每轮独立思考区间计时（工具执行/正文打断即停，下一轮重新起表）
    void stopThinkingInterval();

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
    QTimer *m_streamResizeTimer = nullptr;   // 流式期间测量节流
};
