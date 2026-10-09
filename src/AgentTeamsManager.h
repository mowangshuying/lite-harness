#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

#include "MessageBus.h" // BusMessage 自由结构体（值参/引用出现在本头签名里，须完整类型）
#include "TaskStore.h"

class TeammateRuntime;

/**
 * AgentTeamsManager —— s13 Agent Teams 台账与协议内核（QtCore-only）
 * （lcc s13 34775c8 agent_teams_manager.py 移植，:55-684/:690-796）
 *
 * 职责：五本内存台账（activeTeammates / planGates / planRequestIds /
 * pendingRequests / teammateHandles）+ Lead 七工具薄壳 + 队友协议门
 * （submit_plan / apply_plan_response / apply_shutdown_request / 工具总闸
 * runTeammateTool）+ Lead 信箱核销（consumeLeadInbox）+ 事件渲染（formatTeamEvents）。
 * 队友拉活（claimNextTask，lcc claim_next_task :607-624）也归本类——依赖
 * TaskStore::scanUnclaimedTasks 与 'Claimed ' 承重前缀。
 *
 * ══ 与 lcc 的偏差登记（有意偏离，逐条理由）══
 * D9 同族·锁塌缩：lcc 的 teamLock/task_store_lock（:96-131 fcntl/msvcrt、:320）
 *   在 lite 单宿主进程主线程事件驱动模型下全部塌缩为普通调用——零线程原则，
 *   不存在并发访问者。「bus.send 恒在锁外」的时序纪律天然满足（无锁可言）。
 * 偏A teammateThreads → teammateHandles：lcc 第五本账持 threading.Thread；
 *   lite 无线程，改存 TeammateRuntime* 不透明句柄（本头仅前向声明，.cpp 从不
 *   dereference 它，也不 include TeammateRuntime.h——保证本 TU 纯 QtCore 可进
 *   测试目标；P1 先例：TaskStore.cpp 同法）。真正的启动经注入的 launcher 完成，
 *   launcher 契约见 setTeammateLauncher 注释（登记先于启动 = lcc :677 语义）。
 * 偏B spawn 出生版本：lcc :666 assignment_versions[name]=0 显式写零；
 *   lite TaskStore 无版本写入口（P1 设计），assignmentVersion(未在册 owner) 缺省
 *   即 0——出生语义天然成立，跳过写入。
 * 偏C _run_teammate_tool 与 runtime handlers 字典合并：lcc 门在 manager
 *   （:495-531）、执行器在 runtime（:841-853 闭包字典）；lite 收拢为本类单点
 *   runTeammateTool（门+派发+适配器查表），TeammateRuntime 只负责回合编排。
 *   基工具 cwd 解析（lcc runtime._run_base/current_cwd :857-874）随派发进本类，
 *   文案源已移入 TaskStore::assignmentCwd 三分支（gate① M2 裁决），折叠为
 *   "Error: Invalid task assignment: <err>"。
 * 偏C' PostToolUse 返回值：lcc :529-530 调用后忽略；lite 同款忽略（与
 *   AgentLoop::execute_tool 的拦截惯例不同，此处以 lcc s13 字节语义为准）。
 * 偏D claim_next_task 的 _owner_in_progress 私有查询：lcc :607 用私有方法；
 *   lite 经 listTaskSnapshots 结构化扫描等价实现。
 * 偏E send 失败折叠：lcc 邮箱写失败 raise → 调用栈炸穿；lite 按本仓「失败折叠
 *   为工具输出」纪律返回 "Error: " + bus->lastError() 并回滚已登记的案卷/台账。
 * 偏F run_list_tasks 转发 TaskStore::runListTasks：lcc :780-796 的 [ ]/[>]/[x]
 *   文案与 lite s10 移植同源同款，不重抄第二份。
 * 偏G formatTeamEvents 空批返空串：lcc :450 对空批仍产 "[Team events]\n"（空标题块）；
 *   lite 早退 QString()——零事件零注入块（缓存友好，组11 钉为期望）。行为优于 lcc。
 *   Gate② FIND-F 补录（原为码内注释，未入本清单）。
 * 偏H 队友基工具无租约文案：lcc current_cwd :858-859 教学形 "Error: Claim a Task
 *   before using workspace tools."；lite 随偏C 折叠为 "Error: Invalid task assignment:
 *   <assignmentCwd 三分支文案>"（组9 钉 lite 文本）。均 fail-closed，仅措辞差。
 *   Gate② FIND-K 补录（裁决=接受 lite 形态）。
 *
 * 回调接线（lcc :329-331 构造时挂 task_manager 三回调）在本类构造函数完成：
 * - onAssignmentAdvanced → 换工复位 plan gate（required）+ 清在审案号
 *   （lcc _on_assignment_advanced :356-363）；fix-4 钉死清单第 1 条：plan gate
 *   复位只挂释放/换工回调，别处不翻。
 * - onAssignmentReleased → gate 置 not_required（lcc :365-368）。
 * - planGateCheck → 完成前置否决（lcc _plan_gate_check :370-379，文本逐字：
 *   "Cannot complete while plan status is {required|pending|rejected}"）。
 *
 * 保留名：lead/agent 拒 spawn（AgentConst::isReservedTeammateName，casefold 单源）；
 * 名字合法性用 AgentPathGuard::isValidAgentName（与邮箱名同源，防「可 spawn 却收
 * 不到信」）。'agent' 双重语义注意：TaskStore 侧它是 Lead 的租约 owner 保留键
 * （P1 铁律），团队侧它是不可 spawn 的保留名——两侧同源不同用。
 *
 * 五本账全内存、重启即作废 fail-closed（D7）：禁止从磁盘恢复队友或案卷。
 */
