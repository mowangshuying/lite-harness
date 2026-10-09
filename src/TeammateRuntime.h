#ifndef TEAMMATERUNTIME_H
#define TEAMMATERUNTIME_H

// TeammateRuntime — 队友运行时状态机（移植自 lcc s13 34775c8 agent_teams_manager.py
// 的 TeammateRuntime 类 :801-1063）。
//
// 【P2 交付边界（编排者裁决）】本类只落「状态机骨架 + 信箱/协议门接线 + 回合数据接口」，
// **不含真实 LLM 调用**——lcc 的 client.messages.create 一段（work() :962-970）在 P3 由
// 宿主（ChatSessionPage/AgentLoop 侧）通过 turnRequested 信号驱动、以 deliverTurnResult()
// 回填回合产物。TeammateRuntime 不持有任何 ChatStream：所有入站都是信号/函数调用回调，
// **零嵌套事件循环**（对齐 s12.0 P3 的 BackgroundTasksManager 收敛先例，AGENTS.md「零线程
// 原则」）。
//
// 与 lcc 的线程形态对应关系（有意偏离，登记）：
//   lcc 每队友一条 daemon 线程跑 run()（while work/wait_for_work，Condition 阻塞等信）
//     → lite：QTimer 心跳（间隔 AgentConst::kTeamIdleScanIntervalMs，单源在
//       AgentConstants.h，Gate② FIND-N3 迁入）驱动「wait_for_work 的单次轮询形态」；
//       干活回合由宿主信号驱动，心跳只在 Idle 态跑。
//   lcc daemon=True（宿主退出线程自然终止、从不 join/kill）
//     → lite：合作式退场正道是 shutdown 协议/cancel() → finish()；~TeammateRuntime
//       为 Gate② FIND-L 安全网兜底（未清算则走无 emit 的账目核，见 dtor 注释）。
//
// 租约三时点闭环（lcc :1058 设计说明）：出生=claim（spawn 带单 / 心跳自拉活 / 模型显式
// claim_task 都经 manager 或 TaskStore 落账）；回合边界=空闲出口调
// TaskStore::releaseCompletedAssignment（lcc work() :986）；死亡=finish() 调
// releaseTeammateAssignment + manager::finalizeTeammate（lcc run() finally :1044-1056）。
// fix-4 钉死第 4 条：两处释放点都由本状态机自调，completeTask 不做任何释放。

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "MessageBus.h"

class QTimer;
class AgentTeamsManager;
class MessageBus;
class TaskStore;

class TeammateRuntime : public QObject
{
    Q_OBJECT

public:
    // 依赖注入（fix-4 裁决 2）：manager/bus/taskStore 裸指针由宿主（P3）保证生命周期
    // ——与 lcc __init__(name, role, prompt, ...) + 全局单例引用等价，lite 禁全局。
    // taskId 可空（lcc spawn_teammate 的 task_id 可选参数）；requirePlan 决定出生提示
    // 是否追加 [Plan required] 段（lcc :836-837）。
    TeammateRuntime(const QString &name, const QString &role, const QString &prompt,
                    const QString &taskId, bool requirePlan, AgentTeamsManager *manager,
                    MessageBus *bus, TaskStore *taskStore, QObject *parent = nullptr);

    // lcc run() 的入口半段（:1033-1041 的首轮消息装配）：把 prompt（带单时追加
    // [Assigned task ...] 与 [Plan required ...] 段，lcc :825-837）装配为首条 user 消息，
    // 然后发 turnRequested 请宿主起第一回合。任务快照经 listTaskSnapshots 现读（钉死第 6
    // 条：禁从磁盘恢复队友状态）；cwd 经 assignmentCwd 现读（钉死第 3 条：禁读缓存）。
    // 任务读不到/cwd 解析失败 → lcc work() 异常形态的 lite 对应：发 error 信箱消息后
    // finish()（lcc :963-970 except → bus.send(error) + return "stop"）。
    void start();

    // 宿主侧取消（关会话等）：等价 lcc daemon 随宿主进程终止——lite 里显式走一遍
    // finish() 的清算路径（退租 + 销账），内存台账不留幽灵。
    void cancel();

    // Gate② FIND-L 析构安全网。**正道契约不变：宿主销毁序 = cancel() → delete，
    // 且 manager/bus/taskStore 必须比本对象后死**（P3 fix-5 launcher 契约钉死项）。
    // 本析构只是兜底：若宿主违约跳过 cancel() 直接 delete，仍把账目核走一遍
    // （退租 + finalizeTeammate 弹账），杜绝租约泄漏与台账幽灵；析构期一律
    // **禁 emit**（sendToLead/teamEvent/finished 都不发——Qt 惯例：析构中发信号不
    // 安全，接收方可能随父级半销毁），清理失败仅 qWarning 记账。
    // m_manager/m_bus/m_taskStore 裸指针若已被宿主先杀，则属契约违例的未定义域，
    // 本兜底不试图自救（无磁盘恢复、无悬挂探测——D7 禁磁盘复活队友）。
    ~TeammateRuntime() override;

    bool isFinished() const { return m_finished; }
    QString teammateName() const { return m_name; }

    // 供 P3 宿主构造 LLM 请求：system 提示词（lcc :811-821 逐字）与当前消息历史。
    QString systemPrompt() const;
    const QVector<QJsonObject> &messages() const { return m_messages; }

