#include "AgentTeamsManager.h"

#include "AgentConstants.h"
#include "AgentPathGuard.h"
#include "MessageBus.h"
#include "TaskStore.h"
#include "ToolNames.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStringList>

// lcc s13 34775c8 agent_teams_manager.py 移植（本 TU 覆盖 :297-796 + :391-458 协议面）。
// 铁律：不 include TeammateRuntime.h——句柄只存不碰（偏A），保证本 TU 纯 QtCore 可进测试目标。
// D9 锁塌缩：lcc 的 task_store_lock/team_lock（:96-131）在单宿主进程主线程事件驱动下
//   无并发访问者，全部塌缩为普通调用；「bus.send 恒在锁外」的时序纪律天然满足。

namespace {

// lcc 裸串口径（协议键与信封类型是禁翻区，C 类串，永不 tr()）
const QString kLeadName = QStringLiteral("lead");       // lcc 固定收信人/发信人名
const QString kTypeMessage = QStringLiteral("message");
const QString kTypeResult = QStringLiteral("result");
const QString kTypeIdleNotification = QStringLiteral("idle_notification");
const QString kTypePlanRequest = QStringLiteral("plan_request");
const QString kTypePlanApprovalRequest = QStringLiteral("plan_approval_request");
const QString kTypePlanApprovalResponse = QStringLiteral("plan_approval_response");
const QString kTypeShutdownRequest = QStringLiteral("shutdown_request");
const QString kTypeShutdownResponse = QStringLiteral("shutdown_response");
const QString kStatusPending = QStringLiteral("pending");
const QString kStatusApproved = QStringLiteral("approved");
const QString kStatusRejected = QStringLiteral("rejected");
const QString kKeyRequestId = QStringLiteral("request_id");
const QString kKeyApprove = QStringLiteral("approve");

// 队友工具的裸名（s13 的 8 个新工具名入 ToolNames.h 属 P3/fix-5 写域，此处先行局部单源）
const QString kToolSendMessage = QStringLiteral("send_message");
const QString kToolSubmitPlan = QStringLiteral("submit_plan");
const QString kToolListTasks = QStringLiteral("list_tasks");
const QString kToolClaimTask = QStringLiteral("claim_task");
const QString kToolCompleteTask = QStringLiteral("complete_task");

double nowSecs()
{
    return static_cast<double>(QDateTime::currentDateTime().toMSecsSinceEpoch()) / 1000.0;
}

} // namespace

