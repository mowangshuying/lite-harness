// tst_agentteams.cpp — s13 Agent Teams 内核数据层（AgentTeamsManager）断言套件。
//
// lcc s13 34775c8 agent_teams_manager.py 移植验收证据（port-s13 P2 fix-4 · Gate② 补建）。
// 覆盖 15 组：spawn 校验与回滚矩阵 / list_teammates 形态 / send_message /
// submitPlan 案卷与拒重提 / applyPlanResponse 11 门 / applyShutdownRequest 8 门 /
// review_plan 五校验串与「Lead 不翻队友门」/ matchResponse 4 门 /
// runTeammateTool 三门与 cwd 现读 / 释放三时点与回调接线 /
// consumeLeadInbox 不吞协议 + formatTeamEvents 逐字行形 + M8 失败重试 /
// genRequestId 形态 / claimNextTask 自拉活 / Gate② P3a-B minor 批回归面
// （FIND-D/E 断言随组1、组10 轨迹钉桩，组14 补 FIND-M 诊断面语义与 FIND-N3 常量单源）
// / Gate③ MINOR-4 异步工具桥 manager 侧面貌（组15）。
//
// 套件纪律：临时根一律 ScopedTempRoot（仅认 LITE_TEST_TMPROOT，未设即 SKIP 返 0）；
// 期望文本从 src/AgentTeamsManager.cpp 与 src/TaskStore.cpp 实抄、与 lcc 逐字对照；
// 不为绿迁就——凡实现与 lcc 规格冲突处按正确规格立期望（本轮无此类红项，
// 「疑似生产缺陷」在报告单列）。零删除逻辑、零线程、零事件循环依赖。
//
// 注册要求（编排者收口）：测试目标须加入 src/AgentTeamsManager.cpp、src/MessageBus.cpp
// 并开启 CMAKE_AUTOMOC（AgentTeamsManager 为 Q_OBJECT）；仅链 Qt6::Core。
// TeammateRuntime 不在测试目标（宿主驱动、P3 才有消费者），本套件不实例化它。

#include "TestHarness.h"
#include "ScopedTempRoot.h"
#include "AgentTeamsManager.h"
#include "MessageBus.h"
#include "TaskStore.h"
#include "ToolNames.h"
#include "AgentConstants.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>
#include <functional>
#include <optional>

namespace {

// 从 runCreateTask 返回文本（"Created <id>: <subject>"）提取任务 id。
QString newTask(TaskStore &store, const QString &subject)
{
    QJsonObject args;
    args.insert(QStringLiteral("subject"), subject);
    args.insert(QStringLiteral("description"), QStringLiteral("desc"));
    const QString text = store.runCreateTask(args);
    const QRegularExpression re(QStringLiteral("task_[0-9a-f]{8}"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(0) : QString();
}

QJsonObject idArgs(const QString &taskId)
{
    QJsonObject args;
    args.insert(QStringLiteral("task_id"), taskId);
    return args;
}

// 手写任务文件夹具（与 tst_taskstore_lease 同纪律：mkpath 与文件共用同一 .task 基底）。
bool writeRawTaskFile(const QString &root, const QString &id, const QString &json)
{
    const QString taskDir = QDir(root).filePath(QStringLiteral(".task"));
    if (!QDir().mkpath(taskDir)) {
        return false;
    }
    QFile file(QDir(taskDir).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(json.toUtf8());
    file.close();
    return true;
}

// 六键旧形态合法任务文件（size==6+(hasTs?1:0)+(hasWt?1:0) 口径）。
QString legacyTaskJson(const QString &id, const QString &subject,
                       const QString &status, const QString &ownerJson)
{
    return QStringLiteral(
               "{\n"
               "  \"id\": \"%1\",\n"
               "  \"subject\": \"%2\",\n"
               "  \"description\": \"desc\",\n"
               "  \"status\": \"%3\",\n"
               "  \"owner\": %4,\n"
               "  \"blockedBy\": []\n"
               "}")
        .arg(id, subject, status, ownerJson);
}

// 直构总线信（BusMessage 为自由结构，测试侧可绕开总线直接投递）。
BusMessage mkMsg(const QString &from, const QString &to, const QString &content,
                 const QString &type, const QString &requestId, bool approve)
{
    BusMessage msg;
    msg.from = from;
    msg.to = to;
    msg.content = content;
    msg.type = type;
    if (!requestId.isEmpty()) {
        QJsonObject meta;
        meta.insert(QStringLiteral("request_id"), requestId);
        meta.insert(QStringLiteral("approve"), approve);
        msg.metadata = meta;
    }
    return msg;
}

// 从 submitPlan 返回文本里抓 req id。
QString captureReqId(const QString &text)
{
    const QRegularExpression re(QStringLiteral("(req_\\d{6})"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 记账器：hook / permission / adapter 调用痕迹。
struct Recorder {
    int permissionCalls = 0;
    QStringList hookCalls;          // "evt|tool" 序列
    QString preBlockFor;            // 对该工具名的 PreToolUse 返回拦截串
    int readCalls = 0;
    QString readCwd;
    int bashCalls = 0;
    QString bashCwd;
    int writeCalls = 0;
};

const QLatin1String kPending("pending");

// ————————————————————————————————————————————————————————————————
// 组1：spawn 三道校验 + 带单成功/失败回滚矩阵（lcc :628-684）
// ————————————————————————————————————————————————————————————————
void testSpawnValidationAndRollback()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    const QString badName = mgr.runSpawnTeammate(QStringLiteral("bad name!"),
                                                 QStringLiteral("tester"),
                                                 QStringLiteral("p"), QString(), false);
    TestHarness::check(badName == QStringLiteral("Invalid teammate name: use 1-64 letters, digits, underscores, or dashes"),
                       "spawn门①: 非法名字逐字拒绝");
    const QString reservedLead = mgr.runSpawnTeammate(QStringLiteral("lead"),
                                                      QStringLiteral("tester"),
                                                      QStringLiteral("p"), QString(), false);
    TestHarness::check(reservedLead == QStringLiteral("Invalid teammate name: 'lead' is reserved by the runtime"),
                       "spawn门②: 保留名 lead 拒绝");
    const QString reservedAgentUpper = mgr.runSpawnTeammate(QStringLiteral("AGENT"),
                                                            QStringLiteral("tester"),
                                                            QStringLiteral("p"), QString(), false);
    TestHarness::check(reservedAgentUpper == QStringLiteral("Invalid teammate name: 'AGENT' is reserved by the runtime"),
                       "spawn门②: 保留名 AGENT 大小写折叠同拒");

    // 未注册 launcher：spawn 照成功、无句柄（句柄无公共查询面，偏A 非所有权登记不可观测）。
    const QString okText = mgr.runSpawnTeammate(QStringLiteral("bob"),
                                                QStringLiteral("tester"),
                                                QStringLiteral("do stuff"), QString(), false);
    TestHarness::check(okText == QStringLiteral("Teammate 'bob' spawned as tester without an initial Task. End this turn; the runtime will deliver its events."),
                       "spawn成功: 无单文本逐字");
    TestHarness::check(mgr.hasTeammate(QStringLiteral("bob")), "spawn成功: 名册登记");
    const std::optional<AgentTeamsManager::TeammateStatus> st = mgr.teammateStatus(QStringLiteral("bob"));
    TestHarness::check(st.has_value() && *st == AgentTeamsManager::TeammateStatus::Working,
                       "spawn成功: 初始状态 working");
    TestHarness::check(mgr.planGate(QStringLiteral("bob")) == AgentTeamsManager::PlanGate::NotRequired,
                       "spawn(requirePlan=false): 门 not_required");

    const QString dupText = mgr.runSpawnTeammate(QStringLiteral("Bob"),
                                                 QStringLiteral("tester"),
                                                 QStringLiteral("p"), QString(), false);
    TestHarness::check(dupText == QStringLiteral("Teammate 'Bob' already exists"),
                       "spawn门③: 大小写折叠查重拒绝");

    const QString zoeText = mgr.runSpawnTeammate(QStringLiteral("zoe"),
                                                 QStringLiteral("planner"),
                                                 QStringLiteral("p"), QString(), true);
    TestHarness::check(zoeText.startsWith(QStringLiteral("Teammate 'zoe' spawned as planner")),
                       "spawn(requirePlan=true): 文本形态");
    TestHarness::check(mgr.planGate(QStringLiteral("zoe")) == AgentTeamsManager::PlanGate::Required,
                       "spawn(requirePlan=true): 门 required");

    // 带单出生：Gate② FIND-E 新契约——launcher 返 nullptr = 创建失败 → spawn
    // fail-closed 回滚（退租+弹四本账+折叠报错），旧「幽灵队友」形态（成功文案照返、
    // 租约悬挂）永不可达。以下期望重写属该行为变更的合法期望更新（Gate② P3 前置
    // 条件② 裁决，非弱化断言——断言面反而更宽：名册/租约/版本/文本四路对账）。
    int launcherCalls = 0;
    QString launcherSawName;
    QString launcherSawTask;
    bool launcherSawPlan = false;
    mgr.setTeammateLauncher([&](const QString &name, const QString &, const QString &,
                                const QString &taskId, bool requirePlan) -> TeammateRuntime * {
        ++launcherCalls;
        launcherSawName = name;
        launcherSawTask = taskId;
        launcherSawPlan = requirePlan;
        return nullptr;
    });
    const QString t1 = newTask(store, QStringLiteral("first-job"));
    TestHarness::check(!t1.isEmpty(), "spawn夹具: 任务创建成功");
    const QString devText = mgr.runSpawnTeammate(QStringLiteral("dev"),
                                                 QStringLiteral("coder"),
                                                 QStringLiteral("p"), t1, false);
    TestHarness::check(devText == QStringLiteral("Error: Teammate runtime failed to start for 'dev'"),
                       "FIND-E: launcher 返 nullptr → 折叠报错文本逐字（成功文案不可达）");
    TestHarness::check(launcherCalls == 1 && launcherSawName == QStringLiteral("dev")
                           && launcherSawTask == t1 && !launcherSawPlan,
                       "FIND-E: launcher 实参透传（先认领后创建，lcc :652-677 写序不变）");
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("dev")),
                       "FIND-E: 回滚全链——名册弹出（幽灵不可达）");
    TestHarness::check(!store.leaseFor(QStringLiteral("dev")).has_value(),
                       "FIND-E: 回滚全链——出生租约已退（悬挂幽灵永绝）");
    TestHarness::check(store.assignmentVersion(QStringLiteral("dev")) == 1,
                       "FIND-E: 认领后版本仍 1（TaskStore 退租不降版本，偏差④同款）");

    // 认领失败回滚：t1 需 in_progress → 门②'（status!=pending）文本 → 回滚镜像登记。
    // FIND-E 改造后 dev 出生租约已随回滚退清、t1 打回 pending+无主——旧形态靠幽灵
    // 租约占着 t1 的前提永不可达，改用 agent（Lead 保留 owner 键）真实认领造确定性
    // in_progress 夹具。
    TestHarness::check(store.runClaimTaskLeased(idArgs(t1), QStringLiteral("agent"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "FIND-E级联夹具: agent 认领使 t1 回 in_progress");
    const int callsBeforeFail = launcherCalls;
    const QString carolText = mgr.runSpawnTeammate(QStringLiteral("carol"),
                                                   QStringLiteral("coder"),
                                                   QStringLiteral("p"), t1, false);
    TestHarness::check(carolText == QStringLiteral("Cannot spawn teammate 'carol': Task %1 is in_progress, cannot claim").arg(t1),
                       "spawn回滚: 认领失败文本透传 kernel");
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("carol")),
                       "spawn回滚: 名册已撤销（hasTeammate false）");
    TestHarness::check(mgr.planGate(QStringLiteral("carol")) == AgentTeamsManager::PlanGate::NotRequired,
                       "spawn回滚: 门登记已撤销");
    TestHarness::check(launcherCalls == callsBeforeFail,
                       "spawn回滚: launcher 未被调用（认领先于线程创建，lcc :652-663）");

    // 门②（pending+有主）经手写夹具可达（公开 API 造不出该脏态）。
    const QString dirty = QStringLiteral("task_000000c1");
    TestHarness::check(writeRawTaskFile(root, dirty,
                                       legacyTaskJson(dirty, QStringLiteral("dirty-job"),
                                                      QStringLiteral("pending"),
                                                      QStringLiteral("\"eve\""))),
                       "spawn回滚夹具: pending+owner=eve 手写成功");
    const QString eveText = mgr.runSpawnTeammate(QStringLiteral("newbie"),
                                                 QStringLiteral("coder"),
                                                 QStringLiteral("p"), dirty, true);
    TestHarness::check(eveText == QStringLiteral("Cannot spawn teammate 'newbie': Task %1 is already owned by eve").arg(dirty),
                       "spawn回滚: 门②已有主文本（kernel 承重串透传）");
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("newbie")),
                       "spawn回滚: requirePlan=true 失败同样撤登记");

    // ————— Gate② FIND-E 回滚全链（磁盘健康形态）+ FIND-M 空记点交叉 —————
    const QString t2 = newTask(store, QStringLiteral("second-job"));
    TestHarness::check(!t2.isEmpty(), "FIND-E夹具: t2 创建成功");
    const QString wraithText = mgr.runSpawnTeammate(QStringLiteral("wraith"),
                                                    QStringLiteral("coder"),
                                                    QStringLiteral("p"), t2, false);
    TestHarness::check(wraithText == QStringLiteral("Error: Teammate runtime failed to start for 'wraith'"),
                       "FIND-E: 报错文本逐字（第二例，名字入串）");
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("wraith")), "FIND-E: 名册弹出");
    TestHarness::check(mgr.planGate(QStringLiteral("wraith")) == AgentTeamsManager::PlanGate::NotRequired,
                       "FIND-E: 门账弹出（缺省语义）");
    TestHarness::check(!store.leaseFor(QStringLiteral("wraith")).has_value(),
                       "FIND-E: 出生租约已退");
    QVector<TaskStore::TaskSnapshot> g1Snaps;
    QString g1SnapErr;
    TestHarness::check(store.listTaskSnapshots(&g1Snaps, &g1SnapErr), "FIND-E夹具: 快照可读");
    bool t2Reverted = false;
    for (const TaskStore::TaskSnapshot &s : g1Snaps) {
        if (s.id == t2) {
            t2Reverted = (s.status == kPending && s.owner.isEmpty());
        }
    }
    TestHarness::check(t2Reverted, "FIND-E: 初始任务打回 pending+无主（死亡清算同款降级）");
    TestHarness::check(mgr.lastReleaseWarning().isEmpty(),
                       "FIND-M: 回滚退租成功不记点（诊断面保持空）");