    // 回合产物回填（lcc work() :942-991 的「create 返回之后」半段）：
    //   errorMessage 非空 → lcc except 分支：error 消息发 Lead + 终止（return "stop"）；
    //   toolCalls 非空   → 逐块经 manager 的 runTeammateTool 执行（plan 门/权限/hook 全在
    //                      manager 单点），工具产物回填进 messages，再发 turnRequested
    //                      （lcc return "continue" → while 下一轮）；
    //   两者皆空         → 回合文本即总结：非 pending 门时发 result（lcc :981-985）；
    //                      门 pending → 只转 waiting_approval、result 与退租都不做
    //                      （lcc :986-989：在审案不是完工报告）；
    //                      否则 → 回合边界退租 releaseCompletedAssignment + idle_notification
    //                      + 心跳待命（lcc :990-991 return "idle"）。
    // 有工具块的回合不释放租约（lcc :354 铁律：同回合还要用 cwd 路由）。
    void deliverTurnResult(const QString &assistantText, const QJsonArray &toolCalls,
                           const QString &errorMessage = QString());

signals:
    // 请宿主发起一轮 LLM 调用（messages()/systemPrompt() 即请求体素材）。
    // lcc work() 内联的 client.messages.create 在 lite 里由宿主消费本信号实现——
    // 这样本类零网络依赖（钉死第 1 条：不引入 QOpenAi）。
    void turnRequested(const QString &teammateName);

    // 队友侧协议事件的展示钩子（P3 接 UI；与信箱注入不重复——信箱是给 Lead 模型的，
    // 本信号是给界面点的）。requestId 不适用时为空串。
    void teamEvent(const QString &type, const QString &from, const QString &content,
                   const QString &requestId);

    // 最终总结可达宿主（lcc thread finally 后 Lead 经信箱收 result 的 lite 双通道之一）。
    void taskFinished(const QString &teammateName, const QString &summary);

    // 生命周期终点：finish() 清算完成后发出（宿主据此删对象/刷新台账展示）。
    void finished(const QString &teammateName);

private slots:
    // Idle 态心跳 = lcc wait_for_work() 的单次轮询形态（:998-1023）：
    // ① drain 信箱（M8 fail-closed：空批且 lastError 非空 → 本拍放弃，信留邮箱下拍重试）；
    //    有信 → handleInbox：stop → 静默退场；托盘有内容 → 唤醒（return True 形）。
    // ② 无信 → claimNextTask（lcc :1017-1023 自拉活：装配 [Auto-claimed task ...] 首条并
    //    唤醒）；无活可拉 → 继续待命。
    // 钉死第 3 条：waiting_approval 的队友心跳照打——租约自闸挡自取（lease 占用
    // claimNextTask 首门直接拒绝），不用特判状态。
    void onHeartbeat();

private:
    // lcc handle_inbox :901-928（含 :906-909 顺序陷阱注释的 lite 形态：批内 shutdown
    // 夹中间 → 之后的信永久丢失——破坏性读取已 unlink，托盘整个丢弃、模型永远看不见）。
    // 返回 true = 该退场了（shutdown 已接受并回执）。
    bool handleInbox(const QVector<BusMessage> &inbox);

    void enterIdle();  // 启动心跳（lcc：wait_for_work 的 Condition 等待段）
    void leaveIdle();  // 停心跳（lcc：开工/退场时退出等待）
    // 账目核（Gate② FIND-L 拆分）：闩锁→停心跳→退租→finalizeTeammate 弹账。
    // **全程零 emit**——finish() 壳与 ~TeammateRuntime 兜底共用本核；返回 true=
    // 本次调用完成清算，false=已清算过（幂等空转）；outCleanupError 仅在本次真正
    // 执行且退租失败时带回错误文本（供 emit 壳上报，析构路径只 qWarning）。
    bool settleLedgers(QString *outCleanupError);
    // lcc run() finally :1044-1057 清算序的 emit 壳：settleLedgers + 失败上报
    // （sendToLead error 信，lcc :1049-1055 逐字形态）+ emit finished。
    void finish();

    bool trayHasContent() const { return !m_pendingTray.isEmpty(); }

    // 对话记录装配（lcc messages 列表的 lite 形：QJsonObject role/content[/tool_calls]）
    void appendUser(const QString &content);
    void appendAssistant(const QString &text, const QJsonArray &toolCalls);
    // OpenAI 形工具产物回填（lite 消息形态，P3 宿主消费 messages() 时按此形状装配请求；
    // lcc :971-974 的 Anthropic tool_result 块是同一事实的另一种信封——有意偏离登记）。
    void appendToolResult(const QString &callId, const QString &output);
    void appendTrayNotice(const QString &text); // 托盘条目（收进 m_pendingTray）
    void flushTray();                            // 托盘合并为单条 user 消息（lcc :922-927）

    // 发信给 Lead + 展示信号双通道；bus 失败按 MessageBus 折叠纪律 qDebug（尽力而为，
    // lcc 裸 send 异常会走 run() 外层 except → error 消息——lite 折叠为静默记账）
    void sendToLead(const QString &content, const QString &type,
                    const QJsonObject &metadata = QJsonObject());

    QString m_name;
    QString m_role;
    QString m_prompt;
    QString m_taskId;      // 出生带单（可空）
    bool m_requirePlan = false;

    AgentTeamsManager *m_manager = nullptr; // 非所有权（宿主保证存活）
    MessageBus *m_bus = nullptr;            // 非所有权
    TaskStore *m_taskStore = nullptr;       // 非所有权

    QVector<QJsonObject> m_messages; // 对话记录（lcc self.messages）
    QStringList m_pendingTray;       // 本拍待合并的用户消息文本（lcc work_messages，:903）
    QTimer *m_heartbeat = nullptr;   // Idle 轮询心跳（零线程，钉死第 1 条）
    bool m_finished = false;         // 终态闩锁（finish 幂等；再调回调全静默）
};

#endif // TEAMMATERUNTIME_H
