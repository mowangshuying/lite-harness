// ============================================================================
// tst_taskstore_lease.cpp — s13 Lane A「租约台账 + worktree 绑定」内核单测
// （port-s13 fix-2 重建版：事故前达成 pass=241 的逐字重放，唯一差异 = 临时根
//   由 QTemporaryDir 改为 ScopedTempRoot("taskstore-lease") 契约：
//   LITE_TEST_TMPROOT 未设置/不可写时各套件打印 SKIP 并返回 0，零删除风险。
//   毁仓事故根治件见 ScopedTempRoot.h 头注释。）
//
// 覆盖（对应任务书验收清单）：
//   组1 worktree 字段兼容：旧 6 键文件读回 worktree=null；setWorktree/clearWorktree
//     往返；ts+worktree 双可选键并存；杂键/错类型判 Invalid。
//   组2 claim 六门：门①存在性（不存在→error 通道；completed→业务文本）；
//     门②有主拒绝并点名现主；门③内存租约占用；成功路径（租约建立、版本自增、
//     onAssignmentAdvanced 回调、磁盘落 in_progress+owner、'Claimed id (subject)' 文本）；
//     门④磁盘遗留 in_progress（“重启后”新 store 无内存租约）；门⑤依赖未清；
//     门⑥ cwd 不可解（resolver 报错→业务文本、任务不动、不建租约）；resolver 供值
//     优先于回落链；workDirSink 回落优先于 sessionRootSink。
//   组3 门④ fail-closed：台账不可读（坏 JSON 文件）→ error 通道 false、不建租约。
//   组4 complete 与释放旅程：complete 前释放被拒且租约保留；planGateCheck 否决
//     （含空 reason 静默否决）；complete 后租约仍在（不释放铁律）；版本不递增；
//     回合内再次 claim 撞门③；releaseCompletedAssignment 成功 + onAssignmentReleased +
//     幂等；换工版本自增；owner 不符文本（含 'None' 呈现）；状态门先于 owner 门；
//     解锁链 '\nUnblocked: …'。
//   组5 租约自愈：legacy 无租约 claim → kernel complete 自愈重建租约（不递增版本、
//     不发 advanced）；自愈失败（resolver 报错）→ 业务文本、租约缺失、磁盘不动；
//     租约指向他任时 complete 改指向。
//   组6 releaseTeammateAssignment：遗留 in_progress 降级 pending/清 owner；completed
//     不回退；幽灵 owner 幂等仍发回调；“崩溃后”新 store 内存空账（D7）仍能清磁盘。
//   组7 双 owner 互不干扰；组8 折叠包装 runClaimTaskLeased/runCompleteTaskLeased。
// ============================================================================

#include "TestHarness.h"

#include "ScopedTempRoot.h"
#include "TaskStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

// Lead 保留键（lcc assignments 键 "agent"；lite 同）
const QString kAgent = QStringLiteral("agent");