    // ————— FIND-E 阳性路径：launcher 返非空句柄 → 登记 + 成功文案（回滚分支不误伤） —————
    // 哨兵指针：manager 侧对 m_teammateHandles 只 insert/remove、零解引用
    // （Gate② 焦点③ 亲证），取本地 int 地址仅验「非 nullptr 即成功」分支可达。
    mgr.setTeammateLauncher([&launcherCalls](const QString &, const QString &, const QString &,
                                             const QString &, bool) -> TeammateRuntime * {
        return reinterpret_cast<TeammateRuntime *>(static_cast<void *>(&launcherCalls));
    });
    const QString safeText = mgr.runSpawnTeammate(QStringLiteral("safe"),
                                                  QStringLiteral("planner"),
                                                  QStringLiteral("p"), QString(), true);
    TestHarness::check(safeText == QStringLiteral("Teammate 'safe' spawned as planner without an initial Task. End this turn; the runtime will deliver its events."),
                       "FIND-E阳性: 非空句柄 → 成功文本逐字");
    TestHarness::check(mgr.hasTeammate(QStringLiteral("safe")), "FIND-E阳性: 名册登记可达");
    TestHarness::check(mgr.planGate(QStringLiteral("safe")) == AgentTeamsManager::PlanGate::Required,
                       "FIND-E阳性: requirePlan=true 出生牌不受回滚分支影响");

    // ————— FIND-E×FIND-M 联合：回滚退租磁盘失败 → 内存照清 + 诊断面留痕 —————
    const QString t3 = newTask(store, QStringLiteral("doomed-job"));
    TestHarness::check(!t3.isEmpty(), "FIND-M夹具: t3 创建成功");
    const QString t3File = QDir(QDir(root).filePath(QStringLiteral(".task")))
                               .filePath(t3 + QStringLiteral(".json"));
    mgr.setTeammateLauncher([t3File](const QString &, const QString &, const QString &,
                                     const QString &, bool) -> TeammateRuntime * {
        // launcher 模拟「认领已成、随后磁盘坏」：认领写盘之后才截断成非法 JSON
        // （writeRawTaskFile 同款裸字节形），令回滚的 releaseTeammateAssignment
        // 全盘扫描读 t3 不可读 → false + 错误文本。
        QFile broken(t3File);
        if (broken.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            broken.write("{ broken mid-write");
            broken.close();
        }
        return nullptr;
    });
    const QString doomerText = mgr.runSpawnTeammate(QStringLiteral("doomer"),
                                                    QStringLiteral("coder"),
                                                    QStringLiteral("p"), t3, false);
    TestHarness::check(doomerText == QStringLiteral("Error: Teammate runtime failed to start for 'doomer'"),
                       "FIND-E: 磁盘清理失败仍折叠同一报错（不静默、不炸穿）");
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("doomer")), "FIND-E: 磁盘坏不挡弹账");
    TestHarness::check(!store.leaseFor(QStringLiteral("doomer")).has_value(),
                       "FIND-E: TaskStore finally 形态——盘不可读内存租约照样出清");
    TestHarness::check(!mgr.lastReleaseWarning().isEmpty(),
                       "FIND-M: 回滚退租失败记入诊断面（非空）");
}

// ————————————————————————————————————————————————————————————————
// 组2：list_teammates 形态（lcc :696-704）+ 状态/门名单源命名
// ————————————————————————————————————————————————————————————————
void testListTeammatesAndNames()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    TestHarness::check(mgr.runListTeammates() == QStringLiteral("No active teammates."),
                       "list: 空名册逐字");

    mgr.runSpawnTeammate(QStringLiteral("zoe"), QStringLiteral("p"), QStringLiteral("p"), QString(), false);
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("p"), QStringLiteral("p"), QString(), false);
    mgr.setTeammateStatus(QStringLiteral("bob"), AgentTeamsManager::TeammateStatus::Idle);
    TestHarness::check(mgr.runListTeammates() == QStringLiteral("bob: idle\nzoe: working"),
                       "list: 按名字典序逐字行形");
    TestHarness::check(mgr.teammateNames() == QStringList{QStringLiteral("bob"), QStringLiteral("zoe")},
                       "list: teammateNames 排序同序");

    TestHarness::check(AgentTeamsManager::statusName(AgentTeamsManager::TeammateStatus::Working) == QStringLiteral("working"),
                       "statusName(working)");
    TestHarness::check(AgentTeamsManager::statusName(AgentTeamsManager::TeammateStatus::WaitingApproval) == QStringLiteral("waiting_approval"),
                       "statusName(waiting_approval)");
    TestHarness::check(AgentTeamsManager::statusName(AgentTeamsManager::TeammateStatus::Stopping) == QStringLiteral("stopping"),
                       "statusName(stopping)");
    TestHarness::check(AgentTeamsManager::gateName(AgentTeamsManager::PlanGate::NotRequired) == QStringLiteral("not_required"),
                       "gateName(not_required)");
    TestHarness::check(AgentTeamsManager::gateName(AgentTeamsManager::PlanGate::Rejected) == QStringLiteral("rejected"),
                       "gateName(rejected)");
}

// ————————————————————————————————————————————————————————————————
// 组3：send_message 双侧（Lead 壳 lcc :706-712 / 队友核 lcc :596-603）
// ————————————————————————————————————————————————————————————————
void testSendMessage()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("tester"), QStringLiteral("p"), QString(), false);

    TestHarness::check(mgr.runSendMessage(QStringLiteral("ghost"), QStringLiteral("hi"))
                           == QStringLiteral("Teammate 'ghost' is not active"),
                       "send(Lead): 不在册拒绝逐字");
    TestHarness::check(mgr.runSendMessage(QStringLiteral("bob"), QStringLiteral("hi"))
                           == QStringLiteral("Sent to bob"),
                       "send(Lead): 在册成功");
    const QVector<BusMessage> bobBox = bus.drain(QStringLiteral("bob"));
    TestHarness::check(bobBox.size() == 1 && bobBox.at(0).from == QStringLiteral("lead")
                           && bobBox.at(0).content == QStringLiteral("hi")
                           && bobBox.at(0).type == QStringLiteral("message"),
                       "send(Lead): 落 bob 信箱且 type=message");

    TestHarness::check(mgr.sendTeammateMessage(QStringLiteral("bob"), QStringLiteral("ghost"), QStringLiteral("yo"))
                           == QStringLiteral("Agent 'ghost' is not active"),
                       "send(队友): 目标既非 lead 又不在册拒绝");
    TestHarness::check(mgr.sendTeammateMessage(QStringLiteral("bob"), QStringLiteral("lead"), QStringLiteral("yo"))
                           == QStringLiteral("Sent to lead"),
                       "send(队友): 发 lead 恒可");
    const QVector<BusMessage> leadBox = mgr.consumeLeadInbox();
    TestHarness::check(leadBox.size() == 1 && leadBox.at(0).from == QStringLiteral("bob")
                           && leadBox.at(0).type == QStringLiteral("message"),
                       "send(队友): lead 信箱收信（原样返回不吞）");
    TestHarness::check(mgr.sendTeammateMessage(QStringLiteral("bob"), QStringLiteral("bob"), QStringLiteral("yo"))
                           == QStringLiteral("Sent to bob"),
                       "send(队友): 发在册队友放行");
}