AgentTeamsManager::AgentTeamsManager(MessageBus *bus, TaskStore *taskStore, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_taskStore(taskStore)
{
    // lcc __init__ :329-331：构造时挂 task_manager 三回调（锁塌缩见 D9 头注）
    Q_ASSERT(m_bus && m_taskStore);

    // lcc _on_assignment_advanced :356-363：换工 → 门复位 required + 清在审案号。
    // fix-4 钉死清单第 1 条：门复位只挂这里与释放回调，别处不翻。
    // Gate② FIND-D 对齐：lcc :363 的 planRequestIds.pop 在 if **外**——换工即路由单
    // 无条件作废；旧 lite 把 remove 圈在门发牌 if 内，留了「released(门 NotRequired、
    // 案号按 lcc parity 残留)→再 claim」窗口的陈旧案号（协议路径已证收敛，但
    // currentPlanRequestId 面会读到 lcc 不存在的值，P3 UI 消费即踩）。组10(g) 钉桩。
    m_taskStore->setOnAssignmentAdvanced([this](const QString &owner, const QString &) {
        auto it = m_planGates.find(owner);
        if (it != m_planGates.end() && it.value() != PlanGate::NotRequired) {
            it.value() = PlanGate::Required; // lcc :360-362：只对已发牌的门复位
        }
        m_planRequestIds.remove(owner); // lcc :363：无条件 pop（owner 不在也等价 no-op）
    });

    // lcc _on_assignment_released :365-368：释放 → gate='not_required'。
    // lcc 用 dict 赋值（不在册则创建）——insert 同语义。work_version 陈旧推进
    // 不在此做（lite 释放不 bump 版本=TaskStore 偏差④），P4 若需覆盖「释放」
    // 应在本回调自推本地计数（gate 报告 fix-4 第 1 条）。
    m_taskStore->setOnAssignmentReleased([this](const QString &owner) {
        m_planGates.insert(owner, PlanGate::NotRequired);
    });

    // lcc _plan_gate_check :370-379：completeTask 前置否决（文本逐字，不含 task id）。
    // 缺省（不在册/未开团队）NotRequired → 放行——Lead 用 owner="agent" 不受影响。
    m_taskStore->setPlanGateCheck([this](const QString &owner, const QString &, QString *reason) {
        const PlanGate gate = planGate(owner);
        if (gate == PlanGate::Required || gate == PlanGate::Pending || gate == PlanGate::Rejected) {
            if (reason) {
                *reason = QStringLiteral("Cannot complete while plan status is ") + gateName(gate);
            }
            return false;
        }
        return true;
    });
}

QString AgentTeamsManager::statusName(TeammateStatus status)
{
    switch (status) {
    case TeammateStatus::Working: return QStringLiteral("working");
    case TeammateStatus::WaitingApproval: return QStringLiteral("waiting_approval");
    case TeammateStatus::Idle: return QStringLiteral("idle");
    case TeammateStatus::Stopping: return QStringLiteral("stopping");
    }
    return QStringLiteral("working"); // 不可达（枚举全覆盖），消 /W4 C4715
}

QString AgentTeamsManager::gateName(PlanGate gate)
{
    switch (gate) {
    case PlanGate::NotRequired: return QStringLiteral("not_required");
    case PlanGate::Required: return QStringLiteral("required");
    case PlanGate::Pending: return QStringLiteral("pending");
    case PlanGate::Approved: return QStringLiteral("approved");
    case PlanGate::Rejected: return QStringLiteral("rejected");
    }
    return QStringLiteral("not_required"); // 不可达，同上
}

QString AgentTeamsManager::genRequestId()
{
    // lcc new_request_id :383-389：req_ + 6 位零填充随机数，避撞重取
    QString id;
    do {
        id = QStringLiteral("req_%1")
                 .arg(QRandomGenerator::global()->bounded(1000000), 6, 10, QLatin1Char('0'));
    } while (m_pendingRequests.contains(id));
    return id;
}

void AgentTeamsManager::currentWorkIdentity(const QString &owner, int *version, QString *taskId) const
{
    // lcc current_work_identity :452-458：(assignment_versions.get(owner,0), 租约 task_id or None)
    if (version) {
        *version = m_taskStore->assignmentVersion(owner);
    }
    if (taskId) {
        const auto lease = m_taskStore->leaseFor(owner);
        *taskId = lease ? lease->taskId : QString();
    }
}

bool AgentTeamsManager::hasTeammate(const QString &name) const
{
    return m_activeTeammates.contains(name);
}

std::optional<AgentTeamsManager::TeammateStatus> AgentTeamsManager::teammateStatus(const QString &name) const
{
    const auto it = m_activeTeammates.constFind(name);
    if (it == m_activeTeammates.cend()) {
        return std::nullopt;
    }
    return it.value();
}

void AgentTeamsManager::setTeammateStatus(const QString &name, TeammateStatus status)
{
    // lcc 直接 dict 赋值（不在册则创建）；insert 同语义
    m_activeTeammates.insert(name, status);
}

AgentTeamsManager::PlanGate AgentTeamsManager::planGate(const QString &name) const
{
    // lcc plan_gates.get(name, "not_required")
    const auto it = m_planGates.constFind(name);
    return it == m_planGates.cend() ? PlanGate::NotRequired : it.value();
}

QString AgentTeamsManager::currentPlanRequestId(const QString &name) const
{
    return m_planRequestIds.value(name);
}

const AgentTeamsManager::ProtocolState *AgentTeamsManager::protocolState(const QString &requestId) const
{
    const auto it = m_pendingRequests.constFind(requestId);
    return it == m_pendingRequests.cend() ? nullptr : &it.value();
}

QStringList AgentTeamsManager::teammateNames() const
{
    QStringList names = m_activeTeammates.keys();
    names.sort(); // lcc sorted(items)：按名字典序（大小写敏感，与 Python str 序一致）
    return names;
}

void AgentTeamsManager::noteReleaseWarning(const QString &text)
{
    // Gate② FIND-M 诊断面写入点（三处 release 失败统一经此：deliverTurnResult 回合
    // 边界 / finish 死亡清算 / runSpawnTeammate FIND-E 回滚）。lcc 无此概念——它的
    // release 失败是 fail-stop（异常终结线程），lite 的 fail-continue 必须留痕。
    // 粘滞语义见头文件声明注释（成功路径不清，防掩盖间歇故障）。
    m_lastReleaseWarning = text;
}

QString AgentTeamsManager::lastReleaseWarning() const
{
    return m_lastReleaseWarning;
}

bool AgentTeamsManager::matchResponse(const QString &msgType, const QString &requestId, bool approve,
                                      const QString &msgFrom, const QString &msgTo)
{
    // lcc match_response :391-422 四门软核销，永不抛错：
    // ① 案卷必须存在（拒幻觉案号）——空 rid 在哈希表必miss，同口径
    const auto it = m_pendingRequests.find(requestId);
    if (it == m_pendingRequests.end()) {
        return false;
    }
    // ② 期望回执类型按案卷类型推导（来信不得自证类型——防伪装信封）
    QString expected;
    if (it->type == QStringLiteral("shutdown")) {
        expected = kTypeShutdownResponse;
    } else if (it->type == QStringLiteral("plan_approval")) {
        expected = kTypePlanApprovalResponse;
    } else {
        return false;
    }
    if (msgType != expected) {
        return false;
    }
    // ③ 镜像身份：回执必须发回案卷的 sender（plan: 队友 / shutdown: lead）
    if (msgFrom != it->target || msgTo != it->sender) {
        return false;
    }
    // ④ 一次性防重放：仅 pending 案卷可被核销
    if (it->status != kStatusPending) {
        return false;
    }
    // 本函数是 status 的唯一写者；从不触碰 gates/teammates（裁决/执行分离，lcc :391 注释）
    it->status = approve ? kStatusApproved : kStatusRejected;
    return true;
}

QVector<BusMessage> AgentTeamsManager::consumeLeadInbox()
{
    // lcc consume_lead_inbox :424-439：破坏性整读 + 协议回执核销，原始批交上层渲染
    const QVector<BusMessage> drained = m_bus->drain(kLeadName);
    QVector<BusMessage> msgs = drained;
    for (const BusMessage &msg : msgs) {
        const QString rid = msg.metadata.value(kKeyRequestId).toString();
        if (!rid.isEmpty() && msg.type.endsWith(QStringLiteral("_response"))) {
            matchResponse(msg.type, rid, msg.metadata.value(kKeyApprove).toBool(),
                          msg.from, msg.to);
        }
    }
    return msgs;
    // 有意偏离登记：lcc 文档宣称「回执被吞」不实（gate 报告已否决）——本函数返回
    // 原始批，协议回执照常进 [Team events] 渲染。
}

QString AgentTeamsManager::formatTeamEvents(const QVector<BusMessage> &messages)
{
    // lcc format_team_events :441-450
    if (messages.isEmpty()) {
        return QString(); // 零事件零注入块（缓存友好；消费端判 isEmpty）
    }
    QStringList lines;
    lines.reserve(messages.size());
    for (const BusMessage &msg : messages) {
        const QString rid = msg.metadata.value(kKeyRequestId).toString();
        const QString suffix = rid.isEmpty() ? QString() : QStringLiteral(" request_id=%1").arg(rid);
        lines << QStringLiteral("[%1%2] %3: %4").arg(msg.type, suffix, msg.from, msg.content);
    }
    return QStringLiteral("[Team events]\n") + lines.join(QLatin1Char('\n'));
}

QString AgentTeamsManager::submitPlan(const QString &name, const QString &plan)
{
    // lcc _teammate_submit_plan :462-493：一人一宗在审案（rejected 后允许重提）
    if (planGate(name) == PlanGate::Pending) {
        return QStringLiteral("A plan is already waiting for review.");
    }
    const PlanGate prevGate = planGate(name);
    const QString prevRequestId = currentPlanRequestId(name);
    const auto prevStatus = teammateStatus(name);

    const QString rid = genRequestId();
    int curVersion = -1;
    QString curTaskId;
    currentWorkIdentity(name, &curVersion, &curTaskId);

    ProtocolState st;
    st.requestId = rid;
    st.type = QStringLiteral("plan_approval");
    st.sender = name;
    st.target = kLeadName;
    st.status = kStatusPending;
    st.payload = plan;
    st.workVersion = curVersion; // 快照「这一版工作这一宗事」——换工后旧批复自动作废
    st.taskId = curTaskId;
    st.createdAt = nowSecs();

    m_pendingRequests.insert(rid, st);
    m_planGates.insert(name, PlanGate::Pending);
    m_planRequestIds.insert(name, rid);
    setTeammateStatus(name, TeammateStatus::WaitingApproval);

    QJsonObject meta;
    meta.insert(kKeyRequestId, rid);
    if (!m_bus->send(name, kLeadName, plan, kTypePlanApprovalRequest, meta)) {
        // 偏E：send 失败折叠为 Error 文本并回滚本次台账写入（lcc raise 炸穿等价重来）
        m_pendingRequests.remove(rid);
        m_planGates.insert(name, prevGate);
        if (prevRequestId.isEmpty()) {
            m_planRequestIds.remove(name);
        } else {
            m_planRequestIds.insert(name, prevRequestId);
        }
        if (prevStatus) {
            setTeammateStatus(name, *prevStatus);
        }
        return QStringLiteral("Error: ") + m_bus->lastError();
    }
    return QStringLiteral("Plan submitted (%1). Wait for Lead's decision.").arg(rid);
}

bool AgentTeamsManager::applyPlanResponse(const QString &name, const BusMessage &msg, QString *out)
{
    // lcc apply_plan_response :533-568：11 条件合取，任一不满足即整单忽略。
    // 去重纵深（fix-4 钉死第 2 条）：案号对账 + 成功即摘案号 + 案卷 status 终态
    // 非 pending 二次命中必挂——同案重放必 False。
    const QString rid = msg.metadata.value(kKeyRequestId).toString();
    int curVersion = -1;
    QString curTaskId;
    currentWorkIdentity(name, &curVersion, &curTaskId);

    const ProtocolState *st = rid.isEmpty() ? nullptr : protocolState(rid);
    const bool approveFlag = msg.metadata.value(kKeyApprove).toBool();

    const bool valid = !rid.isEmpty() && msg.from == kLeadName && msg.to == name &&
                       currentPlanRequestId(name) == rid && st != nullptr &&
                       st->type == QStringLiteral("plan_approval") && st->sender == name &&
                       st->target == kLeadName && st->workVersion == curVersion &&
                       st->taskId == curTaskId &&
                       (st->status == kStatusApproved || st->status == kStatusRejected) &&
                       approveFlag == (st->status == kStatusApproved);

    if (!valid) {
        if (out) {
            *out = QStringLiteral("[Ignored plan response: request mismatch]");
        }
        return false;
    }

    // 门翻转的唯一发生地（fix-4 钉死第 7 条）：读案卷终态，不读信件——台账权威。
    m_planGates.insert(name, st->status == kStatusApproved ? PlanGate::Approved : PlanGate::Rejected);
    m_activeTeammates.insert(name, TeammateStatus::Working);
    m_planRequestIds.remove(name);
    if (out) {
        *out = QStringLiteral("[Plan %1] %2").arg(st->status, msg.content);
    }
    return true;
}

bool AgentTeamsManager::applyShutdownRequest(const QString &name, const BusMessage &msg, QString *out)
{
    // lcc apply_shutdown_request :570-594：8 条件合取。
    // 去重纵深：案卷 status=='pending' 一次性——已裁决案卷不再命中。
    const QString rid = msg.metadata.value(kKeyRequestId).toString();
    const ProtocolState *st = rid.isEmpty() ? nullptr : protocolState(rid);
    const auto status = teammateStatus(name);

    const bool valid = !rid.isEmpty() && msg.from == kLeadName && msg.to == name && st != nullptr &&
                       st->type == QStringLiteral("shutdown") && st->sender == kLeadName &&
                       st->target == name && st->status == kStatusPending &&
                       !(status && *status == TeammateStatus::Stopping);

    if (!valid) {
        if (out) {
            *out = QStringLiteral("[Ignored shutdown request: request mismatch]");
        }
        return false;
    }
    setTeammateStatus(name, TeammateStatus::Stopping);
    if (out) {
        *out = rid; // 队友据此回执 shutdown_response
    }
    return true;
}

QString AgentTeamsManager::sendTeammateMessage(const QString &from, const QString &to,
                                               const QString &content)
{
    // lcc _teammate_send_message :596-603：to=lead 恒可，其余须在册
    if (to != kLeadName && !m_activeTeammates.contains(to)) {
        return QStringLiteral("Agent '%1' is not active").arg(to);
    }
    if (!m_bus->send(from, to, content, kTypeMessage)) {
        return QStringLiteral("Error: ") + m_bus->lastError(); // 偏E
    }
    return QStringLiteral("Sent to %1").arg(to);
}

bool AgentTeamsManager::isWriteTool(const QString &toolName)
{
    // lcc :500：{_BASH, WRITE_FILE, EDIT_FILE}——read/glob 永远放行
    return toolName == ToolNames::BASH || toolName == ToolNames::WRITE_FILE ||
           toolName == ToolNames::EDIT_FILE;
}

QString AgentTeamsManager::runTeammateTool(const QString &name, const QString &toolName,
                                           const QJsonObject &params)
{
    // lcc _run_teammate_tool :495-531 + runtime handlers 字典合并（偏C）
    const PlanGate gate = planGate(name);
    if (isWriteTool(toolName)) {
        if (gate != PlanGate::Approved) {
            if (gate != PlanGate::NotRequired) {
                return QStringLiteral("Blocked: plan status is %1. Submit or revise the plan "
                                      "and wait for approval before changing the workspace.")
                    .arg(gateName(gate));
            }
        }
        // lcc :514-517：写三件无论门状态如何都过权限门（prompt_user=False 形态——
        // ASK 自动拒绝是 P3 permissionCheck 注入实现的 lcc 对齐口径，本层只见返回值）
        if (m_permissionCheck) {
            const QString blocked = m_permissionCheck(toolName, params);
            if (!blocked.isEmpty()) {
                return blocked;
            }
        }
    }

    // 派发：基五件走适配器表（cwd 一律 assignmentCwd 现读，禁读租约缓存——fix-4 钉死第 3 条），
    // 团队工具直连本类公共门。lcc :518-520：未知工具先返，钩子不触发。
    std::function<QString()> invoke;
    if (m_toolAdapters.contains(toolName)) {
        const auto adapter = m_toolAdapters.value(toolName);
        invoke = [this, name, adapter, params]() {
            QString cwd;
            QString err;
            if (!m_taskStore->assignmentCwd(name, &cwd, &err)) {
                // 偏C：lcc current_cwd 两文案源已移入 TaskStore::assignmentCwd 三分支
                return QStringLiteral("Error: Invalid task assignment: ") + err;
            }
            return adapter(params, cwd);
        };
    } else if (toolName == kToolSendMessage) {
        invoke = [this, name, params]() {
            return sendTeammateMessage(name, params.value(QStringLiteral("to")).toString(),
                                       params.value(QStringLiteral("content")).toString());
        };
    } else if (toolName == kToolSubmitPlan) {
        invoke = [this, name, params]() {
            return submitPlan(name, params.value(QStringLiteral("plan")).toString());
        };
    } else if (toolName == kToolListTasks) {
        invoke = [this]() { return runListTasks(); };
    } else if (toolName == kToolClaimTask) {
        invoke = [this, name, params]() { return m_taskStore->runClaimTaskLeased(params, name); };
    } else if (toolName == kToolCompleteTask) {
        invoke = [this, name, params]() { return m_taskStore->runCompleteTaskLeased(params, name); };
    } else {
        return QStringLiteral("Unknown tool: %1").arg(toolName);
    }

    // lcc :521-523：PreToolUse（skip_permission=True 形态）非空即拦截
    if (m_hooksTrigger) {
        const QString blocked = m_hooksTrigger(QStringLiteral("PreToolUse"), toolName, params,
                                               QString());
        if (!blocked.isEmpty()) {
            return blocked;
        }
    }
    const QString output = invoke();
    // 偏C'：PostToolUse 返回值忽略（lcc :529-530 同款，勿按 AgentLoop 拦截惯例改）
    if (m_hooksTrigger) {
        m_hooksTrigger(QStringLiteral("PostToolUse"), toolName, params, output);
    }
    return output;
}

std::optional<TaskStore::TaskSnapshot> AgentTeamsManager::claimNextTask(const QString &owner)
{
    // lcc claim_next_task :607-624：持租约或有遗留 in_progress 一律不拉新活
    if (m_taskStore->leaseFor(owner).has_value()) {
        return std::nullopt;
    }
    QVector<TaskStore::TaskSnapshot> all;
    QString err;
    if (!m_taskStore->listTaskSnapshots(&all, &err)) {
        return std::nullopt; // 台账不可读 fail-closed（D7）
    }
    for (const TaskStore::TaskSnapshot &t : all) {
        if (t.status == QStringLiteral("in_progress") && t.owner == owner) {
            return std::nullopt; // 偏D：lcc _owner_in_progress 私有查询的结构化等价
        }
    }
    QVector<TaskStore::TaskSnapshot> candidates;
    if (!m_taskStore->scanUnclaimedTasks(&candidates, &err)) {
        return std::nullopt;
    }
    for (const TaskStore::TaskSnapshot &c : candidates) {
        QJsonObject args;
        args.insert(QStringLiteral("task_id"), c.id);
        // 'Claimed ' 承重前缀对账（P1 契约）：业务拒绝文本原样跳过继续试下一宗
        const QString result = m_taskStore->runClaimTaskLeased(args, owner);
        if (result.startsWith(QStringLiteral("Claimed "))) {
            return c;
        }
    }
    return std::nullopt;
}

void AgentTeamsManager::finalizeTeammate(const QString &name)
{
    // lcc run() finally :1049-1056 的 pop 四本账（第①段退租由 runtime 自调
    // releaseTeammateAssignment——fix-4 钉死第 4 条，两调用位点分离勿混）。
    // pendingRequests 案卷不清：lcc 同样保留（在内存 D7，随进程自然消亡）。
    m_activeTeammates.remove(name);
    m_planGates.remove(name);
    m_planRequestIds.remove(name);
    m_teammateHandles.remove(name);
}

// ── Lead 七工具薄壳（lcc run_* :690-776；P3 只做 args 解包转发）──

QString AgentTeamsManager::runSpawnTeammate(const QString &name, const QString &role,
                                            const QString &prompt, const QString &taskId,
                                            bool requirePlan)
{
    // lcc spawn_teammate :628-684
    // ① 名字合法性与邮箱名同源（AgentPathGuard::isValidAgentName，防「可 spawn 却收不到信」）
    if (!AgentPathGuard::isValidAgentName(name)) {
        return QStringLiteral("Invalid teammate name: use 1-64 letters, digits, underscores, "
                              "or dashes");
    }
    // ② 保留名 lead/agent 拒 spawn（AgentConst casefold 单源）。注意 'agent' 双重语义：
    //    TaskStore 侧是 Lead 的租约 owner 保留键（P1 铁律），团队侧是不可 spawn 的名字。
    if (AgentConst::isReservedTeammateName(name)) {
        return QStringLiteral("Invalid teammate name: '%1' is reserved by the runtime").arg(name);
    }
    // ③ 大小写折叠查重（lcc :641 t.lower()==name.lower()）
    for (auto it = m_activeTeammates.cbegin(); it != m_activeTeammates.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Teammate '%1' already exists").arg(name);
        }
    }

    // 登记（lcc :646-649）；偏B：assignment_versions[name]=0 显式写零跳过——
    // lite TaskStore 无版本写入口，未在册 owner 的 assignmentVersion() 缺省即 0。
    m_activeTeammates.insert(name, TeammateStatus::Working);
    m_planGates.insert(name, requirePlan ? PlanGate::Required : PlanGate::NotRequired);

    // 初始任务认领（lcc :652-663）：失败回滚登记并返 Cannot spawn 文本
    if (!taskId.isEmpty()) {
        QJsonObject args;
        args.insert(QStringLiteral("task_id"), taskId);
        const QString claimed = m_taskStore->runClaimTaskLeased(args, name);
        if (!claimed.startsWith(QStringLiteral("Claimed "))) { // 承重前缀对账
            m_activeTeammates.remove(name);
            m_planGates.remove(name);
            return QStringLiteral("Cannot spawn teammate '%1': %2").arg(name, claimed);
        }
    }

    // 创建 + 句柄登记（lcc :665-678 建 Thread 并在 start 前入册的等价形：launcher 只构造、
    // 启动延迟到调用栈返回后——见 setTeammateLauncher 契约注释）
    if (m_launcher) {
        TeammateRuntime *handle = m_launcher(name, role, prompt, taskId, requirePlan);
        if (!handle) {
            // Gate② FIND-E fail-closed：lcc :676 thread.start() 失败直接 raise（成功
            // 文案不可达）；lite 的 launcher 契约违规（返 nullptr）不得留下「claim 已
            // 成、账已登、无 runtime 驱动」的幽灵队友——整段入职回滚：
            // ① 死亡清算同款退租（taskId 为空时是幽灵 owner 幂等 no-op，仍走=lcc
            //    finally 无条件形态；磁盘清理失败不阻断内存清理，TaskStore :1014-1020
            //    的 finally 段保证租约必出清）；
            // ② 弹四本账（finalizeTeammate，名册/门/案号/句柄——句柄未登记，弹它是
            //    防御性对账）；
            // ③ 折叠为工具输出文本交还 Lead（本仓「失败折叠」纪律，勿恢复静默成功）。
            QString rollbackError;
            if (!m_taskStore->releaseTeammateAssignment(name, &rollbackError)
                && !rollbackError.isEmpty()) {
                noteReleaseWarning(rollbackError); // FIND-M：回滚退租失败留痕遥测
                qWarning() << "[team] spawn rollback release(" << name
                           << "):" << rollbackError;
            }
            finalizeTeammate(name);
            return QStringLiteral("Error: Teammate runtime failed to start for '%1'").arg(name);
        }
        m_teammateHandles.insert(name, handle); // 偏A：只存不碰
    }

    const QString assigned =
        taskId.isEmpty() ? QStringLiteral(" without an initial Task")
                         : QStringLiteral(" for %1").arg(taskId);
    return QStringLiteral("Teammate '%1' spawned as %2%3. "
                          "End this turn; the runtime will deliver its events.")
        .arg(name, role, assigned);
}

