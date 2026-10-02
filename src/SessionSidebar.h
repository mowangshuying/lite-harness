#pragma once

#include <QWidget>
#include <QJsonArray>
#include <QtGlobal> // qsizetype

class QLabel;
class QVBoxLayout;
class QToolButton;
class ContextMeter;
class StatusRow;
class SidebarSection;

/* ============================================================================
 * SessionSidebar —— 会话侧边栏（类 opencode TUI 侧栏：纯信息面板，无导航职能）。
 *
 * 固定宽 LayoutConst::kSidebarWidth，自上而下分节：
 *   面板头部（标题 + 收起钮）
 *   会话标题 / 模型名（muted 小字）
 *   上下文占用（≈token 估算 ÷ 派生 token 预算：contextCharLimit/4）
 *   运行状态灯（空闲 / 运行中 / 等待审批，waiting 优先）
 *   任务清单（todoUpdated 全量快照；条目 >2 时可手风琴折叠，折叠标题带汇总）
 *   页脚（工作目录 + 版本号——运行时 QCoreApplication::applicationVersion()，禁硬编码）
 *
 * 设计约束：
 *   ① 纯视图——不订阅 AgentLoop，所有数据由 ChatSessionPage 接线后调公开 setter 推入；
 *   ② 零线程；
 *   ③ 样式走本仓三主题 stylesheet/<theme>/SessionSidebar.qss，构造尾部 ThemeAware::bind 挂接；
 *   ④ 折叠壳为本文件内轻量 SidebarSection（maximumHeight 300ms OutCubic），
 *      不复用消息流的 CollapsibleBlock——后者为嵌进气泡流的手动几何风格，
 *      在侧栏滚动列里会与其「向上遍历父链 resize」互相打架。
 * ========================================================================== */
class SessionSidebar : public QWidget {
    Q_OBJECT
public:
    explicit SessionSidebar(QWidget* parent = nullptr);

    /// 会话标题 + 模型名（title 空时显示占位「新会话」；由 ChatSessionPage 派生回传）
    void setSessionMeta(const QString& title, const QString& model);
    /// 页脚工作目录
    void setWorkDir(const QString& dir);
    /// 上下文占用：≈token 估算 ÷ token 预算（与压缩触发口径同源；limit<=0 时按 0% 展示）
    void setContextUsage(qsizetype usedTokens, qsizetype limitTokens);
    /// 运行状态灯（AgentLoop::runningChanged 转发）
    void setRunning(bool running);
    /// 审批等待灯（permissionRequired 置亮；应答/回合终结由接线侧回灭）
    void setPermissionPending(bool pending);
    /// 任务清单全量快照（[{content, status: pending|in_progress|completed}]）
    void setTodos(const QJsonArray& todos);
signals:
    /// 头部收起钮被点击（显隐偏好与恢复钮由 ChatSessionPage 管）
    void hideRequested();

private:
    /// 状态灯三态归并（waiting 优先于 running）后刷文案/色
    void applyRunState();
    /// 任务清单计数与折叠汇总
    void refreshTodoSummary(qsizetype total, qsizetype done, qsizetype active);
    /// 主题重刷时按当前主题重取 chevron 位图
    void refreshIcons();

    QToolButton*      m_hideBtn = nullptr;
    QLabel*           m_titleLabel = nullptr;   // 会话标题
    QLabel*           m_modelLabel = nullptr;   // 模型名（muted 小字）
    SidebarSection*   m_contextSection = nullptr;
    ContextMeter*     m_meter = nullptr;
    SidebarSection*   m_statusSection = nullptr;
    StatusRow*        m_statusRow = nullptr;
    SidebarSection*   m_todoSection = nullptr;
    QVBoxLayout*      m_todoList = nullptr;     // 任务清单行容器
    QLabel*           m_todoEmpty = nullptr;    // 「暂无任务」占位
    QLabel*           m_footerDir = nullptr;
    QLabel*           m_footerVersion = nullptr;

    // 缓存态：主题重刷/状态归并时按最新值重放
    QString           m_sessionTitle;
    QString           m_sessionModel;
    bool              m_running = false;
    bool              m_permPending = false;

    int               m_todoCount = 0;          // 当前任务条数（折叠策略判定）
};