// 从 runCreateTask 返回文本 'Created <id>: <subject>' 提取任务 ID
QString newTaskId(TaskStore &store, const QString &subject)
{
    QJsonObject args;
    args.insert(QStringLiteral("subject"), subject);
    args.insert(QStringLiteral("description"), QStringLiteral("desc"));
    const QString text = store.runCreateTask(args);
    static const QRegularExpression re(QStringLiteral("task_[0-9a-f]{8}"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(0) : QString();
}

QJsonObject idArgs(const QString &taskId)
{
    QJsonObject args;
    args.insert(QStringLiteral("task_id"), taskId);
    return args;
}

// 手写脏夹具直落 <root>/.task/<id>.json。
// 根因登记（事故前回归修复）：旧实现 QDir(root).mkpath(".task") 后用 dir.filePath()
// ——filePath 的基准仍是 <root>，夹具落到了 <root>/task_x.json，导致 15 条依赖原始
// 文件的断言全错。此处必须 mkpath 与落盘同用 taskDir 基准。
bool writeRawTask(const QString &root, const QString &taskId, const QString &json)
{
    const QString taskDir = QDir(root).filePath(QStringLiteral(".task"));
    if (!QDir().mkpath(taskDir))
        return false;
    QFile file(QDir(taskDir).filePath(taskId + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray bytes = json.toUtf8();
    return file.write(bytes) == bytes.size();
}

// 旧格式 6 键任务文件（无 timestamp、无 worktree——s12 之前的存量形态）
QString legacyTaskJson(const QString &taskId, const QString &subject, const QString &status,
                       const QString &ownerJson)
{
    return QStringLiteral("{\n"
                          "  \"id\": \"%1\",\n"
                          "  \"subject\": \"%2\",\n"
                          "  \"description\": \"d\",\n"
                          "  \"status\": \"%3\",\n"
                          "  \"owner\": %4,\n"
                          "  \"blockedBy\": []\n"
                          "}")
        .arg(taskId, subject, status, ownerJson);
}

// 回调记录器：声明必须先于 TaskStore（析构逆序：store 先亡，回调后 record 才销毁，
// 避免悬垂捕获——store 析构虽不触发回调，此为防御性次序纪律）
struct Recorder
{
    int advanced = 0;
    QString advancedOwner;
    QString advancedTask;
    int released = 0;
    QString releasedOwner;
};

// 内核 claim/complete 的测试便捷包装：bool=false 时把 error 通道折进返回串方便断言
QString claimKernel(TaskStore &store, const QString &taskId, const QString &owner, bool *ok,
                    QString *error)
{
    QString result;
    const bool success = store.claimTask(taskId, owner, &result, error);
    if (ok)
        *ok = success;
    return success ? result : QString();
}

QString completeKernel(TaskStore &store, const QString &taskId, const QString &owner, bool *ok,
                       QString *error)
{
    QString result;
    const bool success = store.completeTask(taskId, owner, &result, error);
    if (ok)
        *ok = success;
    return success ? result : QString();
}

} // namespace

// ---------------------------------------------------------------------------
// 组1：worktree 字段兼容与往返
// ---------------------------------------------------------------------------
static void testWorktreeFieldCompat()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });

    // 旧 6 键文件：必须兼容可读，缺 worktree 键读回视为 null（runGetTask 落 "null"）
    const QString legacyId = QStringLiteral("task_00000001");
    TestHarness::check(writeRawTask(root, legacyId,
                                    legacyTaskJson(legacyId, QStringLiteral("old-job"),
                                                   QStringLiteral("pending"),
                                                   QStringLiteral("null"))),
                       "夹具: 旧格式文件写入成功");
    const QString legacyText = store.runGetTask(idArgs(legacyId));
    TestHarness::check(!legacyText.startsWith(QStringLiteral("Error:")), "旧 6 键文件兼容可读");
    TestHarness::check(legacyText.contains(QStringLiteral("\"worktree\": null")),
                       "缺键读回视为 null");
    TestHarness::check(legacyText.contains(QStringLiteral("\"timestamp\": 0.000000")),
                       "旧文件缺 timestamp 读回 0");

    // setWorktree / clearWorktree 往返（新 8 键文件）
    const QString tid = newTaskId(store, QStringLiteral("wt-job"));
    QString error;
    TestHarness::check(store.setWorktree(tid, QStringLiteral("D:/repo/.worktrees/wt_a"), &error),
                       "setWorktree 成功");
    TestHarness::check(error.isEmpty(), "setWorktree 无错误");
    const QString bound = store.runGetTask(idArgs(tid));
    TestHarness::check(bound.contains(QStringLiteral("\"worktree\": \"D:/repo/.worktrees/wt_a\"")),
                       "绑定值落盘");
    TestHarness::check(store.clearWorktree(tid, &error), "clearWorktree 成功");
    const QString cleared = store.runGetTask(idArgs(tid));
    TestHarness::check(cleared.contains(QStringLiteral("\"worktree\": null")), "解绑即置 null");

    // setWorktree 对不存在任务：失败走 error 通道
    TestHarness::check(!store.setWorktree(QStringLiteral("task_ffffffff"),
                                          QStringLiteral("x"), &error) && !error.isEmpty(),
                       "setWorktree 无此任务 → error 通道");

    // timestamp + worktree 双可选键并存的 8 键文件可读
    const QString bothId = QStringLiteral("task_00000002");
    const QString bothJson = QStringLiteral("{\n"
                                            "  \"id\": \"%1\",\n"
                                            "  \"subject\": \"both\",\n"
                                            "  \"description\": \"d\",\n"
                                            "  \"status\": \"pending\",\n"
                                            "  \"owner\": null,\n"
                                            "  \"timestamp\": 123.5,\n"
                                            "  \"blockedBy\": [],\n"
                                            "  \"worktree\": \"wt-abc\"\n"
                                            "}")
                                 .arg(bothId);
    TestHarness::check(writeRawTask(root, bothId, bothJson), "夹具: 8 键文件写入成功");
    const QString bothText = store.runGetTask(idArgs(bothId));
    TestHarness::check(bothText.contains(QStringLiteral("\"worktree\": \"wt-abc\""))
                           && bothText.contains(QStringLiteral("\"timestamp\": 123.500000")),
                       "ts+worktree 双可选键并存可读");

    // 第 7 个杂键（既非 timestamp 亦非 worktree）→ 仍判 Invalid（多键从严纪律保持）
    const QString strayId = QStringLiteral("task_00000003");
    const QString strayJson = QStringLiteral("{\n"
                                             "  \"id\": \"%1\",\n"
                                             "  \"subject\": \"s\",\n"
                                             "  \"description\": \"d\",\n"
                                             "  \"status\": \"pending\",\n"
                                             "  \"owner\": null,\n"
                                             "  \"blockedBy\": [],\n"
                                             "  \"zz\": 1\n"
                                             "}")
                                  .arg(strayId);
    TestHarness::check(writeRawTask(root, strayId, strayJson), "夹具: 杂键文件写入成功");
    TestHarness::check(store.runGetTask(idArgs(strayId))
                           .startsWith(QStringLiteral("Error: Invalid task file contents")),
                       "第 7 键杂键仍判 Invalid");

    // 类型错误：worktree 为数值 → Invalid
    const QString badId = QStringLiteral("task_00000004");
    const QString badJson = QStringLiteral("{\n"
                                          "  \"id\": \"%1\",\n"
                                          "  \"subject\": \"s\",\n"
                                          "  \"description\": \"d\",\n"
                                          "  \"status\": \"pending\",\n"
                                          "  \"owner\": null,\n"
                                          "  \"blockedBy\": [],\n"
                                          "  \"worktree\": 7\n"
                                          "}")
                               .arg(badId);
    TestHarness::check(writeRawTask(root, badId, badJson), "夹具: 数值 worktree 文件写入成功");
    TestHarness::check(
        store.runGetTask(idArgs(badId)).startsWith(QStringLiteral("Error: Invalid task file contents")),
        "数值型 worktree 判 Invalid");
}