QString AgentTeamsManager::runListTeammates()
{
    // lcc :696-704
    const QStringList names = teammateNames();
    if (names.isEmpty()) {
        return QStringLiteral("No active teammates.");
    }
    QStringList lines;
    lines.reserve(names.size());
    for (const QString &name : names) {
        const auto status = teammateStatus(name);
        lines << QStringLiteral("%1: %2").arg(name, statusName(status.value_or(TeammateStatus::Working)));
    }
    return lines.join(QLatin1Char('\n'));
}

QString AgentTeamsManager::runSendMessage(const QString &to, const QString &content)
{
    // lcc :706-712：Lead 侧不查保留名——to 必须在册（dict 大小写敏感，同款）
    if (!m_activeTeammates.contains(to)) {
        return QStringLiteral("Teammate '%1' is not active").arg(to);
    }
    if (!m_bus->send(kLeadName, to, content, kTypeMessage)) {
        return QStringLiteral("Error: ") + m_bus->lastError(); // 偏E
    }
    return QStringLiteral("Sent to %1").arg(to);
}

QString AgentTeamsManager::runRequestShutdown(const QString &teammate)
{
    // lcc :714-731：立案卷 → 发关机请求 → 返案号（回执在队友 handle_inbox 里产生）
    if (!m_activeTeammates.contains(teammate)) {
        return QStringLiteral("Teammate '%1' is not active").arg(teammate);
    }
    const QString rid = genRequestId();
    ProtocolState st;
    st.requestId = rid;
    st.type = QStringLiteral("shutdown");
    st.sender = kLeadName;
    st.target = teammate;
    st.status = kStatusPending;
    st.workVersion = -1; // lcc None：关机案卷不带工作快照
    st.createdAt = nowSecs();
    m_pendingRequests.insert(rid, st);

    QJsonObject meta;
    meta.insert(kKeyRequestId, rid);
    if (!m_bus->send(kLeadName, teammate,
                     QStringLiteral("Finish the current step and shut down."),
                     kTypeShutdownRequest, meta)) {
        m_pendingRequests.remove(rid); // 偏E：案卷回滚（lcc raise 等价未送达重来）
        return QStringLiteral("Error: ") + m_bus->lastError();
    }
    return QStringLiteral("Shutdown requested from %1 (%2)").arg(teammate, rid);
}