class AgentTeamsManager : public QObject
{
    Q_OBJECT
public:
    // lcc activeTeammates 值域（:305 注释；字符串单源见 statusName）
    enum class TeammateStatus { Working, WaitingApproval, Idle, Stopping };
    // lcc planGates 值域（FSM：not_required|required|pending|approved|rejected）
    enum class PlanGate { NotRequired, Required, Pending, Approved, Rejected };

    // 协议案卷（lcc ProtocolState dataclass :76-85）。None 的单态口径：
    // workVersion=-1 ≡ lcc None（shutdown 案卷不带版本快照，永不与在册 int 相等）；
    // taskId 空串 ≡ None（无租约认领）。status 用字符串而非枚举：核销/复读文案
    // 直接嵌原文（"Request %1 already %2"），与 lcc 逐字一致。
    struct ProtocolState
    {
        QString requestId;
        QString type;      // plan_approval | shutdown
        QString sender;
        QString target;
        QString status;    // pending | approved | rejected
        QString payload;
        int workVersion = -1;
        QString taskId;    // 空串 ≡ lcc None
        double createdAt = 0.0; // D3 同口径浮点秒
    };

    // 会话保留键（TaskStore 侧 Lead owner 键，同名单源见 P1 头注）
    static constexpr auto kLeadOwnerKey = "agent";

    explicit AgentTeamsManager(MessageBus *bus, TaskStore *taskStore,
                               QObject *parent = nullptr);

    // 值域字符串单源（协议文本与台账展示共用；lcc 裸串逐字）
    static QString statusName(TeammateStatus status);
    static QString gateName(PlanGate gate);

    // ── Lead 七工具内核形态（lcc run_* :690-776；P3 只做 JSON args 解包转发）──
    QString runSpawnTeammate(const QString &name, const QString &role, const QString &prompt,
                             const QString &taskId, bool requirePlan); // lcc spawn_teammate :628-684
    QString runListTeammates();                                        // lcc :696-704
    QString runSendMessage(const QString &to, const QString &content); // lcc :706-712
    QString runRequestShutdown(const QString &teammate);               // lcc :714-731
    QString runRequestPlan(const QString &teammate, const QString &task); // lcc :733-741
    QString runReviewPlan(const QString &requestId, bool approve,
                         const QString &feedback);                     // lcc :743-771
    QString runCreateWorktree(const QString &name, const QString &taskId); // lcc :773-776
    // 队友借用的 list_tasks（lcc :780-796）：直接转发 TaskStore 既有移植（偏F）
    QString runListTasks() const;