// ---------------------------------------------------------------------------
// 组2：claim 六门逐一 + 成功路径 + 回落链
// ---------------------------------------------------------------------------
static void testClaimSixGates()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    Recorder rec;
    TaskStore store([root] { return root; });
    store.setOnAssignmentAdvanced(
        [&rec](const QString &owner, const QString &taskId) {
            ++rec.advanced;
            rec.advancedOwner = owner;
            rec.advancedTask = taskId;
        });

    // 门①（error 通道）：任务不存在
    bool ok = false;
    QString error;
    claimKernel(store, QStringLiteral("task_ffffffff"), QStringLiteral("alice"), &ok, &error);
    TestHarness::check(!ok && error.contains(QStringLiteral("not found")),
                       "门①: 不存在 → error 通道 false");

    // 门①（业务文本）：completed 状态拒绝
    const QString done = newTaskId(store, QStringLiteral("done-job"));
    {
        QString result;
        QString e;
        TestHarness::check(store.completeTask(done, kAgent, &result, &e)
                               && result.contains(QStringLiteral("cannot complete")),
                           "前置: pending 任务 complete 走状态门");
    }
    const QString completedFixture = QStringLiteral("task_00000010");
    TestHarness::check(writeRawTask(root, completedFixture,
                                    legacyTaskJson(completedFixture, QStringLiteral("c"),
                                                   QStringLiteral("completed"),
                                                   QStringLiteral("null"))),
                       "夹具: completed 文件写入成功");
    const QString gate1Text =
        claimKernel(store, completedFixture, QStringLiteral("bob"), &ok, &error);
    TestHarness::check(ok && gate1Text == QStringLiteral("Task %1 is completed, cannot claim").arg(completedFixture),
                       "门①': completed → 业务文本拒绝");

    // 门②：pending 但有主（lcc s13 新增门，手写脏夹具构造该状态）
    const QString ownedFixture = QStringLiteral("task_00000011");
    TestHarness::check(writeRawTask(root, ownedFixture,
                                    legacyTaskJson(ownedFixture, QStringLiteral("o"),
                                                   QStringLiteral("pending"),
                                                   QStringLiteral("\"bob\""))),
                       "夹具: pending+有主文件写入成功");
    const QString gate2Text =
        claimKernel(store, ownedFixture, QStringLiteral("carol"), &ok, &error);
    TestHarness::check(ok && gate2Text == QStringLiteral("Task %1 is already owned by bob").arg(ownedFixture),
                       "claim 门②: 有主任务拒绝并点名现主");

    // 成功路径（首claim）：租约建立、版本 0→1、advanced 回调、磁盘 in_progress+owner、
    // 承重文本 'Claimed <id> (<subject>)'（s13 带括号形态）
    const QString first = newTaskId(store, QStringLiteral("first-job"));
    const QString successText = claimKernel(store, first, QStringLiteral("alice"), &ok, &error);
    TestHarness::check(ok && successText == QStringLiteral("Claimed %1 (first-job)").arg(first),
                       "claim 成功: 'Claimed id (subject)' 承重契约文本");
    TestHarness::check(successText.startsWith(QStringLiteral("Claimed ")),
                       "claim 成功: 承重前缀 'Claimed '");
    const std::optional<TaskStore::Lease> lease = store.leaseFor(QStringLiteral("alice"));
    TestHarness::check(lease.has_value() && lease->taskId == first
                           && lease->cwd == root,
                       "claim 成功: 租约{taskId,cwd=回落 sessionRoot}建立");
    TestHarness::check(store.assignmentVersion(QStringLiteral("alice")) == 1,
                       "claim 成功: 版本自增至 1");
    TestHarness::check(rec.advanced == 1 && rec.advancedOwner == QStringLiteral("alice")
                           && rec.advancedTask == first,
                       "claim 成功: onAssignmentAdvanced(owner,taskId) 被调");
    const QString diskText = store.runGetTask(idArgs(first));
    TestHarness::check(diskText.contains(QStringLiteral("\"status\": \"in_progress\""))
                           && diskText.contains(QStringLiteral("\"owner\": \"alice\"")),
                       "claim 成功: 磁盘落 in_progress+owner");

    // 门③：同 owner 已有内存租约 → 要求先结束当前工作回合
    const QString second = newTaskId(store, QStringLiteral("second-job"));
    const QString gate3Text =
        claimKernel(store, second, QStringLiteral("alice"), &ok, &error);
    TestHarness::check(ok && gate3Text
                           == QStringLiteral("Owner alice must finish the current work turn for %1 "
                                              "before claiming another task")
                                  .arg(first),
                       "门③: 内存租约占用 → 点名现租约任务");

    // 门④：“重启后”的新 store 无内存租约，但磁盘上有该 owner 的 in_progress 遗留
    TaskStore rebooted([root] { return root; });
    const QString gate4Text = claimKernel(rebooted, second, QStringLiteral("alice"), &ok, &error);
    TestHarness::check(ok && gate4Text
                           == QStringLiteral("Owner alice must complete %1 "
                                             "before claiming another task")
                                  .arg(first),
                       "门④: 磁盘遗留 in_progress → 要求先完成");

    // 门⑤：依赖未清
    const QString dep = newTaskId(store, QStringLiteral("dep-job"));
    const QString dependent = newTaskId(store, QStringLiteral("dependent-job"));
    {
        QJsonObject args = idArgs(dependent);
        QJsonArray blockedBy;
        blockedBy.append(dep);
        args.insert(QStringLiteral("addBlockedBy"), blockedBy);
        TestHarness::check(!store.runUpdateTask(args).startsWith(QStringLiteral("Error:")),
                           "前置: addBlockedBy 设置成功");
    }
    const QString gate5Text =
        claimKernel(store, dependent, QStringLiteral("dave"), &ok, &error);
    TestHarness::check(ok && gate5Text.startsWith(QStringLiteral("Blocked by: ['"))
                           && gate5Text.contains(dep),
                       "门⑤: 依赖未清 → Blocked by: [python list repr]");

    // 门⑥：cwd 不可解（resolver 报错）→ 业务文本、任务保持 pending、不建租约
    store.setCwdResolver([](const QString &taskId, QString *err) {
        *err = QStringLiteral("worktree 'gone' is not available for task %1").arg(taskId);
        return QString();
    });
    const QString unresolvable = newTaskId(store, QStringLiteral("unresolvable-job"));
    const QString gate6Text =
        claimKernel(store, unresolvable, QStringLiteral("erin"), &ok, &error);
    TestHarness::check(ok && gate6Text.startsWith(QStringLiteral("Cannot claim "))
                           && gate6Text.contains(QStringLiteral("not available")),
                       "门⑥: cwd 不可解 → 'Cannot claim <id>: <err>' 业务文本");
    TestHarness::check(!store.leaseFor(QStringLiteral("erin")).has_value(),
                       "门⑥: 失败路径不建租约");
    TestHarness::check(store.runGetTask(idArgs(unresolvable)).contains(
                           QStringLiteral("\"status\": \"pending\"")),
                       "门⑥: 失败后任务保持 pending");

    // resolver 供值优先于回落链
    store.setCwdResolver([](const QString &, QString *) { return QStringLiteral("C:/fake/wt_x"); });
    const QString resolved = newTaskId(store, QStringLiteral("resolved-job"));
    claimKernel(store, resolved, QStringLiteral("erin"), &ok, &error);
    TestHarness::check(ok && store.leaseFor(QStringLiteral("erin"))->cwd == QStringLiteral("C:/fake/wt_x"),
                       "resolver 供值 → 租约 cwd 取 resolver 结果");

    // 回落链：workDirSink 优先于 sessionRootSink（cwdResolver 已清空）
    store.setCwdResolver(nullptr);
    TaskStore withWorkDir([root] { return root; },
                          [] { return QStringLiteral("C:/workdir/fallback"); });
    const QString fb1 = newTaskId(withWorkDir, QStringLiteral("fallback-job"));
    claimKernel(withWorkDir, fb1, QStringLiteral("frank"), &ok, &error);
    TestHarness::check(ok && withWorkDir.leaseFor(QStringLiteral("frank"))
                               ->cwd == QStringLiteral("C:/workdir/fallback"),
                       "回落链: workDirSink 优先于 sessionRootSink");
}