// ————————————————————————————————————————————————————————————————
// 组4：submitPlan 案卷/信箱/重提拒（lcc :462-493）
// ————————————————————————————————————————————————————————————————
void testSubmitPlanLedger()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    const QString t1 = newTask(store, QStringLiteral("first-job"));
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("coder"), QStringLiteral("p"), t1, true);
    const QString rid = captureReqId(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("step 1")));
    TestHarness::check(!rid.isEmpty(), "submit: 案号 req_\\d{6} 抓到");
    int curVersion = -99;
    QString curTask;
    mgr.currentWorkIdentity(QStringLiteral("bob"), &curVersion, &curTask);
    TestHarness::check(curVersion == 1 && curTask == t1, "submit: 工作身份=(版本1,在员工单)");
    const AgentTeamsManager::ProtocolState *st = mgr.protocolState(rid);
    TestHarness::check(st != nullptr && st->type == QStringLiteral("plan_approval")
                           && st->sender == QStringLiteral("bob")
                           && st->target == QStringLiteral("lead")
                           && st->status == kPending
                           && st->payload == QStringLiteral("step 1")
                           && st->workVersion == 1 && st->taskId == t1,
                       "submit: 案卷十字段快照（含审批覆盖的工作身份）");
    TestHarness::check(mgr.planGate(QStringLiteral("bob")) == AgentTeamsManager::PlanGate::Pending,
                       "submit: 门翻 pending");
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("bob")) == rid,
                       "submit: 案号登记为在审当前案");
    const std::optional<AgentTeamsManager::TeammateStatus> stt = mgr.teammateStatus(QStringLiteral("bob"));
    TestHarness::check(stt.has_value() && *stt == AgentTeamsManager::TeammateStatus::WaitingApproval,
                       "submit: 状态转 waiting_approval");
    TestHarness::check(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("step 2"))
                           == QStringLiteral("A plan is already waiting for review."),
                       "submit: pending 期间重提拒（一人一宗在审案）");
    TestHarness::check(mgr.runReviewPlan(QStringLiteral("req_424242"), true, QString())
                           == QStringLiteral("Request req_424242 not found"),
                       "review夹具: 未知案号逐字（顺带钉 review 首门文本）");
    // bus.send 失败全量回滚矩阵经公开 API 不可达（lead 恒为合法收件名，路径校验必过）——
    // 已注释登记。lcc parity 观察项：submit 不查队友是否在册（字典默认语义放行）。
    const QString ghostRid =
        captureReqId(mgr.submitPlan(QStringLiteral("ghost-owner"), QStringLiteral("p")));
    TestHarness::check(!ghostRid.isEmpty(),
                       "submit: 未 spawn 名字放行（lcc 字典默认语义不拦截=登记观察项）");
    TestHarness::check(mgr.submitPlan(QStringLiteral("ghost-owner"), QStringLiteral("p2"))
                           == QStringLiteral("A plan is already waiting for review."),
                       "submit: ghost 在审期重提同拒（一人一宗对 ghost 同样生效）");
}

// ————————————————————————————————————————————————————————————————
// 组5：applyPlanResponse 十一条件合取（lcc :533-568）
// —————————————————————————————————————————————————————————————──
void testApplyPlanResponseGates()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    // 正向旅程：submit → Lead review（只写案卷）→ 队友侧 apply（门唯一翻面点）。
    const QString t1 = newTask(store, QStringLiteral("job-one"));
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("coder"), QStringLiteral("p"), t1, true);
    const QString ridA = captureReqId(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("plan")));
    TestHarness::check(mgr.runReviewPlan(ridA, true, QString())
                           == QStringLiteral("Plan approved (%1)").arg(ridA),
                       "review: 成功返回文本（案卷翻 approved）");
    TestHarness::check(mgr.planGate(QStringLiteral("bob")) == AgentTeamsManager::PlanGate::Pending,
                       "review: Lead 侧不翻队友门（钉死第7条，仍 pending）");
    const BusMessage good = mkMsg(QStringLiteral("lead"), QStringLiteral("bob"),
                                  QStringLiteral("Plan approved."),
                                  QStringLiteral("plan_approval_response"), ridA, true);
    QString out;
    TestHarness::check(mgr.applyPlanResponse(QStringLiteral("bob"), good, &out),
                       "apply: 十一门全合取放行");
    TestHarness::check(out == QStringLiteral("[Plan approved] Plan approved."),
                       "apply: 回执文本=[Plan <status>] <content>");
    TestHarness::check(mgr.planGate(QStringLiteral("bob")) == AgentTeamsManager::PlanGate::Approved,
                       "apply: 门由队友侧翻 approved（台账权威非信件）");
    TestHarness::check(mgr.teammateStatus(QStringLiteral("bob")).value()
                           == AgentTeamsManager::TeammateStatus::Working,
                       "apply: 状态回 working");
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("bob")).isEmpty(),
                       "apply: 案号出清");
    TestHarness::check(!mgr.applyPlanResponse(QStringLiteral("bob"), good, &out),
                       "apply重放: 拒绝（案号已清=反重放）");
    TestHarness::check(out == QStringLiteral("[Ignored plan response: request mismatch]"),
                       "apply重放: 忽略回执逐字");

    // 违规矩阵（每条独立拒绝，门不动）。
    bool rejectedAll = true;
    rejectedAll = rejectedAll && !mgr.applyPlanResponse(QStringLiteral("bob"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), QString(), true), &out);
    rejectedAll = rejectedAll && !mgr.applyPlanResponse(QStringLiteral("bob"),
        mkMsg(QStringLiteral("eve"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), ridA, true), &out);
    rejectedAll = rejectedAll && !mgr.applyPlanResponse(QStringLiteral("bob"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("zoe"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), ridA, true), &out);
    rejectedAll = rejectedAll && !mgr.applyPlanResponse(QStringLiteral("bob"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), QStringLiteral("req_999999"), true), &out);
    TestHarness::check(rejectedAll && out == QStringLiteral("[Ignored plan response: request mismatch]"),
                       "apply违规: 空案号/伪发件人/错收件/未知案号 全拒同回执");
    TestHarness::check(mgr.planGate(QStringLiteral("bob")) == AgentTeamsManager::PlanGate::Approved,
                       "apply违规: 门不被违规信扰动");

    // 案卷 pending 未决拒（审前 apply）+ approve 旗标不符拒（审后反向）。
    const QString t2 = newTask(store, QStringLiteral("job-two"));
    mgr.runSpawnTeammate(QStringLiteral("dave"), QStringLiteral("coder"), QStringLiteral("p"), t2, true);
    const QString ridD = captureReqId(mgr.submitPlan(QStringLiteral("dave"), QStringLiteral("plan")));
    TestHarness::check(!mgr.applyPlanResponse(QStringLiteral("dave"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("dave"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), ridD, true), &out),
                       "apply: 案卷 pending 未决即拒（status 必须终态）");
    TestHarness::check(mgr.runReviewPlan(ridD, false, QString())
                           == QStringLiteral("Plan rejected (%1)").arg(ridD),
                       "review: 拒批翻案卷 rejected");
    TestHarness::check(!mgr.applyPlanResponse(QStringLiteral("dave"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("dave"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), ridD, true), &out),
                       "apply: approve 旗标与案卷终态不符 → 拒（第11门）");
    TestHarness::check(mgr.applyPlanResponse(QStringLiteral("dave"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("dave"), QStringLiteral("no"),
              QStringLiteral("plan_approval_response"), ridD, false), &out)
                           && out == QStringLiteral("[Plan rejected] no"),
                       "apply: 旗标相符放行（[Plan rejected] 回执）");
    TestHarness::check(mgr.planGate(QStringLiteral("dave")) == AgentTeamsManager::PlanGate::Rejected,
                       "apply: 门翻 rejected");
    const QString ridD2 = captureReqId(mgr.submitPlan(QStringLiteral("dave"), QStringLiteral("plan v2")));
    TestHarness::check(!ridD2.isEmpty() && ridD2 != ridD,
                       "apply: rejected 后允许重提新案（一人一宗仅约束 pending）");

    // 复合陈旧（⑨+⑩）：批准旅程后换工 → 快照身份失配拒、门不动。
    QString err;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("bob"), QStringLiteral("complete_task"), idArgs(t1))
                           .startsWith(QStringLiteral("Completed ")),
                       "陈旧夹具: bob 完工 job-one");
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("bob"), &err),
                       "陈旧夹具: 回合边界退租成功");
    const QString t3 = newTask(store, QStringLiteral("job-three"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t3), QStringLiteral("bob"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "陈旧夹具: bob 二次认领换工（版本自增）");
    TestHarness::check(!mgr.applyPlanResponse(QStringLiteral("bob"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("plan_approval_response"), ridA, true), &out),
                       "apply: 陈旧案卷（版本+工单双失配）拒绝（⑨⑩复合）");
}