    // ── 协议门（TeammateRuntime / P3 宿主直连本公共面）──
    // 队友提交计划（lcc _teammate_submit_plan :462-493）：一人一宗在审案；
    // 案卷带 (workVersion,taskId) 快照，换工后旧批复在 applyPlanResponse 对账自动作废。
    QString submitPlan(const QString &name, const QString &plan);
    // Lead 批复核销（lcc apply_plan_response :533-568，11 条件合取）。
    // 返回值=out：true=采纳，out="[Plan approved|rejected] <content>"（gate 只在这里
    // 由队友侧翻转——Lead 侧 runReviewPlan 从不直改 planGates，fix-4 钉死第 7 条）；
    // false=out="[Ignored plan response: request mismatch]"。
    bool applyPlanResponse(const QString &name, const BusMessage &msg, QString *out);
    // 关机请求核销（lcc apply_shutdown_request :570-594，8 条件）。
    // true=out=request_id（队友须回执）；false=out="[Ignored shutdown request: request mismatch]"
    bool applyShutdownRequest(const QString &name, const BusMessage &msg, QString *out);
    // 队友互信通信（lcc _teammate_send_message :596-603）：to=lead 恒可，其余须在册
    QString sendTeammateMessage(const QString &from, const QString &to, const QString &content);
    // 工具总闸（lcc _run_teammate_tool :495-531 + 派发合并，偏C）：
    // plan gate 只拦 bash/write_file/edit_file；read/glob 永远放行；
    // 写三件另过 permissionCheck（lcc :514-517 无论 approved 与否都查）。
    QString runTeammateTool(const QString &name, const QString &toolName, const QJsonObject &params);
    // 案号生成（lcc new_request_id :383-389）："req_" + 6 位零填充随机数，避撞重取
    QString genRequestId();
    // 当前工作身份（lcc current_work_identity :452-458）：(版本, 租约任务) 快照对
    void currentWorkIdentity(const QString &owner, int *version, QString *taskId) const;
    // Lead 信箱破坏性整读 + 协议回执核销（lcc consume_lead_inbox :424-439）。
    // 返回原始批供上层 formatTeamEvents 渲染（回执不吞，docs 谬称已被 gate 报告否决）。
    QVector<BusMessage> consumeLeadInbox();
    // 事件渲染（lcc format_team_events :441-450）："[Team events]\n[type request_id=..] from: content"
    static QString formatTeamEvents(const QVector<BusMessage> &messages);
    // 队友拉活（lcc claim_next_task :607-624）：持租约/有遗留 in_progress 一律拒；
    // 否则按 scanUnclaimedTasks 顺序逐个 claim，命中 'Claimed ' 前缀即返快照。
    std::optional<TaskStore::TaskSnapshot> claimNextTask(const QString &owner);

    // ── 台账查询与状态机入口（TeammateRuntime 直连）──
    bool hasTeammate(const QString &name) const;                     // activeTeammates 在册
    std::optional<TeammateStatus> teammateStatus(const QString &name) const;
    void setTeammateStatus(const QString &name, TeammateStatus status);
    PlanGate planGate(const QString &name) const;                    // 缺省 NotRequired
    QString currentPlanRequestId(const QString &name) const;         // 不在册返回空串
    const ProtocolState *protocolState(const QString &requestId) const; // 只读指针，无案卷=nullptr
    // 队友退场清算的第②段（lcc run() finally :1048-1056 的 pop 四本账；
    // 第①段 releaseTeammateAssignment 由 TeammateRuntime 自调——fix-4 钉死第 4 条）
    void finalizeTeammate(const QString &name);
    // 在册队友名（字典序，lcc sorted() 口径）
    QStringList teammateNames() const;

    // ── Gate② FIND-M 诊断面（P3b 宿主遥测消费，只读）──
    // lcc 的 release 失败=fail-stop（异常炸穿线程即死）；lite fail-continue 以本通道
    // 补偿记账：回合边界（TeammateRuntime::deliverTurnResult）、死亡清算（finish）、
    // spawn 回滚（runSpawnTeammate FIND-E 路径）三处 release*Assignment 失败统一
    // noteReleaseWarning，lastReleaseWarning 回读末次错误文本。
    // **粘滞不清**：lcc 语义里失败后不存在「同点位又成功」的时机（线程已死/即将回滚），
    // 成功路径清空反而会让宿主遥测看不见间歇性故障——消费方判 isEmpty 只作弱信号，
    // 要精确须自记序号（本面刻意只做最低成本诊断，不做事件流）。
    void noteReleaseWarning(const QString &text);
    QString lastReleaseWarning() const;