// ---------------------------------------------------------------------------
// 组3：门④ fail-closed —— 台账不可读时 claim 走 error 通道而非误判可 claim
// ---------------------------------------------------------------------------
static void testClaimGate4FailClosed()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });

    const QString target = newTaskId(store, QStringLiteral("target-job"));
    // 名字合法但内容损坏的文件：listTasks 必炸（正则通过但内容 Invalid → 上抛）
    TestHarness::check(writeRawTask(root, QStringLiteral("task_99999999"),
                                    QStringLiteral("{ not json")),
                       "夹具: 损坏台账文件写入成功");
    bool ok = false;
    QString error;
    const QString result = claimKernel(store, target, QStringLiteral("probe"), &ok, &error);
    TestHarness::check(!ok && result.isEmpty(),
                       "门④: 台账不可读 → error 通道 false（非业务文本放行）");
    TestHarness::check(error.contains(QStringLiteral("Invalid task file contents")),
                       "门④: 错误源自不可读台账");
    TestHarness::check(!store.leaseFor(QStringLiteral("probe")).has_value(),
                       "门④: 错误路径不建租约");
}

// ---------------------------------------------------------------------------
// 组4：complete 与双释放的完整旅程
// ---------------------------------------------------------------------------
static void testCompleteAndReleaseJourney()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    Recorder rec;
    TaskStore store([root] { return root; });
    store.setOnAssignmentReleased([&rec](const QString &owner) {
        ++rec.released;
        rec.releasedOwner = owner;
    });
    // 重建修复：事故前旅程测钉桩 advanced==2（b11 记录），重放时漏注册 advanced 回调——补上
    store.setOnAssignmentAdvanced(
        [&rec](const QString &owner, const QString &taskId) {
            ++rec.advanced;
            rec.advancedOwner = owner;
            rec.advancedTask = taskId;
        });

    const QString t1 = newTaskId(store, QStringLiteral("job-one"));
    bool ok = false;
    QString error;
    claimKernel(store, t1, kAgent, &ok, &error);
    TestHarness::check(ok, "前置: agent claim t1 成功");

    // 未 completed 就释放 → false + error 文本 + 租约保留
    const bool earlyRelease = store.releaseCompletedAssignment(kAgent, &error);
    TestHarness::check(!earlyRelease && error.contains(QStringLiteral("nothing released")),
                       "release(任务未完成): false + 说明性 error");
    TestHarness::check(store.leaseFor(kAgent)->taskId == t1,
                       "release(任务未完成): 租约保留");
    TestHarness::check(rec.released == 0, "release(任务未完成): 不发 released 回调");
    // 内核约定：error 出参仅在失败路径写入（本仓 bool+error 风格单源）；
    // 上一断言后 error 仍携 'nothing released' 残值，后续成功路径的 error.isEmpty() 复合断言
    // 必须先清陈旧值（事故前 pass=241 版即有本行清值，重放遗漏——测试侧修复，非生产行为变更）
    error.clear();

    // planGateCheck 否决 complete：任务不动、租约保留、否决原因即返回文本
    QString gateSawTask;
    store.setPlanGateCheck([&gateSawTask](const QString &taskId, QString *reason) {
        gateSawTask = taskId;
        *reason = QStringLiteral("Plan approval required");
        return false;
    });
    QString vetoResult;
    TestHarness::check(store.completeTask(t1, kAgent, &vetoResult, &error) && error.isEmpty()
                           && vetoResult == QStringLiteral("Plan approval required")
                           && gateSawTask == t1,
                       "planGateCheck 否决: 原因文本作业务返回");
    TestHarness::check(store.runGetTask(idArgs(t1)).contains(
                           QStringLiteral("\"status\": \"in_progress\"")),
                       "planGateCheck 否决: 任务保持 in_progress");
    TestHarness::check(store.leaseFor(kAgent).has_value(), "planGateCheck 否决: 租约保留");

    // 空 reason 否决（lcc：非 None 即拒，空串同样拒）→ 空返回文本
    store.setPlanGateCheck([](const QString &, QString *reason) {
        reason->clear();
        return false;
    });
    QString emptyVeto = QStringLiteral("<sentinel>");
    TestHarness::check(store.completeTask(t1, kAgent, &emptyVeto, &error) && emptyVeto.isEmpty(),
                       "planGateCheck 空 reason 否决 → 空结果文本");
    store.setPlanGateCheck(nullptr);

    // complete 成功：文本 'Completed <id> (<subject>)'；铁律——租约仍在、版本不动、无释放回调
    const QString done1 = completeKernel(store, t1, kAgent, &ok, &error);
    TestHarness::check(ok && done1 == QStringLiteral("Completed %1 (job-one)").arg(t1),
                       "complete 成功: 'Completed id (subject)'");
    TestHarness::check(store.leaseFor(kAgent)->taskId == t1,
                       "complete 后租约仍在（不释放铁律）");
    TestHarness::check(store.assignmentVersion(kAgent) == 1,
                       "complete 后版本仍 1（不递增铁律）");
    TestHarness::check(rec.released == 0, "complete 后无 released 回调");

    // 同回合再 claim → 门③拦（租约直到释放点才清空）
    const QString t2 = newTaskId(store, QStringLiteral("job-two"));
    const QString intraTurn = claimKernel(store, t2, kAgent, &ok, &error);
    TestHarness::check(ok && intraTurn.contains(QStringLiteral("must finish the current work turn")),
                       "complete 后同回合 claim: 门③ 拦（租约占用）");

    // 回合边界释放：true + 租约清空 + released 回调 + 版本保持（lite 偏离 lcc :411 钉桩）
    TestHarness::check(store.releaseCompletedAssignment(kAgent, &error) && error.isEmpty(),
                       "release: true 无错误");
    TestHarness::check(!store.leaseFor(kAgent).has_value(), "release: 租约清空");
    TestHarness::check(rec.released == 1 && rec.releasedOwner == kAgent,
                       "release: onAssignmentReleased(agent)");
    TestHarness::check(store.assignmentVersion(kAgent) == 1,
                       "release: 版本保持 1（lite 不在释放时递增——偏离登记）");
    // 幂等：无租约再释放 → false + 'no active assignment'
    TestHarness::check(!store.releaseCompletedAssignment(kAgent, &error)
                           && error.contains(QStringLiteral("no active assignment")),
                       "release 幂等: 无租约 → false");

    // 换工后再次 claim：版本 1→2，advanced 回调累计 2
    claimKernel(store, t2, kAgent, &ok, &error);
    TestHarness::check(ok && store.assignmentVersion(kAgent) == 2 && rec.advanced == 2,
                       "二次 claim: 版本自增至 2、advanced 回调累计");

    // owner 不符：s13 文本带 '; cannot complete' 后缀；'None' 呈现（手写无主 in_progress）
    QString wrongOwner;
    TestHarness::check(store.completeTask(t2, QStringLiteral("zed"), &wrongOwner, &error)
                           && wrongOwner
                                  == QStringLiteral("Task %1 is owned by agent, not zed; cannot complete")
                                         .arg(t2),
                       "complete owner 不符 → s13 全称文本");
    const QString unowned = QStringLiteral("task_00000020");
    TestHarness::check(writeRawTask(root, unowned,
                                    legacyTaskJson(unowned, QStringLiteral("u"),
                                                   QStringLiteral("in_progress"),
                                                   QStringLiteral("null"))),
                       "夹具: in_progress 无主文件写入成功");
    QString noneOwner;
    TestHarness::check(store.completeTask(unowned, QStringLiteral("zed"), &noneOwner, &error)
                           && noneOwner == QStringLiteral("Task %1 is owned by None, not zed; "
                                                          "cannot complete")
                                                 .arg(unowned),
                       "complete: owner=None 以 'None' 呈现");

    // 状态门先于 owner 门：t1 已 completed，用错 owner complete 报状态而非归属
    QString statusFirst;
    TestHarness::check(store.completeTask(t1, QStringLiteral("zed"), &statusFirst, &error)
                           && statusFirst == QStringLiteral("Task %1 is completed, cannot complete").arg(t1),
                       "complete: 状态门先于 owner 门");

    // 解锁链：complete t2 → 释放 → claim t3，t4 blockedBy t3 → complete t3 报 Unblocked
    // 重建修复：事故前旅程含「t2 成功完成」一步（b11：t2 complete+release+t3 claim…），
    // 重放遗漏导致级联——租约指向未完成的 t2，释放必拒、t3 撞门③、Unblocked 永不可达
    const QString done2 = completeKernel(store, t2, kAgent, &ok, &error);
    TestHarness::check(ok && done2 == QStringLiteral("Completed %1 (job-two)").arg(t2),
                       "前置: complete t2 成功");
    TestHarness::check(store.releaseCompletedAssignment(kAgent, &error), "前置: 释放 t2");
    const QString t3 = newTaskId(store, QStringLiteral("job-three"));
    const QString t4 = newTaskId(store, QStringLiteral("job-four"));
    {
        QJsonObject args = idArgs(t4);
        QJsonArray blockedBy;
        blockedBy.append(t3);
        args.insert(QStringLiteral("addBlockedBy"), blockedBy);
        store.runUpdateTask(args);
    }
    claimKernel(store, t3, kAgent, &ok, &error);
    const QString done3 = completeKernel(store, t3, kAgent, &ok, &error);
    TestHarness::check(ok && done3.contains(QStringLiteral("\nUnblocked: job-four")),
                       "complete 解锁链: '\\nUnblocked: …' 段落");
}