// ————————————————————————————————————————————————————————————————
// 组6：applyShutdownRequest 八条件 + request_shutdown 旅程（lcc :570-594/:714-731）
// —————————————————————————————————————————————————————————————──
void testShutdownJourney()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("tester"), QStringLiteral("p"), QString(), false);

    TestHarness::check(mgr.runRequestShutdown(QStringLiteral("ghost"))
                           == QStringLiteral("Teammate 'ghost' is not active"),
                       "shutdown: 不在册拒绝逐字");
    const QString text = mgr.runRequestShutdown(QStringLiteral("bob"));
    const QString rid = captureReqId(text);
    TestHarness::check(!rid.isEmpty() && text == QStringLiteral("Shutdown requested from bob (%1)").arg(rid),
                       "shutdown: 请求文本逐字");
    const AgentTeamsManager::ProtocolState *st = mgr.protocolState(rid);
    TestHarness::check(st != nullptr && st->type == QStringLiteral("shutdown")
                           && st->sender == QStringLiteral("lead")
                           && st->target == QStringLiteral("bob")
                           && st->status == kPending
                           && st->workVersion == -1,
                       "shutdown: 案卷方向 lead→队友（shutdown 不快照工作身份，workVersion=-1≡None）");
    const QVector<BusMessage> box = bus.drain(QStringLiteral("bob"));
    TestHarness::check(box.size() == 1 && box.at(0).type == QStringLiteral("shutdown_request")
                           && box.at(0).content == QStringLiteral("Finish the current step and shut down.")
                           && box.at(0).metadata.value(QStringLiteral("request_id")).toString() == rid,
                       "shutdown: 信箱信体逐字（承重句 + request_id）");

    QString out;
    BusMessage req = box.isEmpty() ? BusMessage() : box.at(0);
    TestHarness::check(req.type == QStringLiteral("shutdown_request"), "shutdown夹具: 收信回读");
    TestHarness::check(mgr.applyShutdownRequest(QStringLiteral("bob"), req, &out),
                       "shutdown apply: 八门放行（msg.type 被忽略=lcc :570-594 parity）");
    TestHarness::check(out == rid, "shutdown apply: 回执=案号（供 ack 元数据）");
    TestHarness::check(mgr.teammateStatus(QStringLiteral("bob")).value()
                           == AgentTeamsManager::TeammateStatus::Stopping,
                       "shutdown apply: 状态转 stopping");
    TestHarness::check(!mgr.applyShutdownRequest(QStringLiteral("bob"), req, &out)
                           && out == QStringLiteral("[Ignored shutdown request: request mismatch]"),
                        "shutdown apply: 二次拒（八门⑧: Stopping 再拒；案卷仍 pending 故仅条件⑧所致）");

    // 违规矩阵：空案号/错发件/错收件/未知案号/plan 案卷冒充。
    bool rejectedAll = true;
    rejectedAll = rejectedAll && !mgr.applyShutdownRequest(QStringLiteral("bob"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("shutdown_request"), QString(), true), &out);
    rejectedAll = rejectedAll && !mgr.applyShutdownRequest(QStringLiteral("bob"),
        mkMsg(QStringLiteral("eve"), QStringLiteral("bob"), QStringLiteral("x"),
              QStringLiteral("shutdown_request"), rid, true), &out);
    rejectedAll = rejectedAll && !mgr.applyShutdownRequest(QStringLiteral("eve"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("eve"), QStringLiteral("x"),
              QStringLiteral("shutdown_request"), QStringLiteral("req_123456"), true), &out);
    mgr.runSpawnTeammate(QStringLiteral("dave"), QStringLiteral("t"), QStringLiteral("p"), QString(), true);
    const QString ridP = captureReqId(mgr.submitPlan(QStringLiteral("dave"), QStringLiteral("plan")));
    rejectedAll = rejectedAll && !mgr.applyShutdownRequest(QStringLiteral("dave"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("dave"), QStringLiteral("x"),
              QStringLiteral("shutdown_request"), ridP, true), &out);
    TestHarness::check(rejectedAll && out == QStringLiteral("[Ignored shutdown request: request mismatch]"),
                       "shutdown违规: 全矩阵同回执拒绝（第5门 type!=shutdown 拦截 plan 案卷）");

    // ack 经 consume 翻案卷（matchResponse 与 shutdown 的闭环，串组8）。
    QJsonObject ackMeta;
    ackMeta.insert(QStringLiteral("request_id"), rid);
    ackMeta.insert(QStringLiteral("approve"), true);
    TestHarness::check(bus.send(QStringLiteral("bob"), QStringLiteral("lead"),
                                QStringLiteral("Shutdown acknowledged."),
                                QStringLiteral("shutdown_response"), ackMeta),
                       "shutdown ack: 入 lead 信箱");
    mgr.consumeLeadInbox();
    TestHarness::check(st->status == QStringLiteral("approved"),
                       "shutdown ack: consume 后案卷 approved（matchResponse 唯一翻案卷者）");
}