QString AgentTeamsManager::runRequestPlan(const QString &teammate, const QString &task)
{
    // lcc :733-741：门置 required + 发 plan_request（无 metadata）
    if (!m_activeTeammates.contains(teammate)) {
        return QStringLiteral("Teammate '%1' is not active").arg(teammate);
    }
    m_planGates.insert(teammate, PlanGate::Required);
    if (!m_bus->send(kLeadName, teammate, task, kTypePlanRequest)) {
        // 有意偏离统一：此处不回滚——lcc 门写在 send 前且 raise 后不撤销；
        // 保守的 required 门不会造成危险状态（顶多多要一次计划）。
        return QStringLiteral("Error: ") + m_bus->lastError();
    }
    return QStringLiteral("Plan requested from %1").arg(teammate);
}

QString AgentTeamsManager::runReviewPlan(const QString &requestId, bool approve,
                                         const QString &feedback)
{
    // lcc run_review_plan :743-771：Lead 侧只写案卷终态 + 发回执，
    // 绝不翻 planGates（门翻转唯一发生地=队友侧 applyPlanResponse，fix-4 钉死第 7 条）。
    auto it = m_pendingRequests.find(requestId);
    if (it == m_pendingRequests.end()) {
        return QStringLiteral("Request %1 not found").arg(requestId);
    }
    if (it->type != QStringLiteral("plan_approval")) {
        return QStringLiteral("Request %1 is not a plan").arg(requestId);
    }
    int curVersion = -1;
    QString curTaskId;
    currentWorkIdentity(it->sender, &curVersion, &curTaskId);
    if (it->status != kStatusPending) {
        return QStringLiteral("Request %1 already %2").arg(requestId, it->status);
    }
    if (it->workVersion != curVersion || it->taskId != curTaskId) {
        return QStringLiteral("Request %1 belongs to an earlier assignment").arg(requestId);
    }
    if (currentPlanRequestId(it->sender) != requestId) {
        return QStringLiteral("Request %1 is not the current plan").arg(requestId);
    }

    it->status = approve ? kStatusApproved : kStatusRejected;
    const QString content =
        feedback.isEmpty()
            ? (approve ? QStringLiteral("Plan approved.")
                       : QStringLiteral("Revise the plan and submit it again."))
            : feedback;
    QJsonObject meta;
    meta.insert(kKeyRequestId, requestId);
    meta.insert(kKeyApprove, approve);
    if (!m_bus->send(kLeadName, it->sender, content, kTypePlanApprovalResponse, meta)) {
        it->status = kStatusPending; // 偏E：案卷回滚（lcc raise 等价未裁决重来）
        return QStringLiteral("Error: ") + m_bus->lastError();
    }
    return QStringLiteral("Plan %1 (%2)").arg(it->status, requestId);
}