// ---------------------------------------------------------------------------
// 组5：complete 的租约自愈（legacy 无租约路径 claim 的任务）
// ---------------------------------------------------------------------------
static void testLeaseSelfHeal()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();

    Recorder rec;
    TaskStore store([root] { return root; });
    store.setOnAssignmentAdvanced(
        [&rec](const QString &, const QString &) { ++rec.advanced; });

    // legacy 折叠入口 claim：s10 原文本（无括号）、不建租约、不递增版本
    const QString t1 = newTaskId(store, QStringLiteral("legacy-job"));
    const QString legacyClaim = store.runClaimTask(idArgs(t1));
    TestHarness::check(legacyClaim == QStringLiteral("Claimed %1 legacy-job").arg(t1),
                       "legacy claim: s10 原文本 'Claimed <id> <subject>' 无括号");
    TestHarness::check(!store.leaseFor(kAgent).has_value() && store.assignmentVersion(kAgent) == 0,
                       "legacy claim: 不建租约、版本不动（向后兼容铁律）");

    // kernel complete 自愈：无租约 → resolveTaskCwd 重建{taskId,回落 cwd}；
    // 有意偏离登记：自愈不递增版本、不发 advanced 回调（防 TOCTOU 快照误复位）
    bool ok = false;
    QString error;
    const QString healed = completeKernel(store, t1, kAgent, &ok, &error);
    TestHarness::check(ok && healed == QStringLiteral("Completed %1 (legacy-job)").arg(t1),
                       "自愈: kernel complete 对 legacy claim 任务成功");
    const std::optional<TaskStore::Lease> lease = store.leaseFor(kAgent);
    TestHarness::check(lease.has_value() && lease->taskId == t1 && lease->cwd == root,
                       "自愈: 租约重建{taskId,cwd=回落}");
    TestHarness::check(store.assignmentVersion(kAgent) == 0 && rec.advanced == 0,
                       "自愈: 不递增版本、不发 advanced（偏离登记钉桩）");

    // 自愈失败（resolver 报错）→ 业务文本、租约缺失、磁盘不动
    TaskStore storeB([root] { return root; });
    const QString t2 = newTaskId(storeB, QStringLiteral("broken-job"));
    storeB.runClaimTask(idArgs(t2));
    storeB.setCwdResolver([](const QString &, QString *err) {
        *err = QStringLiteral("binding is broken");
        return QString();
    });
    QString healFail;
    TestHarness::check(storeB.completeTask(t2, kAgent, &healFail, &error)
                           && healFail
                                  == QStringLiteral("Task %1 cannot complete: binding is broken")
                                         .arg(t2),
                       "自愈失败: 'Task <id> cannot complete: <err>' 业务文本");
    TestHarness::check(!storeB.leaseFor(kAgent).has_value(), "自愈失败: 不建租约");
    TestHarness::check(storeB.runGetTask(idArgs(t2)).contains(
                           QStringLiteral("\"status\": \"in_progress\"")),
                       "自愈失败: 磁盘保持 in_progress");

    // 租约指向他任任务 → complete 时改指向（lcc :370-375 assignment.get != task 分支）
    TaskStore storeD([root] { return root; });
    const QString d1 = newTaskId(storeD, QStringLiteral("d-one"));
    const QString d2 = newTaskId(storeD, QStringLiteral("d-two"));
    claimKernel(storeD, d1, kAgent, &ok, &error); // 租约 → d1
    storeD.runClaimTask(idArgs(d2));              // legacy 旁路：磁盘 d2 in_progress/agent，租约不动
    const QString d2Done = completeKernel(storeD, d2, kAgent, &ok, &error);
    TestHarness::check(ok && d2Done.startsWith(QStringLiteral("Completed ")),
                       "改指向: complete d2 成功（租约原指向 d1）");
    TestHarness::check(storeD.leaseFor(kAgent)->taskId == d2, "改指向: 租约重指到 d2");
}