// ————————————————————————————————————————————————————————————————
// 组7：review_plan 五校验串逐字 + 回执信箱（lcc :743-771）
// —————————————————————————————————————————————————————————————──
void testReviewPlanFiveGates()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);
    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("t"), QStringLiteral("p"), QString(), false);

    TestHarness::check(mgr.runRequestShutdown(QStringLiteral("bob")).startsWith(
                           QStringLiteral("Shutdown requested from bob (")),
                       "review夹具: shutdown 案卷备用");
    // 用真实案卷驱动五门：先 shutdown 案卷冒充 plan（门2），再 plan 案卷走门3/4/5。
    // 取 bob 名下任一 shutdown 案卷号：
    QString shutRid;
    const QString t1 = newTask(store, QStringLiteral("sub-job"));
    mgr.runSpawnTeammate(QStringLiteral("dave"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString ridD = captureReqId(mgr.submitPlan(QStringLiteral("dave"), QStringLiteral("plan")));
    TestHarness::check(!ridD.isEmpty(), "review夹具: dave plan 案卷");

    // 门1 未知：
    TestHarness::check(mgr.runReviewPlan(QStringLiteral("req_999999"), true, QString())
                           == QStringLiteral("Request req_999999 not found"),
                       "review门1: 未知案号逐字");
    // 门2 类型：shutdown 案卷（bob 名下）冒充。
    const QString shutText = mgr.runRequestShutdown(QStringLiteral("bob"));
    shutRid = captureReqId(shutText);
    TestHarness::check(!shutRid.isEmpty(), "review夹具: bob shutdown 案号");
    TestHarness::check(mgr.runReviewPlan(shutRid, true, QString())
                           == QStringLiteral("Request %1 is not a plan").arg(shutRid),
                       "review门2: 非 plan 案卷逐字");
    // 门3 已决：对 dave plan 双审。
    TestHarness::check(mgr.runReviewPlan(ridD, true, QString())
                           == QStringLiteral("Plan approved (%1)").arg(ridD),
                       "review门3前: 首次审通过");
    TestHarness::check(mgr.runReviewPlan(ridD, false, QString())
                           == QStringLiteral("Request %1 already approved").arg(ridD),
                       "review门3: 已决二次审逐字（含终态名）");
    TestHarness::check(mgr.planGate(QStringLiteral("dave")) == AgentTeamsManager::PlanGate::Pending,
                       "review: Lead 只写案卷不翻队友门（审后仍 pending）");

    // 门4 身份陈旧：leo 未认领 submit → 再认领 → 版本+工单双变。
    mgr.runSpawnTeammate(QStringLiteral("leo"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString ridL = captureReqId(mgr.submitPlan(QStringLiteral("leo"), QStringLiteral("lp")));
    const QString t9 = newTask(store, QStringLiteral("t9-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t9), QStringLiteral("leo"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "门4夹具: leo 认领致换工");
    TestHarness::check(mgr.runReviewPlan(ridL, true, QString())
                           == QStringLiteral("Request %1 belongs to an earlier assignment").arg(ridL),
                       "review门4: 早前工单归属逐字（陈旧审批自动作废）");

    // 门5 非当前案：wren submit ridA → finalize（清案号）→ 重 submit ridB → 审 ridA。
    const QString tW = newTask(store, QStringLiteral("wren-job"));
    mgr.runSpawnTeammate(QStringLiteral("wren"), QStringLiteral("c"), QStringLiteral("p"), tW, true);
    const QString ridA = captureReqId(mgr.submitPlan(QStringLiteral("wren"), QStringLiteral("pa")));
    mgr.finalizeTeammate(QStringLiteral("wren"));
    const QString ridB = captureReqId(mgr.submitPlan(QStringLiteral("wren"), QStringLiteral("pb")));
    TestHarness::check(!ridB.isEmpty() && ridB != ridA, "门5夹具: 重提新案成功");
    TestHarness::check(mgr.runReviewPlan(ridA, true, QString())
                           == QStringLiteral("Request %1 is not the current plan").arg(ridA),
                       "review门5: 非当前案逐字（身份未变仅案号出清）");

    // 回执信箱：feedback 原文透传 + approve 旗标落 metadata。
    const QString ridC = captureReqId(mgr.submitPlan(QStringLiteral("dave"), QStringLiteral("pc")));
    TestHarness::check(ridC.isEmpty(), "回执夹具: dave pending 期重提被拒（ridC 应为空）");
    // 上一案 ridD 已被双审消耗（approved 终态，案号未清——apply 才清）：直接换新队友。
    mgr.runSpawnTeammate(QStringLiteral("fay"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString ridF = captureReqId(mgr.submitPlan(QStringLiteral("fay"), QStringLiteral("pf")));
    TestHarness::check(mgr.runReviewPlan(ridF, false, QStringLiteral("do it again"))
                           == QStringLiteral("Plan rejected (%1)").arg(ridF),
                       "回执: 带 feedback 的拒批返回逐字");
    const QVector<BusMessage> fayBox = bus.drain(QStringLiteral("fay"));
    TestHarness::check(fayBox.size() == 1 && fayBox.at(0).content == QStringLiteral("do it again")
                           && fayBox.at(0).metadata.value(QStringLiteral("approve")).toBool() == false
                           && fayBox.at(0).metadata.value(QStringLiteral("request_id")).toString() == ridF
                           && fayBox.at(0).type == QStringLiteral("plan_approval_response"),
                       "回执: 信箱信体=feedback 原文 + request_id/approve 元数据");
    TestHarness::check(mgr.protocolState(ridF)->status == QStringLiteral("rejected"),
                       "回执: 案卷终态 rejected（Lead 侧唯一写点）");
}

// ————————————————————————————————————————————————————————————————
// 组8：matchResponse 四门（经 consumeLeadInbox 驱动，lcc :391-422）
// —————————————————————————————————————————————————————————————──
void testMatchResponseGates()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("t"), QStringLiteral("p"), QString(), true);
    const QString rid = captureReqId(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("plan")));
    const AgentTeamsManager::ProtocolState *st = mgr.protocolState(rid);
    TestHarness::check(st != nullptr && st->status == kPending, "match夹具: plan 案卷 pending");

    // 先清掉 submit 落在 lead 信箱的请求信，再按批断言。
    const QVector<BusMessage> pre = mgr.consumeLeadInbox();
    TestHarness::check(pre.size() == 1, "match夹具: 先清掉 submit 的请求信");
    // 幻觉案号：不翻任何东西、原样返回。
    TestHarness::check(bus.send(QStringLiteral("lead"), QStringLiteral("lead"), QStringLiteral("x"),
                                QStringLiteral("plan_approval_response"),
                                QJsonObject{{QStringLiteral("request_id"), QStringLiteral("req_888888")},
                                           {QStringLiteral("approve"), true}}),
                       "match夹具: 幻觉案号信入 lead 箱");
    QVector<BusMessage> batch = mgr.consumeLeadInbox();
    TestHarness::check(batch.size() == 1 && st->status == kPending,
                       "match门1: 未知案号不伤案卷且原始批返回");

    // 期望类型不符：plan 案卷遇 shutdown_response。
    QJsonObject meta;
    meta.insert(QStringLiteral("request_id"), rid);
    meta.insert(QStringLiteral("approve"), true);
    TestHarness::check(bus.send(QStringLiteral("bob"), QStringLiteral("lead"), QStringLiteral("x"),
                                QStringLiteral("shutdown_response"), meta),
                       "match夹具: 类型错配信入箱（bob 持 mirror 正确发件人）");
    batch = mgr.consumeLeadInbox();
    TestHarness::check(batch.size() == 1 && st->status == kPending,
                       "match门2: 应答类型与案卷期望不符 → 不翻（incoming 不自证类型）");

    // 镜像身份不符：lead→lead（应 bob→lead… plan 案卷 mirror 要求 from==target(lead) && to==sender(bob)，
    // 而信必须躺在 lead 信箱 → from 必须是他人：构造 from=lead,to=lead 即 to!=sender 失配）。
    TestHarness::check(bus.send(QStringLiteral("lead"), QStringLiteral("lead"), QStringLiteral("x"),
                                QStringLiteral("plan_approval_response"), meta),
                       "match夹具: 镜像错置信入箱");
    batch = mgr.consumeLeadInbox();
    TestHarness::check(batch.size() == 1 && st->status == kPending,
                       "match门3: 镜像身份不符 → 不翻");

    // 正例：mirror 正确（from==target lead, to==sender bob——但收件箱必须是 lead！
    // lcc 同款悖论在 lite 复现：consume 只读 lead 箱，to 字段却要求 ==sender。
    // plan 应答的真实通路是 review_plan→applyPlanResponse（队友侧读自己信箱），
    // matchResponse 对 plan 案卷仅在 from=lead,to=bob 且此信位于 lead 箱时才翻——
    // 该形态只有手工投递可达。用 send 的 'to' 仅做路径校验、正文照落 lead 箱：
    // bus.send(from="lead", to="lead") 落 lead 箱但 to==lead≠bob → 不可达正例。
    // 结论：plan_approval_response 经 consume 翻案卷在 lcc 同样不可达（其正例通路
    // 是 shutdown 方向：from=teammate(=target) to=lead(=sender) 天然镜像）。
    // 因此门4（一次性反重放）以 shutdown 案卷验证。
    const QString shutRid = captureReqId(mgr.runRequestShutdown(QStringLiteral("bob")));
    QJsonObject ackMeta;
    ackMeta.insert(QStringLiteral("request_id"), shutRid);
    ackMeta.insert(QStringLiteral("approve"), true);
    TestHarness::check(bus.send(QStringLiteral("bob"), QStringLiteral("lead"),
                                QStringLiteral("Shutdown acknowledged."),
                                QStringLiteral("shutdown_response"), ackMeta),
                       "match门4夹具: shutdown ack 入 lead 箱");
    batch = mgr.consumeLeadInbox();
    const AgentTeamsManager::ProtocolState *shutSt = mgr.protocolState(shutRid);
    TestHarness::check(shutSt != nullptr && shutSt->status == QStringLiteral("approved"),
                       "match正例: shutdown 方向镜像全合 → 案卷 approved");
    // 重放（status 非 pending）→ 不翻不回退：
    TestHarness::check(bus.send(QStringLiteral("bob"), QStringLiteral("lead"),
                                QStringLiteral("again"),
                                QStringLiteral("shutdown_response"),
                                QJsonObject{{QStringLiteral("request_id"), shutRid},
                                           {QStringLiteral("approve"), false}}),
                       "match门4夹具: 重放反旗标信入箱");
    mgr.consumeLeadInbox();
    TestHarness::check(shutSt->status == QStringLiteral("approved"),
                       "match门4: 一次性反重放（重放不改写、不回退案卷）");
    TestHarness::check(st->status == kPending, "match: plan 案卷全程未被波及");
}

// ————————————————————————————————————————————————————————————————
// 组9：runTeammateTool 三门 + read/glob 恒过 + 权限硬拒 + hook 记账 + cwd 现读
// （lcc :495-531 + fix-4 钉死第3条）
// —————————————————————————————————————————————————————————————──
void testTeammateToolGatesAndCwd()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);
    mgr.runSpawnTeammate(QStringLiteral("eve"), QStringLiteral("c"), QStringLiteral("p"), QString(), false);

    Recorder rec;
    mgr.setToolAdapter(ToolNames::READ_FILE, [&](const QJsonObject &, const QString &cwd) {
        ++rec.readCalls;
        rec.readCwd = cwd;
        return QStringLiteral("READ-OK");
    });
    mgr.setToolAdapter(ToolNames::BASH, [&](const QJsonObject &, const QString &cwd) {
        ++rec.bashCalls;
        rec.bashCwd = cwd;
        return QStringLiteral("BASH-OK");
    });
    mgr.setToolAdapter(ToolNames::WRITE_FILE, [&](const QJsonObject &, const QString &) {
        ++rec.writeCalls;
        return QStringLiteral("WRITE-OK");
    });
    mgr.setPermissionCheck([&](const QString &toolName, const QJsonObject &) {
        ++rec.permissionCalls;
        return toolName == ToolNames::BASH ? QStringLiteral("Permission required: no way")
                                           : QString();
    });
    mgr.setHooksTrigger([&](const QString &evt, const QString &toolName,
                            const QJsonObject &, const QString &output) {
        rec.hookCalls << evt + QLatin1Char('|') + toolName;
        if (evt == QStringLiteral("PreToolUse") && rec.preBlockFor == toolName) {
            return QStringLiteral("HOOK-BLOCK");
        }
        if (evt == QStringLiteral("PostToolUse")) {
            rec.hookCalls.last() += QLatin1Char('=') + output; // 记账输出，返回值被忽略
        }
        return QString();
    });

    QJsonObject pathArgs;
    pathArgs.insert(QStringLiteral("path"), QStringLiteral("a.txt"));
    // 无租约 read_file：adapter 分支 cwd 现读失败 fail-closed（偏C 折叠 + TaskStore 分支②文本）。
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("Error: Invalid task assignment: No active assignment for eve"),
                       "cwd现读: 无租约拦截（门③之前、error 通道折叠逐字）");
    TestHarness::check(rec.readCalls == 0 && rec.permissionCalls == 0,
                       "cwd现读: 拦截时 adapter/permission 均未触达");

    const QString t1 = newTask(store, QStringLiteral("eve-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t1), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "工具夹具: eve 认领");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK"),
                       "read: 认领后放行");
    TestHarness::check(rec.readCwd == root && rec.readCalls == 1,
                       "read: cwd=回落链（sessionRoot sink）现读");

    // 写三件套：gate not_required 放行门但权限照问（lcc :495-531 缩进实证：权限恒查）。
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, pathArgs)
                           == QStringLiteral("Permission required: no way"),
                       "权限: not_required 下 bash 仍被权限硬拒（写三件套恒查）");
    TestHarness::check(rec.bashCalls == 0, "权限: 硬拒时 adapter 未触达");

    // Required 门：拦写不拦读、Blocked 先于权限。
    TestHarness::check(mgr.runRequestPlan(QStringLiteral("eve"), QStringLiteral("do x"))
                           == QStringLiteral("Plan requested from eve"),
                       "门夹具: request_plan 置 required");
    const int permBefore = rec.permissionCalls;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, pathArgs)
                           == QStringLiteral("Blocked: plan status is required. Submit or revise the plan and wait for approval before changing the workspace."),
                       "门required: bash 拦截文本逐字");
    TestHarness::check(rec.permissionCalls == permBefore, "门required: Blocked 先于权限检查");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK"),
                       "门required: read 恒过（只拦写三件套）");

    // pending 门同拦。
    mgr.submitPlan(QStringLiteral("eve"), QStringLiteral("my plan"));
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, pathArgs)
                           == QStringLiteral("Blocked: plan status is pending. Submit or revise the plan and wait for approval before changing the workspace."),
                       "门pending: bash 拦截文本逐字");

    // 批准旅程 → approved 后权限仍恒查（写三件套）。
    const QString rid = captureReqId(mgr.submitPlan(QStringLiteral("ghost2"), QStringLiteral("x")));
    (void)rid; // ghost2 未在册不可提交——改走 eve 的在审案：先 review+apply 之。
    const QString eveRid = mgr.currentPlanRequestId(QStringLiteral("eve"));
    TestHarness::check(!eveRid.isEmpty(), "批准夹具: eve 在审案号");
    mgr.runReviewPlan(eveRid, true, QString());
    QString apOut;
    TestHarness::check(mgr.applyPlanResponse(QStringLiteral("eve"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("eve"), QStringLiteral("ok"),
              QStringLiteral("plan_approval_response"), eveRid, true), &apOut)
                           && apOut == QStringLiteral("[Plan approved] ok"),
                       "批准夹具: eve 案卷 apply 放行（out 文案 [Plan approved] 内容 钉死）");
    TestHarness::check(mgr.planGate(QStringLiteral("eve")) == AgentTeamsManager::PlanGate::Approved,
                       "批准夹具: 门 approved");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, pathArgs)
                           == QStringLiteral("Permission required: no way"),
                       "权限: approved 后 bash 仍被权限硬拒（门与权限正交）");
    const int permBeforeClear = rec.permissionCalls;
    // 替换 lambda 必须继续计数——:864 断言「每次写工具调用恰一问」的可观测性依赖它；
    // 无捕获不计数版是首轮 FAIL「权限: 每次写工具调用恰一问」的根因（测试缺陷，非生产缺陷）。
    mgr.setPermissionCheck([&rec](const QString &, const QJsonObject &) {
        ++rec.permissionCalls;
        return QString();
    });
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, pathArgs)
                           == QStringLiteral("BASH-OK") && rec.bashCalls == 1,
                       "权限清空: bash 经 adapter 放行");
    TestHarness::check(rec.permissionCalls == permBeforeClear + 1, "权限: 每次写工具调用恰一问");

    // 未知工具：先于 hook 返回（lcc :518-520）。
    const int hooksBefore = rec.hookCalls.size();
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), QStringLiteral("zipzap"), pathArgs)
                           == QStringLiteral("Unknown tool: zipzap"),
                       "未知工具: 拒绝文本逐字");
    TestHarness::check(rec.hookCalls.size() == hooksBefore, "未知工具: hook 不触发（查无先于钩）");

    // PreToolUse 拦截；PostToolUse 记账返回值被忽略（偏C'）。
    rec.preBlockFor = ToolNames::WRITE_FILE;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::WRITE_FILE, pathArgs)
                           == QStringLiteral("HOOK-BLOCK"),
                       "hook: PreToolUse 非空拦截逐字");
    TestHarness::check(rec.writeCalls == 0, "hook: 拦截时 adapter 未触达");
    rec.preBlockFor.clear();
    rec.hookCalls.clear();
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK"),
                       "hook: 放行路径不受 Post 影响");
    TestHarness::check(rec.hookCalls.contains(QStringLiteral("PreToolUse|read_file"))
                           && rec.hookCalls.contains(QStringLiteral("PostToolUse|read_file=READ-OK")),
                       "hook: Pre/Post 各记一次、Post 携输出（返回值忽略=偏C'）");

    // 租约死亡后同工具再犯：cwd 现读再失败（钉死第3条禁缓存）。
    QString err;
    TestHarness::check(store.runCompleteTaskLeased(idArgs(t1), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Completed ")),
                       "现读夹具: eve 完工");
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("eve"), &err),
                       "现读夹具: 回合边界退租");
    const int readsBefore = rec.readCalls;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("Error: Invalid task assignment: No active assignment for eve"),
                       "cwd现读: 退租即失败（无陈旧缓存）");
    TestHarness::check(rec.readCalls == readsBefore, "cwd现读: 失败时 adapter 未触达");

    // worktree 供值：resolver 给出假路径 → adapter cwd 即该值 + 台账自愈回写。
    const QString t2 = newTask(store, QStringLiteral("wt-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t2), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "worktree夹具: eve 认领 t2");
    TestHarness::check(store.setWorktree(t2, QStringLiteral("wt_x"), &err),
                       "worktree夹具: 绑定名字 wt_x");
    int resolverCalls = 0;
    QString resolverSawWorktree;
    store.setCwdResolver([&](const TaskStore::TaskSnapshot &task, QString *e) {
        ++resolverCalls;
        resolverSawWorktree = task.worktree;
        if (task.worktree != QStringLiteral("wt_x")) {
            *e = QStringLiteral("unexpected worktree");
            return QString();
        }
        return QStringLiteral("C:/fake/wt_x");
    });
    const int versionBefore = store.assignmentVersion(QStringLiteral("eve"));
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK"),
                       "worktree供值: adapter 放行");
    TestHarness::check(resolverCalls == 1 && resolverSawWorktree == QStringLiteral("wt_x"),
                       "worktree供值: resolver 恰一次、快照 worktree=名字（M4 已绑定才进）");
    TestHarness::check(rec.readCwd == QStringLiteral("C:/fake/wt_x"),
                       "worktree供值: adapter cwd=resolver 返回值");
    const std::optional<TaskStore::Lease> healed = store.leaseFor(QStringLiteral("eve"));
    TestHarness::check(healed.has_value() && healed->cwd == QStringLiteral("C:/fake/wt_x"),
                       "worktree供值: 台账 cwd 自愈回写（现读盘重建）");
    TestHarness::check(store.assignmentVersion(QStringLiteral("eve")) == versionBefore,
                       "worktree供值: 自愈不 bump 版本（M2 钉死保护 fix-4）");

    // M4 早退：未绑定任务不进 resolver。
    QString err2;
    TestHarness::check(store.runCompleteTaskLeased(idArgs(t2), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Completed ")),
                       "早退夹具: eve 完工 t2");
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("eve"), &err2),
                       "早退夹具: 退租");
    const QString t3 = newTask(store, QStringLiteral("plain-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t3), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "早退夹具: eve 认领未绑定 t3");
    const int resBefore = resolverCalls;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK") && rec.readCwd == root,
                       "M4早退: 未绑定走回落链（cwd=root）");
    TestHarness::check(resolverCalls == resBefore, "M4早退: 未绑定任务不进 resolver");
}