QString AgentTeamsManager::runCreateWorktree(const QString &name, const QString &taskId)
{
    // lcc :773-776 转发 worktreeManager.create_worktree。remove_worktree 刻意不是工具
    // （宿主清理 API，归 WorktreeManager 与宿主生命周期）。注入属 fix-3/P3 接线。
    if (!m_worktreeCreator) {
        return QStringLiteral("Error: Worktree support is not wired (setWorktreeCreator)");
    }
    return m_worktreeCreator(name, taskId);
}

QString AgentTeamsManager::runListTasks() const
{
    // 偏F：lcc :780-796 的 [ ]/[>]/[x] 文案与 lite s10 移植同源同款，直接转发
    return m_taskStore->runListTasks();
}

// ── P3 注入点 ──

void AgentTeamsManager::setTeammateLauncher(TeammateLauncher launcher)
{
    m_launcher = std::move(launcher);
}

void AgentTeamsManager::setWorktreeCreator(
    std::function<QString(const QString &, const QString &)> creator)
{
    m_worktreeCreator = std::move(creator);
}

void AgentTeamsManager::setPermissionCheck(
    std::function<QString(const QString &, const QJsonObject &)> check)
{
    m_permissionCheck = std::move(check);
}

void AgentTeamsManager::setHooksTrigger(
    std::function<QString(const QString &, const QString &, const QJsonObject &, const QString &)>
        hooks)
{
    m_hooksTrigger = std::move(hooks);
}

void AgentTeamsManager::setToolAdapter(
    const QString &toolName,
    std::function<QString(const QJsonObject &, const QString &)> adapter)
{
    m_toolAdapters.insert(toolName, std::move(adapter));
}