// ---------------------------------------------------------------------------
// 组6：releaseTeammateAssignment（队友死亡/退出清理路径）
// ---------------------------------------------------------------------------
static void testReleaseTeammateJourney()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    Recorder rec;
    TaskStore store([root] { return root; });
    store.setOnAssignmentReleased([&rec](const QString &owner) {
        ++rec.released;
        rec.releasedOwner = owner;
    });

    // worker1：claim 后直接释放（未 complete）→ 遗留 in_progress 降级 pending、owner 清空
    bool ok = false;
    QString error;
    const QString w1Task = newTaskId(store, QStringLiteral("w1-task"));
    claimKernel(store, w1Task, QStringLiteral("worker1"), &ok, &error);
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("worker1"), &error)
                           && error.isEmpty(),
                       "releaseTeammate: true 无错误");
    const QString w1Disk = store.runGetTask(idArgs(w1Task));
    TestHarness::check(w1Disk.contains(QStringLiteral("\"status\": \"pending\""))
                           && w1Disk.contains(QStringLiteral("\"owner\": null")),
                       "releaseTeammate: 遗留 in_progress → pending + owner 置 null");
    TestHarness::check(!store.leaseFor(QStringLiteral("worker1")).has_value(),
                       "releaseTeammate: 租约清空");
    TestHarness::check(rec.released == 1 && rec.releasedOwner == QStringLiteral("worker1"),
                       "releaseTeammate: onAssignmentReleased 被调");
    TestHarness::check(store.assignmentVersion(QStringLiteral("worker1")) == 1,
                       "releaseTeammate: 版本保持（lite 不递增——偏离登记）");

    // worker2：claim + complete 后释放 → completed 不回退
    const QString w2Task = newTaskId(store, QStringLiteral("w2-task"));
    claimKernel(store, w2Task, QStringLiteral("worker2"), &ok, &error);
    completeKernel(store, w2Task, QStringLiteral("worker2"), &ok, &error);
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("worker2"), &error),
                       "releaseTeammate(worker2): true");
    TestHarness::check(store.runGetTask(idArgs(w2Task)).contains(
                           QStringLiteral("\"status\": \"completed\"")),
                       "releaseTeammate: completed 任务不被回退（只降级 in_progress）");

    // 幽灵 owner：什么账都没有 → 幂等 true，但 finally 语义回调照发（lcc 原样）
    const int beforeGhost = rec.released;
    TestHarness::check(store.releaseTeammateAssignment(QStringLiteral("ghost"), &error)
                           && error.isEmpty(),
                       "releaseTeammate(幽灵): 幂等 true");
    TestHarness::check(rec.released == beforeGhost + 1,
                       "releaseTeammate(幽灵): released 回调仍无条件触发（lcc finally 语义）");

    // 崩溃恢复场景：storeA 有内存租约后“进程死亡”，全新 storeB 内存空账（D7 fail-closed），
    // 但 release 仍必须把磁盘遗留任务降级——清理不依赖内存台账存在
    TaskStore storeA([root] { return root; });
    const QString w3Task = newTaskId(storeA, QStringLiteral("w3-task"));
    claimKernel(storeA, w3Task, QStringLiteral("worker3"), &ok, &error);
    TaskStore storeB([root] { return root; }); // 模拟重启：租约/版本全空
    TestHarness::check(!storeB.leaseFor(QStringLiteral("worker3")).has_value(),
                       "D7 钉桩: 新进程内存租约为空");
    TestHarness::check(storeB.releaseTeammateAssignment(QStringLiteral("worker3"), &error),
                       "重启后 release: 不依赖内存租约仍执行");
    TestHarness::check(storeB.runGetTask(idArgs(w3Task)).contains(
                           QStringLiteral("\"status\": \"pending\"")),
                       "重启后 release: 磁盘 in_progress 仍被降级 pending");
}

