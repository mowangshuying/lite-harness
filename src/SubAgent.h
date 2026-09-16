#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>

#include <functional>

#include "AgentLoop.h" // ToolHandler 为 AgentLoop 嵌套类型（friend class SubAgent 授权访问）

class QProcess;

// task 子代理（lcc s06 run_subagent 等价）：以全新会话上下文运行的轻量异步状态机。
// - 独立 messages（[system, user:prompt] 起步，不继承主循环历史）、独立 ChatStream、
//   独立串行工具队列、独立权限暂停槽位（m_awaitingPermission / m_pendingPermissionCall）
// - 黑盒：思考/回复增量与内部工具执行一律不发 UI 展示信号，唯一产物是最终汇总文本
//   （由宿主作为 task 工具的 tool_result 回填）；无嵌套 QEventLoop，全事件驱动
// - 权限转发：命中询问规则时暂停自身并经自身 permissionRequired 信号发出，
//   宿主以信号直连方式转发为主循环同名 3 参信号（UI 契约零改动）
// - 轮次预算 kMaxSubagentTurns（lcc range(50)）：每次发起请求（含 Stop 续跑）消耗一次，
//   预算耗尽仍未产出最终答案时以停跑文案收尾
// - 取消：cancel() 幂等地断流、kill 子进程、抑制完成回调；配对收口由宿主
//   AgentLoop::cancelSubAgent() 统一处理（stop()/错误链/析构三路复用）
class SubAgent : public QObject
{
    Q_OBJECT
public:
    // 完成回调：子代理唯一对外出口（最终汇总 / "Error: ..." / 停跑文案）；
    // 宿主触发 cancel() 后不再调用
    using CompleteHandler = std::function<void(const QString &result)>;

    SubAgent(AgentLoop *host, QObject *parent, const QString &workDir,
             const QString &model, const QString &prompt);

    // 启动（构建独立初始历史后发起首个请求）；onComplete 恰好被调用一次（除非被 cancel）
    void start(CompleteHandler onComplete);

    // 权限询问中（宿主 resolvePermission 据此路由裁决）
    bool isAwaitingPermission() const { return m_awaitingPermission; }
    // 用户裁决：deny 回填 "Permission denied" 并续跑队列；allow 走跳过询问规则的执行路径
    void resolvePermission(bool allow);
    // 取消：断流、kill 子进程、清队列、抑制回调（幂等，可重复调用）
    void cancel();

signals:
    // 与主循环 AgentLoop::permissionRequired 的 3 参契约一致；宿主信号直连转发
    void permissionRequired(const QString &toolName, const QString &summary, const QString &reason);

private:
    // 发起一次流式请求（先做轮次预算检查，超预算以停跑文案收尾）
    void startChatRequest();
    // 有工具调用：追加带 tool_calls 的 assistant 消息并进入工具执行链
    void continueWithToolResults(const QJsonObject &assistantMessage);
    // 串行队列驱动：空队列回填并再次请求，否则取队首执行
    void runNextTool();
    // 单工具执行（与主循环 executeTool 同构：PreToolUse 门 → bash 异步 / handler 表同步 →
    // PostToolUse → 收口；门钩子链复用宿主注册表，暂停状态存自身槽位）
    void executeTool(const QJsonObject &toolCall, bool permissionGranted);
    // 黑盒收口：仅回填 tool 结果消息并续跑队列，不发任何 UI 展示信号
    void onToolFinished(const QJsonObject &toolCall, const QString &output);
    // bash 异步执行（与主循环同一套黑名单/超时/截断文案，进程簿记独立）
    void executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args);
    // 完结（幂等保护后触发一次完成回调）
    void finish(const QString &result);
    // 子代理 system prompt（独立于主循环 makeSystemPrompt）
    static QString subSystemPrompt(const QString &workDir);

    AgentLoop *m_host = nullptr;            // 宿主（钩子注册表与权限信号转发；父子关系保证生命周期）
    QString m_workDir;                      // 创建时继承宿主工作目录（沙箱根，宿主改目录不影响运行中的子代理）
    QString m_model;
    QString m_prompt;                       // 任务描述（独立会话的首条 user 消息）
    QVector<QJsonObject> m_messages;        // 独立对话历史
    QJsonArray m_pendingToolCalls;          // 待执行 tool 调用队列
    QJsonArray m_toolResultsReady;          // 已执行完、待回填的 tool 结果消息
    QPointer<QObject> m_currentStream = nullptr; // 当前 ChatStream（弱引用）
    QList<QProcess *> m_activeProcesses;    // 正在运行的 QProcess，cancel()/析构时 kill
    QJsonObject m_pendingPermissionCall;    // 等待权限裁决的工具调用（自身队列暂停上下文）
    bool m_awaitingPermission = false;      // 权限询问中
    int m_turns = 0;                        // 已发起的请求次数（lcc range(50) 计数语义）
    bool m_settled = false;                 // finish() 已触发（幂等保护）
    bool m_cancelled = false;               // 已取消（回调抑制）
    CompleteHandler m_onComplete;
    // 同步 handler 表（lcc subToolsHandlers 等价：仅 read/write/edit/glob；
    // bash 为异步特判，todo_write/task 无表项 → 即便模型幻觉调用也只回填 Unknown）
    QHash<QString, AgentLoop::ToolHandler> m_handlers;
};
