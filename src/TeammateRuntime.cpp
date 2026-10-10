// TeammateRuntime — 队友运行时状态机实现（lcc s13 34775c8 agent_teams_manager.py
// TeammateRuntime :801-1063 的 Qt 事件驱动移植；线程形态偏离见头文件登记）。
//
// P2 交付边界：不含 ChatStream / 网络依赖（钉死第 1 条）。真实 LLM 回合由 P3 宿主
// 经 turnRequested → deliverTurnResult 闭环驱动；本 TU 只落状态迁移、信箱协议、
// 对话记录装配与租约三时点清算。

#include "TeammateRuntime.h"

#include "AgentConstants.h"
#include "AgentTeamsManager.h"
#include "CompactManager.h"
#include "MessageBus.h"
#include "TaskStore.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>

#include <optional>

namespace {
// P8 观测面 ok 判定：镜像 AgentLoop::isToolFailure 的折叠失败文案族前缀（单源在
// GUI 层 TU，本 TU 层律禁 include AgentLoop.h/AgentLoopInternal.h——镜像副本登记
// 偏差；改动文案族时两处同步）。仅供 teammateToolActivity 的 ok 诊断位，UI 不着色。
bool teamToolOutputLooksFailed(const QString &output)
{
    return output.startsWith(QStringLiteral("Error:"))
        || output == QStringLiteral("Permission denied")
        || output.startsWith(QStringLiteral("Blocked:"))
        || output.startsWith(QStringLiteral("[Background task start error]"))
        || output.startsWith(QStringLiteral("Unknown tool:"));
}
} // namespace

// ── 修① 上下文预算与压缩────────────────────────────────────────────
// 队友回合协议与主循环同源：≤ budget 平稳运行；超预算丢旧（保任务卡）+ usage 锚
// 外推；服务端溢出 → 反应式强压缩重发恰一次。两目标比在 AgentConstants 语义域外
// （预算单位是 token，比例只作用于换算后整数），系数取整为 qsizetype 防丢尾。
namespace {
// 常规压缩目标：预算的 80%（与主循环 CompactManager 压缩目标 0.8T' 同口径簇）
constexpr qsizetype kCompactTargetRatioNumerator = 4;
constexpr qsizetype kCompactTargetRatioDenominator = 5;
// 反应式压缩目标：预算的 50%（溢出后要留足余量给新回合输出）
constexpr qsizetype kReactiveTargetRatioNumerator = 1;
constexpr qsizetype kReactiveTargetRatioDenominator = 2;
} // namespace

// 宿主注入预算（initTeamEngine 拉线：full = contextTokenBudget()，overhead 见 AgentLoopTeam）。
void TeammateRuntime::setContextBudget(qsizetype fullBudgetTokens, qsizetype overheadTokens)
{
    m_fullBudgetTokens = qMax<qsizetype>(0, fullBudgetTokens);
    m_overheadTokens = qMax<qsizetype>(0, overheadTokens);
}

// lcc 无锚：队友回合增量仅来自 m_messages 尾部追加（无 system 改写、无注入块），
// 锚外推即「锚 prompt_tokens + 锚后每条消息估算」——与主循环 estimatedContextTokens
// 同口径（AgentConst::estimateTokens 串行化一致）。锚失效（改写/越界/未锚）回落全量。
qsizetype TeammateRuntime::estimatedContextTokens() const
{
    if (m_anchorPromptTokens > 0 && !m_rewrittenSinceAnchor && m_anchorMessageCount >= 0
        && m_anchorMessageCount <= m_messages.size()) {
        qint64 total = m_anchorPromptTokens;
        for (int i = m_anchorMessageCount; i < m_messages.size(); ++i) {
            total += AgentConst::estimateTokens(
                QString::fromUtf8(QJsonDocument(m_messages.at(i)).toJson(QJsonDocument::Compact)));
        }
        return qMax<qint64>(0, total);
    }
    return CompactManager::estimateTokens(m_messages) + m_overheadTokens;
}