// ---------------------------------------------------------------------------
// 组7：双 owner 互不干扰
// ---------------------------------------------------------------------------
static void testTwoOwnersIndependent()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });
    bool ok = false;
    QString error;

    const QString a1 = newTaskId(store, QStringLiteral("a-one"));
    const QString a2 = newTaskId(store, QStringLiteral("a-two"));
    const QString a3 = newTaskId(store, QStringLiteral("a-three"));
    claimKernel(store, a1, QStringLiteral("o1"), &ok, &error);
    claimKernel(store, a2, QStringLiteral("o2"), &ok, &error);
    TestHarness::check(store.leaseFor(QStringLiteral("o1"))->taskId == a1
                           && store.leaseFor(QStringLiteral("o2"))->taskId == a2,
                       "双 owner: 租约各自独立");
    TestHarness::check(store.assignmentVersion(QStringLiteral("o1")) == 1
                           && store.assignmentVersion(QStringLiteral("o2")) == 1,
                       "双 owner: 版本各自计数");

    // o1 走 complete+release 全程，o2 不受任何影响
    completeKernel(store, a1, QStringLiteral("o1"), &ok, &error);
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("o1"), &error),
                       "双 owner: o1 释放成功");
    TestHarness::check(store.leaseFor(QStringLiteral("o2"))->taskId == a2,
                       "双 owner: o1 释放不碰 o2 租约");
    TestHarness::check(!store.releaseCompletedAssignment(QStringLiteral("o2"), &error),
                       "双 owner: o2 未 complete → 释放被拒");

    // o1 换工：版本 1→2；o2 恒 1
    claimKernel(store, a3, QStringLiteral("o1"), &ok, &error);
    TestHarness::check(store.assignmentVersion(QStringLiteral("o1")) == 2
                           && store.assignmentVersion(QStringLiteral("o2")) == 1,
                       "双 owner: o1 换工版本 2、o2 不受牵连");
}

