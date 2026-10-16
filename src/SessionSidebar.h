#pragma once

#include <QList>
#include <QPair>
#include <QWidget>
#include <QJsonArray>
#include <QtGlobal> // qsizetype

class QLabel;
class QVBoxLayout;
class QToolButton;
class SidebarSection;
class ContextMeter;
class StatusRow;

/* ============================================================================
 * SessionSidebar —— 会话侧边栏（类 opencode TUI 侧栏：纯信息面板，无导航职能）。
 *
 * 固定宽 LayoutConst::kSidebarWidth，自上而下分节：
 *   面板头部（标题 + 收起钮）
 *   会话标题 / 模型名（muted 小字）
 *   上下文占用（进度条；数字文字在状态条，本节只画条）
 *   运行状态（三态灯：空闲 / 运行中 / 等待审批）
 *   任务清单（todoUpdated 全量快照；条目 >2 时可手风琴折叠，折叠标题带汇总）
 *   队友清单（s13 名册快照；空名册整节隐藏）
 *   页脚（工作目录 + 版本号——运行时 QCoreApplication::applicationVersion()，禁硬编码）
 *
 * 「上下文占用」节只留**进度条**在本类；数字、消耗、速度三段文字在输入框下方的
 * ChatStatusStrip 单行（见 ChatStatusStrip.h）。条与字分属两处是明确分工：条是比例读数、
 * 字是精确读数，同一时刻两处必须给同一个数——所以比例算术只在 UsageTokens::usageRatio
 * 实现一次，两个视图共用，不各自除一遍。
 * 「运行状态」灯整盏留在本类（s16.1 曾下移到状态条，随后按用户要求整体搬回）：它是「现在
 * 什么状态」的常驻读数，与上下文数字/消耗/速度那类**计量数据**不同——计量数据侧栏收起时
 * 必须仍可见，状态灯则属于面板内的身份信息，单处显示即可，不在状态条重复一份（两处同时
 * 亮灯会在同一时刻给出两个读数，与上面条/字的「同一读数」纪律相悖）。
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
    /// 任务清单全量快照（[{content, status: pending|in_progress|completed}]）
    void setTodos(const QJsonArray& todos);
    /// s13 队友名册全量快照（[name, statusName token]，AgentLoop::teammateRoster 推入；
    /// 空名册隐藏整节——单代理会话零扰动）。纯视图纪律不破：数据由 ChatSessionPage 推入。
    void setTeammates(const QList<QPair<QString, QString>>& roster);
    /// 上下文占用**条**：≈token 估算 ÷ token 预算（与压缩触发口径同源；limit<=0 时按 0 画）。
    /// 本节只画条不写数——数字在 ChatStatusStrip 单行；比例走 UsageTokens::usageRatio 单一实现，
    /// 两处不会给出不同读数。tooltip 写口径边界（估算会被后续回读覆盖、不含压缩自身开销）。
    void setContextUsage(qsizetype usedTokens, qsizetype limitTokens);
    /// 运行状态灯（AgentLoop::runningChanged 转发）
    void setRunning(bool running);
    /// 审批等待灯（permissionRequired 置亮；应答/回合终结由接线侧回灭）
    void setPermissionPending(bool pending);
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
    SidebarSection*   m_contextSection = nullptr;   // 「上下文」节：只有占用条
    ContextMeter*     m_contextMeter = nullptr;     // 条本体（数字在状态条，本节不写数）
    SidebarSection*   m_statusSection = nullptr;    // 「状态」节：三态灯整盏在此（不在状态条）
    StatusRow*        m_statusRow = nullptr;        // 灯本体（圆点 + 文案）
    SidebarSection*   m_todoSection = nullptr;
    QVBoxLayout*      m_todoList = nullptr;     // 任务清单行容器
    QLabel*           m_todoEmpty = nullptr;    // 「暂无任务」占位
    SidebarSection*   m_teammateSection = nullptr;  // s13 队友名册节（空名册整节隐藏）
    QVBoxLayout*      m_teammateList = nullptr;     // 队友行容器
    QLabel*           m_footerDir = nullptr;
    QLabel*           m_footerVersion = nullptr;

    // 缓存态：主题重刷时按最新值重放
    QString           m_sessionTitle;
    QString           m_sessionModel;
    bool              m_running = false;        // 状态灯缓存态（waiting 优先于 running 归并）
    bool              m_permPending = false;    // 审批挂起缓存态

    int               m_todoCount = 0;          // 当前任务条数（折叠策略判定）
};