// 锚定：发送点快照 prompt_tokens + 消息条数。countAtSend 必须与请求体一致性
// （AgentLoopTeam 构建 messages 时取 runtime->messages().size()），越界由锚有效性
// 守卫兜底回落全量。
void TeammateRuntime::adoptUsageAnchor(const QJsonObject &usage, int countAtSend)
{
    const double promptTokens = usage.value(QStringLiteral("prompt_tokens")).toDouble();
    if (promptTokens <= 0 || countAtSend < 0 || countAtSend > m_messages.size()) {
        return;
    }
    m_anchorPromptTokens = static_cast<qint64>(promptTokens);
    m_anchorMessageCount = countAtSend;
    m_rewrittenSinceAnchor = false;
}

// 主（非反应式）压缩：估算超预算才动手，目标 4/5 预算。丢旧只保留任务卡 [0]。
// 返回是否动过（锚作废）；m_messages 只删不重组，OpenAI 形态配对（assistant 带
// tool_calls 与其后连续 tool 是成组单元）在 compactConversationTo 内保证。
bool TeammateRuntime::compactConversationIfNeeded()
{
    if (m_fullBudgetTokens <= 0 || m_messages.size() <= 1) {
        return false;
    }
    const qsizetype target = m_fullBudgetTokens * kCompactTargetRatioNumerator
                             / kCompactTargetRatioDenominator;
    if (estimatedContextTokens() <= target) {
        return false;
    }
    const bool changed = compactConversationTo(target);
    return changed;
}

// 反应式：服务端已吐 400 溢出错误后才调，预算 1 次/回合（m_reactiveCompactUsed），
// 成功回填在 deliverTurnResult 成功路径复位。目标 1/2 预算（要留余量）。
bool TeammateRuntime::tryReactiveCompact()
{
    if (m_fullBudgetTokens <= 0 || m_reactiveCompactUsed || m_messages.size() <= 1) {
        return false;
    }
    const qsizetype target = m_fullBudgetTokens * kReactiveTargetRatioNumerator
                             / kReactiveTargetRatioDenominator;
    if (!compactConversationTo(target)) {
        return false;
    }
    m_reactiveCompactUsed = true;
    return true;
}

// 私有压缩内核：从最旧开始找「可安全成组删除」的单元：
//  - assistant 带 tool_calls：连同其随后连续 tool 结果一并删（tool 孤立删会破坏配对）
//  - 其余单条（user / 无 tool_calls 的 assistant）：可独立删
// 迭代直到估算低于目标或无可删单元。m_messages[0] 任务卡恒保留（索引 1 起扫）。
// QVector::remove 逐段 O(n)，队友回合规模下可接受。
bool TeammateRuntime::compactConversationTo(qsizetype targetTokens)
{
    bool changed = false;
    qsizetype current = estimatedContextTokens();
    int i = 1; // 任务卡 [0] 不删
    while (current > targetTokens && i < m_messages.size()) {
        const QJsonObject &head = m_messages.at(i);
        const QString role = head.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("tool")) {
            // 属于更早 assistant 的工具结果：不能独立删，继续向后找单元头
            ++i;
            continue;
        }
        int j = i + 1;
        if (role == QLatin1String("assistant") && head.contains(QLatin1String("tool_calls"))) {
            while (j < m_messages.size()
                   && m_messages.at(j).value(QStringLiteral("role")).toString()
                          == QLatin1String("tool")) {
                ++j;
            }
        }
        current -= CompactManager::estimateTokens(m_messages.mid(i, j - i));
        m_messages.remove(i, j - i);
        changed = true;
        (void)role; // (无 tool_calls 的 assistant / user 单条) 默认单元 [i, j)
    }
    if (changed) {
        m_rewrittenSinceAnchor = true;
        m_anchorPromptTokens = -1;
    }
    return changed;
}