// ---------------------------------------------------------------------------
// 组8：run_* 折叠包装（P3 接线面：Lead 传 "agent"、队友传邮箱名）
// ---------------------------------------------------------------------------
static void testFoldedWrappers()
{
    ScopedTempRoot tmp("taskstore-lease");
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return;
    }
    const QString root = tmp.path();
    TaskStore store([root] { return root; });

    const QString t1 = newTaskId(store, QStringLiteral("fold-one"));
    // 折叠成功文本以承重前缀 'Claimed ' 开头（跨模块 startswith 判成功契约）
    const QString claimed = store.runClaimTaskLeased(idArgs(t1), kAgent);
    TestHarness::check(claimed.startsWith(QStringLiteral("Claimed ")),
                       "runClaimTaskLeased: 'Claimed ' 承重前缀");
    TestHarness::check(claimed == QStringLiteral("Claimed %1 (fold-one)").arg(t1),
                       "runClaimTaskLeased: 全文 'Claimed id (subject)'");

    // 内核 error 通道 → 折叠 'Error: ' 前缀
    TestHarness::check(store.runClaimTaskLeased(idArgs(QStringLiteral("task_ffffffff")), kAgent)
                           .startsWith(QStringLiteral("Error: ")),
                       "runClaimTaskLeased(不存在): 'Error: ' 前缀折叠");

    const QString completed = store.runCompleteTaskLeased(idArgs(t1), kAgent);
    TestHarness::check(completed.startsWith(QStringLiteral("Completed ")),
                       "runCompleteTaskLeased: 'Completed ' 文本");
    TestHarness::check(store.leaseFor(kAgent).has_value(),
                       "runCompleteTaskLeased: 租约仍在（折叠层不破坏不释放铁律）");

    // 门否决走业务通道：文本=否决原因本身，不得带 'Error:' 前缀
    TestHarness::check(store.releaseCompletedAssignment(kAgent, nullptr), "前置: 释放 t1");
    const QString t2 = newTaskId(store, QStringLiteral("fold-two"));
    store.runClaimTaskLeased(idArgs(t2), kAgent);
    store.setPlanGateCheck([](const QString &, QString *reason) {
        *reason = QStringLiteral("Plan still pending approval");
        return false;
    });
    const QString vetoed = store.runCompleteTaskLeased(idArgs(t2), kAgent);
    TestHarness::check(vetoed == QStringLiteral("Plan still pending approval")
                           && !vetoed.startsWith(QStringLiteral("Error:")),
                       "门否决: 原因文本原样、非 'Error:' 通道");
}

// ============================================================================
// 套件入口（编排者收口时在 tests/main.cpp 注册调用；本 lane 无权改 main）
// ============================================================================
int tst_taskstore_lease()
{
    const int before = TestHarness::failCount();
    testWorktreeFieldCompat();
    testClaimSixGates();
    testClaimGate4FailClosed();
    testCompleteAndReleaseJourney();
    testLeaseSelfHeal();
    testReleaseTeammateJourney();
    testTwoOwnersIndependent();
    testFoldedWrappers();
    return TestHarness::failCount() - before;
}