    // ── P3 注入点（默认空 = 相应路径 fail-closed 或跳过）──
    // 返回新建的 TeammateRuntime 句柄（未拥有指针，所有权归调用栈上的宿主；
    // 本类只登记不 dereference——偏A）。返回 nullptr = 创建失败：Gate② FIND-E 起
    // spawn 侧 fail-closed——退租+弹四本账+折叠报错（对齐 lcc :676 thread.start()
    // 失败 raise 不留幽灵），成功文案不再可达。launcher 自身创建失败时不得留下半
    // 成品 runtime（P3 fix-5 契约：要么返有效句柄，要么纯 nullptr、资源自清）。
    using TeammateLauncher =
        std::function<TeammateRuntime *(const QString &name, const QString &role,
                                        const QString &prompt, const QString &taskId,
                                        bool requirePlan)>;
    // 契约：launcher 须「先建后启且启动延迟到当前调用栈返回后」（QTimer::singleShot(0)
    // 或等价），使 runSpawnTeammate 在 launcher 返回后立即登记句柄、先于 runtime
    // 任何代码运行——对齐 lcc :677 在 start() 前注册 thread 的
    // 「秒死线程 finally 也能找到自己」语义。
    void setTeammateLauncher(TeammateLauncher launcher);
    void setWorktreeCreator(std::function<QString(const QString &name, const QString &taskId)> creator);
    void setPermissionCheck(
        std::function<QString(const QString &toolName, const QJsonObject &params)> check);
    // 钩子（lcc hooks_trigger）：PreToolUse 非空返回=拦截（lcc s13 已采拦截惯例）；
    // PostToolUse 返回值忽略（偏C'）。output 仅 PostToolUse 时有值。
    void setHooksTrigger(std::function<QString(const QString &eventName, const QString &toolName,
                                               const QJsonObject &params, const QString &output)> hooks);
    // 基五件工具适配器（lcc tool_adapters；bash/read_file/write_file/edit_file/glob）。
    // 以 ToolNames:: 常量作键注入；未注入的基工具按 lcc 口径报 Unknown tool。
    void setToolAdapter(const QString &toolName,
                        std::function<QString(const QJsonObject &params, const QString &cwd)> adapter);

private:
    // lcc match_response :391-422 四门软核销（①案卷存在 ②回执类型按案卷推导
    // ③镜像身份 ④一次性防重放）；全过才把案卷置 approved/rejected——它是
    // status 的唯一写者，从不触碰 gates/teammates（裁决/执行分离）。
    bool matchResponse(const QString &msgType, const QString &requestId, bool approve,
                       const QString &msgFrom, const QString &msgTo);
    // lcc _teammate_threads 的 lite 对应物：不透明句柄，本类只存不碰（偏A）
    bool isWriteTool(const QString &toolName);

    MessageBus *m_bus = nullptr;
    TaskStore *m_taskStore = nullptr;
    TeammateLauncher m_launcher;
    std::function<QString(const QString &, const QString &)> m_worktreeCreator;
    std::function<QString(const QString &, const QJsonObject &)> m_permissionCheck;
    std::function<QString(const QString &, const QString &, const QJsonObject &, const QString &)>
        m_hooksTrigger;
    QHash<QString, std::function<QString(const QJsonObject &, const QString &)>> m_toolAdapters;

    // 五本账（全内存，D7：重启即作废，禁从磁盘恢复）
    QHash<QString, TeammateStatus> m_activeTeammates;                 // name → status
    QHash<QString, PlanGate> m_planGates;                              // name → gate
    QHash<QString, QString> m_planRequestIds;                          // name → 在审案号
    QHash<QString, ProtocolState> m_pendingRequests;                   // request_id → 案卷
    QHash<QString, TeammateRuntime *> m_teammateHandles;               // name → 不透明句柄（偏A）

    // Gate② FIND-M 诊断面存储：末次 release*Assignment 失败的错误文本（粘滞，见
    // lastReleaseWarning 注释）。
    QString m_lastReleaseWarning;
};

// 空闲心跳扫描间隔常量已迁 AgentConstants.h::AgentConst::kTeamIdleScanIntervalMs
// （Gate② FIND-N3，本仓魔法数字单源约定；消费方 TeammateRuntime 心跳 QTimer）。