TeammateRuntime::TeammateRuntime(const QString &name, const QString &role,
                                 const QString &prompt, const QString &taskId,
                                 bool requirePlan, AgentTeamsManager *manager,
                                 MessageBus *bus, TaskStore *taskStore,
                                 QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_role(role)
    , m_prompt(prompt)
    , m_taskId(taskId)
    , m_requirePlan(requirePlan)
    , m_manager(manager)
    , m_bus(bus)
    , m_taskStore(taskStore)
{
    Q_ASSERT(m_manager);
    Q_ASSERT(m_bus);
    Q_ASSERT(m_taskStore);

    // lcc IDLE_SCAN_INTERVAL=2.0s 的 QTimer 形态（零线程，钉死第 1 条）。
    // 构造不启动：只有 enterIdle()（回合空收尾/自拉活失败）才武装心跳；
    // 干活回合期间停表（lcc 里 work() 与 wait_for_work() 天然互斥的对应）。
    m_heartbeat = new QTimer(this);
    m_heartbeat->setInterval(AgentConst::kTeamIdleScanIntervalMs);
    m_heartbeat->setSingleShot(false);
    connect(m_heartbeat, &QTimer::timeout, this, &TeammateRuntime::onHeartbeat);
}

// lcc :811-821 逐字。禁翻区：发往 LLM 的 C 类串，永不 tr()。
QString TeammateRuntime::systemPrompt() const
{
    return QStringLiteral(
               "You are '%1', a %2. Use tools to complete the assigned Task, then "
               "call complete_task and report a concise result. If the first user "
               "message contains [Assigned task], that Task is already claimed; do "
               "not call claim_task for it again. When asked for a plan, call "
               "submit_plan and wait for approval before bash or file changes. "
               "File and shell tools use the Task's working directory; that "
               "directory is not a sandbox. The runtime delivers your final text "
"to Lead. Use send_message only for intermediate coordination, and "
                "address the coordinator as 'lead'.")
        // 修④ prompt 硬约束（有意偏离 lcc，防队友无限工具循环）：完成判定与收尾契约。
        // 要点：只有任务 description 里的交付物真实产出后才算完成；结束报告必须是无
        // 工具调用的纯文本总结回合；不得提前 complete_task，也不得在交付后再灌水工具。
        .arg(m_name, m_role)
        + QStringLiteral(
            " Treat the task as finished only when every deliverable named in the "
            "task description actually exists and no further tool call would change "
            "your report. When done, call complete_task, then end your turn with a "
            "plain-text summary that contains NO tool calls — that final text-only "
            "message is delivered to Lead as your result. Do not call complete_task "
            "before the deliverables exist, and do not keep issuing tool calls after "
            "they do: if the next round would only restate facts already written, "
            "stop and finish instead of looping. While your plan is pending approval, "
            "do not run file/shell tools or call complete_task.");
}

// lcc run()/__init__ 首条装配 :825-837 + work() 步① :944-946 的首轮 drain。
void TeammateRuntime::start()
{
    if (m_finished) {
        return;
    }

    QString content = m_prompt;
    if (!m_taskId.isEmpty()) {
        // 钉死第 6 条：任务卡现读盘（TaskSnapshot 是唯一跨模块视图，禁刮人类文本）；
        // cwd 经 assignmentCwd 现读（钉死第 3 条：禁读 leaseFor().cwd 缓存）。
        QVector<TaskStore::TaskSnapshot> tasks;
        QString listError;
        if (!m_taskStore->listTaskSnapshots(&tasks, &listError)) {
            sendToLead(QStringLiteral("Error: %1").arg(listError),
                       QStringLiteral("error"));
            finish();
            return;
        }
        const TaskStore::TaskSnapshot *assigned = nullptr;
        for (const TaskStore::TaskSnapshot &task : tasks) {
            if (task.id == m_taskId) {
                assigned = &task;
                break;
            }
        }
        if (!assigned) {
            // 有意偏离：lcc load_task 抛 FileNotFoundError 由线程 bootstrap 折叠成
            // 型名串；lite 用稳定文案（P3 宿主/日志两侧都更好判定）。
            sendToLead(QStringLiteral("Error: Task %1 not found").arg(m_taskId),
                       QStringLiteral("error"));
            finish();
            return;
        }
        QString cwd;
        QString cwdError;
        if (!m_taskStore->assignmentCwd(m_name, &cwd, &cwdError)) {
            sendToLead(QStringLiteral("Error: Invalid task assignment: %1").arg(cwdError),
                       QStringLiteral("error"));
            finish();
            return;
        }
        // Gate② FIND-H 修复（M3 扩形）：恢复 lcc 任务卡 description 行——lcc :829-832
        // 现文逐字："\n\n[Assigned task {id}] {subject}\n{description}\nWork directory: {cwd}"。
        // lcc 对空 description 无分支（f-string 直接插值成空行），lite 同形照拼。
        content += QStringLiteral("\n\n[Assigned task %1] %2\n%3\nWork directory: %4")
                       .arg(assigned->id, assigned->subject, assigned->description, cwd);
    }
    if (m_requirePlan) {
        content += QStringLiteral(
            "\n\n[Plan required] Submit a plan and wait for Lead approval before "
            "changing files or using bash.");
    }
    appendUser(content);

    // lcc work() 第一步永远 drain（:944-946）：shutdown 能在任何 API 调用前截住。
    // 出生瞬间信箱通常为空，但 Lead 在 spawn 返回后、本 start 被宿主排队执行前
    // 已发信的时序是真实可能的（lcc 线程同竞）。
    const QVector<BusMessage> inbox = m_bus->drain(m_name);
    if (!inbox.isEmpty() && m_bus->lastError().isEmpty() && handleInbox(inbox)) {
        finish();
        return;
    }

    ++m_turnNo; // 新回合开编号（观测面 turnNo：第 N 个模型回合，自 1 起）
    emit turnRequested(m_name);
}

void TeammateRuntime::cancel()
{
    // 宿主显式退场（关会话）：等价 lcc daemon 随进程终止的 lite 显式化——
    // 走完整清算路径，内存台账不留幽灵。
    if (!m_finished) {
        finish();
    }
}

// lcc wait_for_work() :998-1023 的单次轮询形态（心跳一拍 = loop 一圈）：
// drain → handleInbox/托盘唤醒 → 自拉活 → 无事继续待命。
void TeammateRuntime::onHeartbeat()
{
    if (m_finished) {
        return;
    }
    if (m_pendingTool) {
        // 挂起窗口不开新回合（MINOR-4 防重入闸）：心跳回合中间本就不武装，
        // 此闸是防御——挂起期间不动信箱、不自拉活，等适配器收口驱动续跑。
        return;
    }

    const QVector<BusMessage> inbox = m_bus->drain(m_name);
    if (inbox.isEmpty() && !m_bus->lastError().isEmpty()) {
        // M8 fail-closed（fix-4 钉死第 2 条）：unlink 失败 → MessageBus 已把信保留
        // 在信箱里，本拍什么都不做，下一拍重试。lcc 的 read 抛错→error 信形态在此
        // 折叠为静默重试（重试比重试前还发 error 信更稳）。
        return;
    }

    if (!inbox.isEmpty()) {
        const int before = m_messages.size();
        if (handleInbox(inbox)) {
            // shutdown 已接受并回执：合作式退场（lcc return False → run() break）。
            finish();
            return;
        }
        if (m_messages.size() > before) {
            // 托盘有货 → 唤醒干活（lcc :1006-1010 len 比较的对应）。
            leaveIdle();
            m_manager->setTeammateStatus(m_name,
                                         AgentTeamsManager::TeammateStatus::Working);
            ++m_turnNo; // 新回合开编号（观测面 turnNo 口径）
            emit turnRequested(m_name);
            return;
        }
        // 批内全是「Ignored …」之外的协议噪声却没长出托盘（理论上 handleInbox
        // 恒产托盘，除非整批被 shutdown 前缀截断）——lcc continue 形：落到自拉活。
    }

    // 自拉活（lcc :1012-1023）：waiting_approval 的队友心跳照打，claimNextTask 的
    // 租约首门自闸挡自取（钉死第 3 条：不用特判状态）。
    const std::optional<TaskStore::TaskSnapshot> next =
        m_manager->claimNextTask(m_name);
    if (next.has_value()) {
        QString cwd;
        QString cwdError;
        if (!m_taskStore->assignmentCwd(m_name, &cwd, &cwdError)) {
            // lcc :1014 assignment_cwd 抛错 → run() 外层 except 发 error 信后
            // finally 清算——lite 等价：error 信 + finish()。
            sendToLead(QStringLiteral("Error: Invalid task assignment: %1").arg(cwdError),
                       QStringLiteral("error"));
            finish();
            return;
        }
        // Gate② FIND-H 修复：description 行恢复，逐字对齐 lcc :1015-1021 现文
        // "[Auto-claimed task {id}] {subject}\n{description}\nWork directory: {cwd}"
        //（lcc 空 description 同样无分支，照拼空行）。
        appendUser(QStringLiteral("[Auto-claimed task %1] %2\n%3\nWork directory: %4")
                       .arg(next->id, next->subject, next->description, cwd));
        leaveIdle();
        m_manager->setTeammateStatus(m_name,
                                     AgentTeamsManager::TeammateStatus::Working);
        ++m_turnNo; // 新回合开编号（观测面 turnNo 口径）
        emit turnRequested(m_name);
        return;
    }

    // 无活可拉：心跳继续跑（lcc loop 的 continue 分支）。状态账本不在此写——
    // lcc 只在 work() 入口写 working、收尾写 idle，本类写点保持 1:1。
}

// lcc handle_inbox :901-928。返回 true = shutdown 已接受、该退场了。
bool TeammateRuntime::handleInbox(const QVector<BusMessage> &inbox)
{
    m_pendingTray.clear();
    for (const BusMessage &msg : inbox) {
        const QString msgType =
            msg.type.isEmpty() ? QStringLiteral("message") : msg.type;

        if (msgType == QLatin1String("shutdown_request")) {
            QString out;
            if (!m_manager->applyShutdownRequest(m_name, msg, &out)) {
                appendTrayNotice(out); // [Ignored shutdown request: ...]
                continue;
            }
            QJsonObject ack;
            ack.insert(QStringLiteral("request_id"), out);
            ack.insert(QStringLiteral("approve"), true);
            m_bus->send(m_name, QStringLiteral("lead"),
                        QStringLiteral("Shutdown acknowledged."),
                        QStringLiteral("shutdown_response"), ack);
            // 有意丢弃已攒托盘（lcc :913-914 语义：return True 前不 append 托盘）——
            // 模型永远看不见关机信本身；顺序陷阱：批内 shutdown 夹中间时，
            // 它后面的信已被破坏性读取 unlink，永久丢失（lcc :906-909 同款接受）。
            return true;
        }

        if (msgType == QLatin1String("plan_approval_response")) {
            QString out;
            // lcc :917：bool 忽略、notice 恒附（[Ignored ...] 或 [Plan approved] …）。
            m_manager->applyPlanResponse(m_name, msg, &out);
            appendTrayNotice(out);
            continue;
        }

        if (msgType == QLatin1String("plan_request")) {
            appendTrayNotice(QStringLiteral("[Plan required] %1").arg(msg.content));
            continue;
        }

        appendTrayNotice(
            QStringLiteral("[Message from %1] %2").arg(msg.from, msg.content));
    }

    if (trayHasContent()) {
        flushTray(); // 每次 API 调用只端一个托盘（lcc :922-927）
    }
    return false;
}

// lcc work() :942-991 的「create 返回之后」半段（P2：create 由宿主做，本函数接产物）。
void TeammateRuntime::deliverTurnResult(const QString &assistantText,
                                        const QJsonArray &toolCalls,
                                        const QString &errorMessage)
{
    if (m_finished) {
        return;
    }
    if (m_pendingTool) {
        // 挂起窗口迟到的回合结果：宿主单飞闸下物理不可达（挂起期间不发
        // turnRequested、无从起新流），此闸防御——不得清批队列态（MINOR-4）。
        return;
    }

    if (!errorMessage.isEmpty()) {
        // lcc :963-970 except：error 信发 Lead + return "stop"。
        // （宿主侧 ChatStream 的异常串已在 P3 按 lcc 口径 f"{type}: {exc}" 组装。）
        sendToLead(errorMessage, QStringLiteral("error"));
        finish();
        return;
    }

    // 修① 反应式压缩预算回填：只有「拿到合法回合产物」才复位——响应式压缩重发
    // 本身不经过本函数（宿主直接再开流），成功路径到此即预算可用（1 次/turn）。
    m_reactiveCompactUsed = false;

    appendAssistant(assistantText, toolCalls);

    if (!toolCalls.isEmpty()) {
        // lcc :971-977：逐块执行并回填，return "continue"（while 下一轮）。
        // MINOR-4：批执行拆入 runToolBatch——异步工具挂起时保存批位退出，
        // 续跑经 resumePendingBatch 接批；批完 drain/turnRequested 照旧。
        m_manager->setTeammateStatus(m_name,
                                     AgentTeamsManager::TeammateStatus::Working);
        runToolBatch(toolCalls);
        return;
    }

    // 无工具块 = 回合文本即总结（lcc :979-991）。
    // 有意偏离（信封形态）：lcc _last_assistant_text 取首个 text 块；lite OpenAI 形
    // 回合 assistantText 即纯文本，trimmed 等价。
    const QString summary = assistantText.trimmed();
    const AgentTeamsManager::PlanGate gate = m_manager->planGate(m_name);

    if (gate != AgentTeamsManager::PlanGate::Pending && !summary.isEmpty()) {
        // '完工'与'可用'正交（lcc :941 注）：Lead 看 [result]+[idle_notification] 各一行。
        sendToLead(summary, QStringLiteral("result"));
        emit taskFinished(m_name, summary);
    }

    if (gate == AgentTeamsManager::PlanGate::Pending) {
        // lcc :986-989：在审案不是完工报告——result 不发、不退租、idle_notification
        // 也不发；任务仍 in_progress，租约必须活着（Lead 手里已有案卷，批复经心跳收）。
        m_manager->setTeammateStatus(m_name,
                                     AgentTeamsManager::TeammateStatus::WaitingApproval);
    } else {
        // 回合边界释放（lcc :990，fix-4 钉死第 4 条：状态机自调，completeTask 不管释放）。
        // lcc 裸调用忽略返回值：false=无租约/未完成属幂等常态。有意偏离（Gate② FIND-M
        // 补录诊断面）：TaskStore 的「租约任务不可读」类错误同样静默（lcc 该处抛
        // ValueError 会进外层 except 发 error 信——lite 判断：幂等噪音不值得打扰 Lead）。
        // 补偿通道：qWarning + manager 诊断面 noteReleaseWarning（P3b 宿主遥测可读
        // lastReleaseWarning，连续失败不再只有滚走的日志）。
        QString releaseError;
        if (!m_taskStore->releaseCompletedAssignment(m_name, &releaseError)
            && !releaseError.isEmpty()) {
            m_manager->noteReleaseWarning(releaseError);
            qWarning() << "[team] release_completed_assignment(" << m_name
                       << "):" << releaseError;
        }
        m_manager->setTeammateStatus(m_name, AgentTeamsManager::TeammateStatus::Idle);
        sendToLead(QStringLiteral("Waiting for more work."),
                   QStringLiteral("idle_notification"));
    }

    enterIdle();
}

// 工具批执行（Gate③ MINOR-4 拆自 deliverTurnResult 工具分支）。同步工具即时回填
// 续批；异步工具哨兵 → 存批位、向 manager 挂续跑闭包后退出本栈（不 drain、不续轮）。
// 挂起窗口内 cancel()/finish() 到达：settleLedgers 销 manager 账 + 清三态，晚归
// 结果在 resumePendingTool 丢弃、终态照 finish 清算链——shutdown 观察点推迟到
// 批尾 drain（至多当前工具的有界超时），裁决口径登记于报告。
void TeammateRuntime::runToolBatch(const QJsonArray &toolCalls)
{
    for (int i = 0; i < toolCalls.size(); ++i) {
        const QJsonObject tc = toolCalls.at(i).toObject();
        const QString callId = tc.value(QStringLiteral("id")).toString();
        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        const QString toolName = fn.value(QStringLiteral("name")).toString();
        // arguments 兼容对象/JSON 串两形（repo-wide parseToolCall 同款容错；
        // 本类不 include AgentLoopDetail 重 TU，P3 宿主可统一）。
        QJsonObject params;
        const QJsonValue argsVal = fn.value(QStringLiteral("arguments"));
        if (argsVal.isObject()) {
            params = argsVal.toObject();
        } else if (argsVal.isString()) {
            const QJsonDocument doc =
                QJsonDocument::fromJson(argsVal.toString().toUtf8());
            if (doc.isObject()) {
                params = doc.object();
            }
        }
        // 队友全部工具（含 base5/send_message/submit_plan/task 三件/协议门）
        // 收敛在 manager 单点（偏C）；plan 门/权限/hook 都在那里。
        const QString output = m_manager->runTeammateTool(m_name, toolName, params);
        if (output == AgentTeamsManager::asyncPendingSentinel()) {
            // 异步挂起：哨兵不是 tool_result——当前项不回填，存批位后交闭包退栈。
            // 挂位窗口内 done 已同步早归的（暂存态）由 setPendingResume 同栈续跑。
            m_pendingTool = true;
            m_pendingCallId = callId;
            QJsonArray rest;
            for (int j = i + 1; j < toolCalls.size(); ++j) {
                rest.append(toolCalls.at(j));
            }
            m_pendingBatch = rest;
            // 挂起项身份快照（P8 观测面）：异步收口点在 resumePendingBatch 补发
            // 活动行时取用——发起点不发，行不先于结果出现。
            m_pendingToolName = toolName;
            m_pendingArgs = params;
            m_manager->setPendingResume(m_name,
                                        [this](const QString &result) {
                resumePendingBatch(result);
            });
            return;
        }
        appendToolResult(callId, output);
        // P8 观测面：工具真实收口（回填完成）才发活动行——协议零影响，纯 emit。
        emit teammateToolActivity(m_name, m_turnNo, toolName, params,
                                  !teamToolOutputLooksFailed(output));
    }

    // 中间链 drain：对齐 lcc「work() 第一步永远 drain」（:944-946）——多工具回合
    // 续轮前让 shutdown 有机会截住（否则最长拖一整轮 LLM 时延）。
    const QVector<BusMessage> inbox = m_bus->drain(m_name);
    if (!inbox.isEmpty() && m_bus->lastError().isEmpty() && handleInbox(inbox)) {
        finish();
        return;
    }

    ++m_turnNo; // 新回合开编号（观测面 turnNo 口径）
    emit turnRequested(m_name);
}

// 续跑入口：manager 在销账+PostToolUse 补发后调进来。回填挂起项、接续剩余批
// （可能再次挂起——递归深度受批大小界，同栈无事件循环重入）。
void TeammateRuntime::resumePendingBatch(const QString &result)
{
    if (m_finished || !m_pendingTool) {
        return; // 已退场清算 / 非挂起态：防御丢弃（manager 侧账已同步出清）
    }
    m_pendingTool = false;
    const QString callId = m_pendingCallId;
    m_pendingCallId.clear();
    const QJsonArray rest = m_pendingBatch;
    m_pendingBatch = QJsonArray();
    const QString toolName = m_pendingToolName; // 发起点存的身份快照
    m_pendingToolName.clear();
    const QJsonObject args = m_pendingArgs;
    m_pendingArgs = QJsonObject();
    appendToolResult(callId, result);
    // P8 观测面：异步工具在**真实结果到达的收口点**发活动行（发起点不发——
    // 行不先于结果；挂起窗口内被清算时本函数根本到不了，行自然缺失，正确）。
    emit teammateToolActivity(m_name, m_turnNo, toolName, args,
                              !teamToolOutputLooksFailed(result));
    runToolBatch(rest);
}

// 账目核（Gate② FIND-L 拆分）：闩锁 → 停心跳 → 退租 → 弹四本账。**全程零 emit**，
// 析构上下文可安全调用；失败上报职责留给 emit 壳/析构兜底各通道（见出参注释）。
bool TeammateRuntime::settleLedgers(QString *outCleanupError)
{
    if (m_finished) {
        if (outCleanupError) {
            outCleanupError->clear();
        }
        return false; // 终态闩锁幂等
    }
    m_finished = true;
    leaveIdle(); // m_heartbeat 是本对象子 QTimer，stop() 在析构上下文同样安全

    // 挂起台账出清（MINOR-4）：终态在挂起窗口到达（cancel/shutdown/error）时，
    // manager 销账使晚到的适配器结果在 resumePendingTool 无账可查即丢弃；
    // 本地批队列态一并复位。终态清算链照走，不因挂起而变。
    m_pendingTool = false;
    m_pendingCallId.clear();
    m_pendingBatch = QJsonArray();
    m_pendingToolName.clear(); // P8 观测面身份快照同窗口出清（m_turnNo 不复位——终态将亡）
    m_pendingArgs = QJsonObject();
    m_manager->clearPendingResume(m_name);

    // 死亡清算释放点（lcc :1048；fix-4 钉死第 4 条之二）：遗留 in_progress 打回
    // pending 让别的 idle 队友捡。lcc except→"Assignment cleanup failed: {型名}: {exc}"
    // error 信的 lite 形（TaskStore 已把失败折叠为 error 出参，型名段并入文案头）。
    // 退租失败**不阻断销账**：TaskStore finally 形态已保证内存租约出清，四本账照弹，
    // 失败文本经出参交上层选择上报通道（emit 壳=error 信，析构=qWarning）。
    const bool released =
        m_taskStore->releaseTeammateAssignment(m_name, outCleanupError);
    if (!released && outCleanupError && !outCleanupError->isEmpty()) {
        m_manager->noteReleaseWarning(*outCleanupError); // Gate② FIND-M 诊断面
    }

    m_manager->finalizeTeammate(m_name);
    return true;
}

// lcc run() finally :1044-1057 清算序的 emit 壳：账目核 → 失败上报 → finished。
// 全程无 force-kill。上报顺序说明：账已弹后才发 error 信/finished——lcc 是
// 「except 上报先于 pop」，lite 可观测面（Lead 信箱读取在 Lead 回合边界、
// finished 消费在宿主槽）均不区分这个先后，属无外部可见差异的重排。
void TeammateRuntime::finish()
{
    QString cleanupError;
    if (!settleLedgers(&cleanupError)) {
        return;
    }
    if (!cleanupError.isEmpty()) {
        sendToLead(QStringLiteral("Assignment cleanup failed: %1").arg(cleanupError),
                   QStringLiteral("error"));
    }
    emit finished(m_name);
}

// Gate② FIND-L 析构兜底：正道是宿主 cancel()→delete（契约见类头注释）；这里只接
// 违约跳过 cancel 的宿主——账目核照走（退租+弹账），**一律不 emit**（析构期发信号
// 不安全），清理失败降为 qWarning（诊断面已在核内记账）。心跳定时器为子对象，
// 随 QObject 基类析构自然消亡，零线程无残留。
TeammateRuntime::~TeammateRuntime()
{
    QString cleanupError;
    if (settleLedgers(&cleanupError) && !cleanupError.isEmpty()) {
        qWarning() << "[team] dtor settlement cleanup failed(" << m_name
                   << "):" << cleanupError;
    }
}

void TeammateRuntime::enterIdle()
{
    if (m_heartbeat && !m_heartbeat->isActive()) {
        m_heartbeat->start();
    }
}

void TeammateRuntime::leaveIdle()
{
    if (m_heartbeat && m_heartbeat->isActive()) {
        m_heartbeat->stop();
    }
}

void TeammateRuntime::appendUser(const QString &content)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));
    msg.insert(QStringLiteral("content"), content);
    m_messages.append(msg);
}