// ————————————————————————————————————————————————————————————————
// 组10：释放三时点与回调接线（lcc :329-331/:356-368 + fix-4 钉死第1/4条）
// —————————————————————————————————————————————————————————————──
void testReleaseTripointsAndCallbacks()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    // (a) advanced：pending 中换工 → 门复位 required + 案号出清（钉死第1条唯一挂点）。
    mgr.runSpawnTeammate(QStringLiteral("leo"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString ridL = captureReqId(mgr.submitPlan(QStringLiteral("leo"), QStringLiteral("lp")));
    const QString tA = newTask(store, QStringLiteral("leo-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(tA), QStringLiteral("leo"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "advanced夹具: leo 认领换工");
    TestHarness::check(mgr.planGate(QStringLiteral("leo")) == AgentTeamsManager::PlanGate::Required,
                       "advanced: pending→required 复位（换工使在审案作废）");
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("leo")).isEmpty(),
                       "advanced: 案号出清");
    TestHarness::check(mgr.protocolState(ridL) != nullptr
                           && mgr.protocolState(ridL)->status == kPending,
                       "advanced: 旧案卷保留 pending（案卷不随复位销毁，D7 进程内）");

    // (b) planGateCheck 否决族 + released 门复位 + 偏4 版本不递增钉桩。
    TestHarness::check(store.runCompleteTaskLeased(idArgs(tA), QStringLiteral("leo"))
                           == QStringLiteral("Cannot complete while plan status is required"),
                       "planGateCheck: required 否决 complete（回执即 veto 文本）");
    const AgentTeamsManager::ProtocolState *stL = mgr.protocolState(ridL);
    TestHarness::check(stL != nullptr, "否决夹具: 旧案卷仍在");
    // 否决时案卷不受波及（leo 在审的是旧案 ridL——complete 被门否决后原样）。
    TestHarness::check(mgr.planGate(QStringLiteral("leo")) == AgentTeamsManager::PlanGate::Required,
                       "planGateCheck: 否决后门仍是 required");
    // 走批准：submit 新案 → review → apply（门 approved）→ complete → release。
    const QString ridL2 = captureReqId(mgr.submitPlan(QStringLiteral("leo"), QStringLiteral("lp2")));
    TestHarness::check(mgr.runReviewPlan(ridL2, true, QString())
                           == QStringLiteral("Plan approved (%1)").arg(ridL2),
                       "批准夹具: leo 新案审批");
    TestHarness::check(mgr.applyPlanResponse(QStringLiteral("leo"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("leo"), QStringLiteral("ok"),
              QStringLiteral("plan_approval_response"), ridL2, true), nullptr),
                       "批准夹具: leo apply 放行（out=nullptr 容错）");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("leo"), QStringLiteral("complete_task"), idArgs(tA))
                           .startsWith(QStringLiteral("Completed ")),
                       "批准放行: complete_task 经工具面成功");
    QString err;
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("leo"), &err),
                       "released夹具: 回合边界退租");
    TestHarness::check(mgr.planGate(QStringLiteral("leo")) == AgentTeamsManager::PlanGate::NotRequired,
                       "released: 门复位 not_required（lcc :365-368 唯一挂点）");
    TestHarness::check(store.assignmentVersion(QStringLiteral("leo")) == 1,
                       "released: 退租不递增版本（偏差④ lcc :411 有意偏离钉桩）");
    // 案号由 applyPlanResponse 出清（released 回调从不碰 planRequestIds——另以下方 sarah 轨迹隔离钉桩）。
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("leo")).isEmpty(),
                       "apply 出清: 案号空（apply 是唯一出清点，released 不动案号账）");

    // (c) finalize 弹四本账、案卷保留（D7）。
    mgr.runSpawnTeammate(QStringLiteral("finn"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString ridF = captureReqId(mgr.submitPlan(QStringLiteral("finn"), QStringLiteral("fp")));
    mgr.finalizeTeammate(QStringLiteral("finn"));
    TestHarness::check(!mgr.hasTeammate(QStringLiteral("finn")), "finalize: 名册弹出");
    TestHarness::check(mgr.planGate(QStringLiteral("finn")) == AgentTeamsManager::PlanGate::NotRequired,
                       "finalize: 门账弹出（缺省语义）");
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("finn")).isEmpty(),
                       "finalize: 案号账弹出");
    TestHarness::check(mgr.protocolState(ridF) != nullptr,
                       "finalize: 案卷保留（pendingRequests 不弹=lcc 同语义，随进程 D7 消亡）");

    // (d) 死亡清算：releaseTeammateAssignment 降级遗留 in_progress + 门复位。
    const QString tG = newTask(store, QStringLiteral("gem-job"));
    mgr.runSpawnTeammate(QStringLiteral("gem"), QStringLiteral("c"), QStringLiteral("p"), tG, false);
    QString err2;
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("gem"), &err2),
                       "死亡清算: 退租成功");
    TestHarness::check(!store.leaseFor(QStringLiteral("gem")).has_value(),
                       "死亡清算: 租约出清");
    QVector<TaskStore::TaskSnapshot> snaps;
    TestHarness::check(store.listTaskSnapshots(&snaps, &err2), "死亡清算: 快照可读");
    bool degraded = false;
    for (const TaskStore::TaskSnapshot &s : snaps) {
        if (s.id == tG) {
            degraded = (s.status == kPending && s.owner.isEmpty());
        }
    }
    TestHarness::check(degraded, "死亡清算: 遗留 in_progress 打回 pending+无主");
    TestHarness::check(mgr.planGate(QStringLiteral("gem")) == AgentTeamsManager::PlanGate::NotRequired,
                       "死亡清算: released 回调复位门（两释放点同挂 fix-4 钉死第1条）");

    // (e) 幽灵 owner 幂等（TaskStore 语义交叉钉）。
    QString err3;
    TestHarness::check(!store.releaseCompletedAssignment(QStringLiteral("ghost"), &err3),
                       "幽灵: releaseCompleted false（无租约）");
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("ghost"), &err3),
                       "幽灵: releaseTeammate true 幂等");

    // (f) lcc parity 钉桩：released 回调只复位门、不清 planRequestIds（sarah 轨迹隔离观察）。
    mgr.runSpawnTeammate(QStringLiteral("sarah"), QStringLiteral("c"), QStringLiteral("p"), QString(), false);
    mgr.submitPlan(QStringLiteral("sarah"), QStringLiteral("sp"));
    const QString ridS = mgr.currentPlanRequestId(QStringLiteral("sarah"));
    TestHarness::check(!ridS.isEmpty(), "sarah夹具: submit 写入案号");
    QString err4;
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("sarah"), &err4),
                       "sarah: 无租约幂等退租 true（finally 无条件）");
    TestHarness::check(mgr.planGate(QStringLiteral("sarah")) == AgentTeamsManager::PlanGate::NotRequired
                            && store.assignmentVersion(QStringLiteral("sarah")) == 0
                            && mgr.currentPlanRequestId(QStringLiteral("sarah")) == ridS,
                        "released 不清案号（lcc :365-368 parity 钉桩：门复位/版本不动/案号残留）");

    // (g) Gate② FIND-D 对齐钉桩：换工（advanced 回调）**无条件**清在审案号——
    // lcc :363 的 planRequestIds.pop 长在 if 外。sarah 此刻门=NotRequired（f 步已复位）
    // + 案号 ridS 残留：旧 lite 实现（remove 圈在发牌 if 内）对此形态跳过清除、
    // currentPlanRequestId 恒读到 lcc 不存在的陈旧值——本断言在该实现下必红，
    // 修复后才有绿（合法期望变更，行为对齐 lcc，非弱化）。
    const QString tH = newTask(store, QStringLiteral("sarah-next"));
    TestHarness::check(!store.leaseFor(QStringLiteral("sarah")).has_value(),
                       "FIND-D夹具: sarah 无租约（可领新单）");
    TestHarness::check(store.runClaimTaskLeased(idArgs(tH), QStringLiteral("sarah"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "FIND-D夹具: sarah 认领触发 advanced 回调");
    TestHarness::check(mgr.currentPlanRequestId(QStringLiteral("sarah")).isEmpty(),
                       "FIND-D: 换工即作废案号（lcc :363 无条件 pop）");
    TestHarness::check(mgr.planGate(QStringLiteral("sarah")) == AgentTeamsManager::PlanGate::NotRequired,
                       "FIND-D: NotRequired 门换工不动（lcc :360-362 只复位发过牌的门）");
}

// ————————————————————————————————————————————————————————————————
// 组11：consumeLeadInbox 不吞协议 + formatTeamEvents 行形 + M8 重试（lcc :424-450）
// —————————————————————————————————————————————————————————————──
void testLeadInboxAndEventsFormat()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    TestHarness::check(mgr.consumeLeadInbox().isEmpty(), "inbox: 空箱返回空批");
    TestHarness::check(AgentTeamsManager::formatTeamEvents(QVector<BusMessage>()).isEmpty(),
                       "format: 空批返回空串");

    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString rid = captureReqId(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("the plan")));
    TestHarness::check(bus.send(QStringLiteral("eve"), QStringLiteral("lead"),
                                QStringLiteral("all done"), QStringLiteral("result")),
                       "inbox夹具: eve result 入箱");
    const QVector<BusMessage> batch = mgr.consumeLeadInbox();
    TestHarness::check(batch.size() == 2,
                       "inbox: 协议请求信不吞——原始批全量返回（doc「swallowed」说法已否决）");
    const QString rendered = AgentTeamsManager::formatTeamEvents(batch);
    TestHarness::check(rendered == QStringLiteral("[Team events]\n"
                                                  "[plan_approval_request request_id=%1] bob: the plan\n"
                                                  "[result] eve: all done").arg(rid),
                       "format: 逐字行形（类型+可选 request_id+发件人冒号正文，信箱序）");
    TestHarness::check(mgr.consumeLeadInbox().isEmpty(), "inbox: 破坏性读取二次为空");

    // M8 fail-closed：句柄占用 lead.jsonl → drain 释放失败 → 空批 + lastError；
    // 松手后下一拍重试成功（消息不丢，钉死第2条）。占用失败则诚实 SKIP 该项。
    const QString mailboxes = QDir(root).filePath(AgentConst::kMailboxesDirName);
    TestHarness::check(bus.send(QStringLiteral("zoe"), QStringLiteral("lead"),
                                QStringLiteral("later"), QStringLiteral("message")),
                       "M8夹具: 追加一封信");
    QFile hold(QDir(mailboxes).filePath(QStringLiteral("lead.jsonl")));
    if (hold.open(QIODevice::ReadOnly)) {
        const QVector<BusMessage> failed = mgr.consumeLeadInbox();
        TestHarness::check(failed.isEmpty() && !bus.lastError().isEmpty(),
                           "M8: 释放失败 → 空批 + lastError 非空（fail-closed）");
        hold.close();
        const QVector<BusMessage> retried = mgr.consumeLeadInbox();
        TestHarness::check(retried.size() == 1 && retried.at(0).content == QStringLiteral("later"),
                           "M8: 下拍重试收全量（信箱保留不丢信）");
    } else {
        std::printf("SKIP: M8 句柄占用夹具不可用\n");
    }
}