// 有意偏离（信封形态，登记）：lcc Anthropic content-block 列表 → lite OpenAI 形
// {role, content[, tool_calls]}（与 SubAgent/AgentLoop 全仓消息形状同源）。
void TeammateRuntime::appendAssistant(const QString &text, const QJsonArray &toolCalls)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    msg.insert(QStringLiteral("content"), text);
    if (!toolCalls.isEmpty()) {
        msg.insert(QStringLiteral("tool_calls"), toolCalls);
    }
    m_messages.append(msg);
}

void TeammateRuntime::appendToolResult(const QString &callId, const QString &output)
{
    // lite 消息形态（SubAgent.cpp:279-283 同款）：lcc :971-974 的 tool_result 块
    // 是同一事实的另一种信封。
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("tool"));
    msg.insert(QStringLiteral("tool_call_id"), callId);
    msg.insert(QStringLiteral("content"), output);
    m_messages.append(msg);
}

void TeammateRuntime::appendTrayNotice(const QString &text)
{
    m_pendingTray.append(text);
}

void TeammateRuntime::flushTray()
{
    if (m_pendingTray.isEmpty()) {
        return;
    }
    appendUser(m_pendingTray.join(QLatin1Char('\n')));
    m_pendingTray.clear();
}

void TeammateRuntime::sendToLead(const QString &content, const QString &type,
                                 const QJsonObject &metadata)
{
    const bool ok = m_bus->send(m_name, QStringLiteral("lead"), content, type, metadata);
    if (!ok) {
        // 尽力而为（偏离登记）：lcc 裸 send 抛错会进 run() 外层 except → error 信；
        // lite MessageBus 失败已折叠为 lastError，此处 qWarning 记账不再二次上报
        // （上报本身也是 send，失败即递归无意义）。
        qWarning() << "[team] send to lead failed:" << m_bus->lastError();
        return;
    }
    // 展示信号双通道之一（界面点；信箱是给 Lead 模型的）。
    emit teamEvent(type, m_name, content,
                   metadata.value(QStringLiteral("request_id")).toString());
}