// ————————————————————————————————————————————————————————————————
// 组12：genRequestId 形态与案卷去重（lcc :383-389）
// —————————————————————————————————————————————————————————————──
void testGenRequestId()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    const QRegularExpression shape(QStringLiteral("^req_\\d{6}$"));
    bool allMatch = true;
    for (int i = 0; i < 30; ++i) {
        const QString id = mgr.genRequestId();
        allMatch = allMatch && shape.match(id).hasMatch() && id.size() == 10;
    }
    TestHarness::check(allMatch, "genRequestId: 30 连抽全形 req_\\d{6}");

    mgr.runSpawnTeammate(QStringLiteral("bob"), QStringLiteral("c"), QStringLiteral("p"), QString(), true);
    const QString rid = captureReqId(mgr.submitPlan(QStringLiteral("bob"), QStringLiteral("p1")));
    TestHarness::check(!rid.isEmpty(), "genRequestId夹具: 案卷入账");
    bool noneDup = true;
    for (int i = 0; i < 20; ++i) {
        noneDup = noneDup && mgr.genRequestId() != rid;
    }
    TestHarness::check(noneDup, "genRequestId: 在册案号必避重（do/while contains）");
}

// ————————————————————————————————————————————————————————————————
// 组13：claimNextTask 自拉活（lcc :607-624，偏D 快照扫描）
// —————————————————————————————————————————————————————————————──
void testClaimNextTask()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    TestHarness::check(!mgr.claimNextTask(QStringLiteral("ann")).has_value(),
                       "自拉活: 空台账 nullopt");

    const QString t13 = newTask(store, QStringLiteral("pool-job"));
    const std::optional<TaskStore::TaskSnapshot> got = mgr.claimNextTask(QStringLiteral("ann"));
    TestHarness::check(got.has_value() && got->id == t13 && got->subject == QStringLiteral("pool-job"),
                       "自拉活: 候选命中回快照（TaskSnapshot 唯一视图）");
    // Gate② FIND-H 钉桩：回快照须带 description——TeammateRuntime 自拉活任务卡的取文
    // 通道（lcc agent_teams_manager.py :1019；newTask 夹具固定写 "desc"）。
    TestHarness::check(got.has_value() && got->description == QStringLiteral("desc"),
                       "自拉活: 回快照 description 逐字透传（FIND-H 任务卡通道）");
    const std::optional<TaskStore::Lease> lease = store.leaseFor(QStringLiteral("ann"));
    TestHarness::check(lease.has_value() && lease->taskId == t13
                           && store.assignmentVersion(QStringLiteral("ann")) == 1,
                       "自拉活: 认领生效建租约、版本 1");
    TestHarness::check(!mgr.claimNextTask(QStringLiteral("ann")).has_value(),
                       "自拉活: 持租约即闸（nullopt）");

    // 依赖闸：唯一 pending 块挂在真实存在且未完成的依赖上 → 无候选。
    // lcc task_manager.py :228-229 实证添加时即做存在性校验（'Dependency not found:<id>'
    // 冒号后无空格），lite 生产码同口径——夹具必须挂真依赖，不存在 id 走钉桩拒绝断言。
    TaskStore store2([root] { return root; });
    MessageBus bus2([root] { return root; });
    AgentTeamsManager mgr2(&bus2, &store2);
    const QString t14 = newTask(store2, QStringLiteral("blocked-job"));
    const QString depId = newTask(store2, QStringLiteral("dep-job"));
    QJsonObject badUpd = idArgs(t14);
    badUpd.insert(QStringLiteral("addBlockedBy"), QJsonArray{QStringLiteral("task_deadbeef")});
    TestHarness::check(store2.runUpdateTask(badUpd)
                           == QStringLiteral("Error: Dependency not found:task_deadbeef"),
                       "依赖钉桩: addBlockedBy 挂不存在依赖被拒（lcc :228-229 存在性校验）");
    QJsonObject upd = idArgs(t14);
    upd.insert(QStringLiteral("addBlockedBy"), QJsonArray{depId});
    TestHarness::check(store2.runUpdateTask(upd).startsWith(QStringLiteral("Updated ")),
                       "依赖夹具: addBlockedBy 挂真实 pending 依赖（未完即闸）");
    // dep-job 本身 pending+无主+无依赖=合法候选（lcc scan pending&unowned&can_start 同义，
    // 生产码正确）——须先被他主认领退出候选池，本断言的「唯一候选 blocked-job 被未完依赖闸」
    // 场景才成立（测试夹具缺陷修复，非生产缺陷）。
    TestHarness::check(store2.runClaimTaskLeased(idArgs(depId), QStringLiteral("ana"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "依赖夹具: dep-job 他主认领后退出候选池");
    TestHarness::check(!mgr2.claimNextTask(QStringLiteral("tim")).has_value(),
                       "自拉活: 全候选被依赖闸 → nullopt");

    // 损坏台账：fail-closed nullopt 不抛（listTaskSnapshots 错误静默折叠）。
    TestHarness::check(writeRawTaskFile(root, QStringLiteral("task_bad0000"),
                                        QStringLiteral("{ not json")),
                       "损坏夹具: 非法任务文件写入");
    TestHarness::check(!mgr2.claimNextTask(QStringLiteral("cor")).has_value(),
                       "自拉活: 台账不可读 fail-closed nullopt（不抛异常）");
}

// ————————————————————————————————————————————————————————————————
// 组14：Gate② P3a-B minor 批回归面——FIND-M 诊断面语义 + FIND-N3 心跳常量单源。
// FIND-D/FIND-E 的承重断言随组1（回滚全链/磁盘失败留痕/阳性路径）与组10(g)
// （advanced 无条件清案号）钉桩。FIND-L 析构安全网**不可进单元面**：
// TeammateRuntime.cpp 不在测试目标（root CMakeLists 禁域不可增源），
// 行为验证归 cl /c 语法编译 + P3 宿主冒烟——交付报告如实登记，不造摆动断言。
// ————————————————————————————————————————————————————————————————
void testGate2MinorBatchSurface()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);

    TestHarness::check(mgr.lastReleaseWarning().isEmpty(), "FIND-M: 新 mgr 诊断面为空");
    mgr.noteReleaseWarning(QStringLiteral("boom-1"));
    TestHarness::check(mgr.lastReleaseWarning() == QStringLiteral("boom-1"),
                       "FIND-M: note 后逐字回读");
    mgr.noteReleaseWarning(QStringLiteral("boom-2"));
    TestHarness::check(mgr.lastReleaseWarning() == QStringLiteral("boom-2"),
                       "FIND-M: 粘滞覆盖保末次（lcc fail-stop 无此概念，lite fail-continue 补偿语义）");
    mgr.finalizeTeammate(QStringLiteral("nobody"));
    TestHarness::check(mgr.lastReleaseWarning() == QStringLiteral("boom-2"),
                       "FIND-M: 退场清算不清诊断面（间歇故障对宿主保持可见）");

    TestHarness::check(AgentConst::kTeamIdleScanIntervalMs == 2000,
                       "FIND-N3: 空闲心跳间隔单源在 AgentConst（2000ms=lcc :185 2.0s 逐值对位）");
}

// ————————————————————————————————————————————————————————————————
// 组15：Gate③ MINOR-4 异步工具桥（manager 侧面貌）——挂起哨兵 / 续跑 /
// 早归暂存 / clearPendingResume 与 finalizeTeammate 丢弃 / 门-权限-cwd 复核
// 先于发起 / 同步表回退。TeammateRuntime 截批续跑不在测试目标（组14 同款
// 纪律：该 TU 不链入），其验证归 cl /c + 宿主冒烟。期望文本从
// src/AgentTeamsManager.cpp 现文逐字实抄（哨兵字面量 :802、折叠前缀 :444、
// Blocked :410-412 等）。零事件循环依赖：假适配器直接调 done。
// —————————————————————————————————————————————————————————————──
void testAsyncToolBridge()
{
    ScopedTempRoot tmp("agentteams");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    MessageBus bus([root] { return root; });
    AgentTeamsManager mgr(&bus, &store);
    mgr.runSpawnTeammate(QStringLiteral("eve"), QStringLiteral("c"), QStringLiteral("p"), QString(), false);

    QStringList hookLog;
    mgr.setHooksTrigger([&](const QString &evt, const QString &toolName,
                            const QJsonObject &, const QString &output) {
        hookLog << evt + QLatin1Char('|') + toolName;
        if (evt == QStringLiteral("PostToolUse")) {
            hookLog.last() += QLatin1Char('=') + output; // 记账输出，返回值被忽略（偏C' 同款）
        }
        return QString();
    });
    QString permissionDeny; // 空串=放行
    mgr.setPermissionCheck([&](const QString &toolName, const QJsonObject &) {
        return toolName == ToolNames::BASH ? permissionDeny : QString();
    });

    int syncBashCalls = 0;
    mgr.setToolAdapter(ToolNames::BASH, [&](const QJsonObject &, const QString &) {
        ++syncBashCalls;
        return QStringLiteral("BASH-OK");
    });
    int asyncStarts = 0;
    QString asyncCwd;
    bool earlyMode = false;
    std::function<void(const QString &)> lastDone;
    mgr.setToolAsyncAdapter(ToolNames::BASH,
                            [&](const QJsonObject &, const QString &cwd,
                                std::function<void(const QString &)> done) {
                                ++asyncStarts;
                                asyncCwd = cwd;
                                if (earlyMode) {
                                    // 适配器内同步早归（danger 前置拒等形态）：测 earlyResult 暂存。
                                    done(QStringLiteral("EARLY-DONE"));
                                } else {
                                    lastDone = std::move(done);
                                }
                            });

    const QString t1 = newTask(store, QStringLiteral("async-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t1), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "异步夹具: eve 认领任务（租约立起，assignmentCwd 放行）");

    QJsonObject bashArgs;
    bashArgs.insert(QStringLiteral("command"), QStringLiteral("echo hi"));

    // ① plan 门先于异步发起（与同步同位同口径，无挂起账）。
    TestHarness::check(mgr.runRequestPlan(QStringLiteral("eve"), QStringLiteral("do x"))
                           == QStringLiteral("Plan requested from eve"),
                       "异步夹具: 置 required 门");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == QStringLiteral("Blocked: plan status is required. Submit or revise the plan "
                                             "and wait for approval before changing the workspace."),
                       "异步门: plan Blocked 逐字且不发起");
    TestHarness::check(asyncStarts == 0 && syncBashCalls == 0, "异步门: Blocked 时两侧 adapter 均未触达");
    // 合法在审账本（组5同规）：runRequestPlan 只复位门不立案卷，案号唯 submit_plan 登记；
    // 缺这一步则 currentPlanRequestId 恒空 → 批准整单忽略 → 门停 required → 哨兵永不可达。
    const QString subOut = mgr.submitPlan(QStringLiteral("eve"), QStringLiteral("step-one"));
    TestHarness::check(subOut.startsWith(QStringLiteral("Plan submitted (")),
                       "异步夹具: submit_plan 立案卷、门转 pending");
    const QString eveRid = captureReqId(subOut);
    TestHarness::check(!eveRid.isEmpty(), "异步夹具: eve 在审案号");
    TestHarness::check(mgr.runReviewPlan(eveRid, true, QString())
                           == QStringLiteral("Plan approved (%1)").arg(eveRid),
                       "异步夹具: Lead 审案翻案卷 approved");
    QString apOut;
    TestHarness::check(mgr.applyPlanResponse(QStringLiteral("eve"),
        mkMsg(QStringLiteral("lead"), QStringLiteral("eve"), QStringLiteral("ok"),
              QStringLiteral("plan_approval_response"), eveRid, true), &apOut)
                           && apOut == QStringLiteral("[Plan approved] ok"),
                       "异步夹具: 批准放行");

    // ② 正常发起：哨兵逐字返回、同步表被优先压制、Post 推迟到续跑点。
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == QStringLiteral("<async-tool-pending>"),
                       "异步派发: 挂起哨兵逐字（钉桩字面量=禁翻区串）");
    TestHarness::check(mgr.asyncPendingSentinel() == QStringLiteral("<async-tool-pending>"),
                       "异步派发: 哨兵单源同字面量（生产定义点与测试锚同源）");
    TestHarness::check(asyncStarts == 1 && syncBashCalls == 0 && asyncCwd == root,
                       "异步派发: 异步表优先于同步表、cwd 仍 assignmentCwd 现读");
    TestHarness::check(hookLog.contains(QStringLiteral("PreToolUse|bash"))
                           && !hookLog.contains(QStringLiteral("PostToolUse|bash")),
                       "异步派发: PostToolUse 推迟（发起点无 Post）");

    // ③ 续跑：先挂闭包、done 后到 → 闭包携结果、Post 补发真实结果、销账后二次 done 丢弃。
    int resumes = 0;
    QString resumeText;
    mgr.setPendingResume(QStringLiteral("eve"), [&](const QString &result) {
        ++resumes;
        resumeText = result;
    });
    TestHarness::check(resumes == 0, "异步续跑: 仅挂位不触发（无暂存态时闭包静候）");
    TestHarness::check(static_cast<bool>(lastDone), "异步续跑: done 闭包已捕获（防空调用炸整个测试进程）");
    const std::function<void(const QString &)> done1 = lastDone;
    if (done1) {
        done1(QStringLiteral("BASH-RESULT"));
    }
    TestHarness::check(resumes == 1 && resumeText == QStringLiteral("BASH-RESULT"),
                       "异步续跑: done 驱动闭包逐字回灌");
    TestHarness::check(hookLog.contains(QStringLiteral("PostToolUse|bash=BASH-RESULT")),
                       "异步续跑: PostToolUse 在续跑点携真实结果补发（有意偏离登记面）");
    // 与上方「done 闭包已捕获」同源的 if(done1) 门卫：捕获断言已红时此处
    // 不再空调用炸进程；失败面不缩——空调被跳过则 resumes 仍 0，
    // 下一条「同名二次 done 丢弃」照红。
    if (done1) {
        done1(QStringLiteral("BASH-SECOND"));
    }
    TestHarness::check(resumes == 1, "异步续跑: 同名二次 done 丢弃（账已销）");

    // ④ 早归：done 在发起栈内同步早归 → earlyResult 暂存 → setPendingResume 同栈续跑。
    earlyMode = true;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == AgentTeamsManager::asyncPendingSentinel(),
                       "异步早归: 哨兵照发（done 早归不改派发语义）");
    earlyMode = false;
    bool earlyDelivered = false;
    QString earlyText;
    mgr.setPendingResume(QStringLiteral("eve"), [&](const QString &result) {
        earlyDelivered = true;
        earlyText = result;
    });
    TestHarness::check(earlyDelivered && earlyText == QStringLiteral("EARLY-DONE"),
                       "异步早归: setPendingResume 即同栈消费暂存续跑（递归深度受批大小界，无事件重入）");
    TestHarness::check(hookLog.contains(QStringLiteral("PostToolUse|bash=EARLY-DONE")),
                       "异步早归: Post 于早归续跑时同样补发");

    // ⑤ clearPendingResume 销账：晚归结果丢弃（runtime 退场清算的 manager 侧半面）。
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == AgentTeamsManager::asyncPendingSentinel(),
                       "异步销账夹具: 再发起");
    bool clearedFired = false;
    mgr.setPendingResume(QStringLiteral("eve"), [&](const QString &) { clearedFired = true; });
    mgr.clearPendingResume(QStringLiteral("eve"));
    TestHarness::check(static_cast<bool>(lastDone), "异步销账: done 闭包已捕获（防空调用）");
    const std::function<void(const QString &)> done3 = lastDone;
    if (done3) {
        done3(QStringLiteral("DISCARDED"));
    }
    TestHarness::check(!clearedFired, "异步销账: clearPendingResume 后 done 静默丢弃（shutdown 窗口裁决口径）");

    // ⑥ finalizeTeammate 五本账收口：挂起窗口内晚归丢弃。
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == AgentTeamsManager::asyncPendingSentinel(),
                       "异步退场夹具: 再发起");
    bool finalizeFired = false;
    mgr.setPendingResume(QStringLiteral("eve"), [&](const QString &) { finalizeFired = true; });
    mgr.finalizeTeammate(QStringLiteral("eve"));
    TestHarness::check(static_cast<bool>(lastDone), "异步退场: done 闭包已捕获（防空调用）");
    const std::function<void(const QString &)> done4 = lastDone;
    if (done4) {
        done4(QStringLiteral("LATE-AFTER-FINALIZE"));
    }
    TestHarness::check(!finalizeFired, "异步退场: finalizeTeammate 销挂起账 → 晚归结果无人认领即丢弃");

    // ⑦ 孤儿续跑调用：无账可查一律静默，不崩不抛。
    mgr.resumePendingTool(QStringLiteral("ghostx"), QStringLiteral("orphan"));
    bool orphanFired = false;
    mgr.setPendingResume(QStringLiteral("ghosty"), [&](const QString &) { orphanFired = true; });
    mgr.resumePendingTool(QStringLiteral("ghosty"), QStringLiteral("x"));
    TestHarness::check(!orphanFired, "异步孤儿: 无挂起账时挂位/续跑均 no-op（闭包作废）");

    // ⑧ 权限硬拒先于发起：拒绝逐字返回、异步零发起、零挂起账。
    permissionDeny = QStringLiteral("Permission required: no way");
    const int startsBefore = asyncStarts;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == QStringLiteral("Permission required: no way"),
                       "异步权限: 硬拒文本逐字返回（与同步同位同口径）");
    TestHarness::check(asyncStarts == startsBefore, "异步权限: 硬拒零发起");
    permissionDeny.clear();

    // ⑨ cwd 现读失败：fail-closed 折叠、不发起（fix-4 钉死第 3 条同步口径延伸）。
    QString err;
    TestHarness::check(store.runCompleteTaskLeased(idArgs(t1), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Completed ")),
                       "异步现读夹具: eve 完工");
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("eve"), &err),
                       "异步现读夹具: 回合边界退租");
    const int startsBefore2 = asyncStarts;
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::BASH, bashArgs)
                           == QStringLiteral("Error: Invalid task assignment: No active assignment for eve"),
                       "异步现读: 无租约折叠逐字（未发起异步）");
    TestHarness::check(asyncStarts == startsBefore2, "异步现读: 失败零发起");

    // ⑩ 回退面：未注册异步的 read_file 仍走同步表；未知工具先返不受影响。
    const QString t2 = newTask(store, QStringLiteral("sync-fallback-job"));
    TestHarness::check(store.runClaimTaskLeased(idArgs(t2), QStringLiteral("eve"))
                           .startsWith(QStringLiteral("Claimed ")),
                       "回退夹具: eve 认领 t2（租约复立）");
    QJsonObject pathArgs;
    pathArgs.insert(QStringLiteral("path"), QStringLiteral("a.txt"));
    mgr.setToolAdapter(ToolNames::READ_FILE, [](const QJsonObject &, const QString &) {
        return QStringLiteral("READ-OK");
    });
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), ToolNames::READ_FILE, pathArgs)
                           == QStringLiteral("READ-OK"),
                       "回退: read_file 未注册异步 → 同步 adapter 照常派发");
    TestHarness::check(mgr.runTeammateTool(QStringLiteral("eve"), QStringLiteral("zipzap"), pathArgs)
                           == QStringLiteral("Unknown tool: zipzap"),
                       "回退: 异步表存在不改未知工具先返（lcc :518-520）");
}

} // namespace

int tst_agentteams()
{
    const int before = TestHarness::failCount();
    testSpawnValidationAndRollback();
    testListTeammatesAndNames();
    testSendMessage();
    testSubmitPlanLedger();
    testApplyPlanResponseGates();
    testShutdownJourney();
    testReviewPlanFiveGates();
    testMatchResponseGates();
    testTeammateToolGatesAndCwd();
    testReleaseTripointsAndCallbacks();
    testLeadInboxAndEventsFormat();
    testGenRequestId();
    testClaimNextTask();
    testGate2MinorBatchSurface();
    testAsyncToolBridge();
    return TestHarness::failCount() - before;
}
