// tst_worktree.cpp —— WorktreeManager（lcc s13 34775c8 worktree_manager.py 移植，Lane B）单测。
//
// 覆盖面：
//  · 名字正则边界（合法含点/64 字符；拒内嵌 '..'、单独 '.'/'..'、非字母数字首字符、65 字符、路径分隔符）；
//  · create 成功链（目录+分支+任务绑定=名字非路径+注册表可查+resolveWorktreeCwd 指回）；
//  · create 拒绝门（任务不存在/非 pending 无主/名字被别任务占用/非仓库 toplevel/分支已存在）；
//  · resolveWorktreeCwd：空绑定早退（零 git 进程）、破损绑定 fail-closed 钉死文案；
//  · remove 五连门（活跃任务拒/租约在手指向拒/脏拒/干净成功+分支保留）；
//  · discardChanges=true 只豁免脏门（借 --force 移除脏 worktree）；
//  · 注册表解析对含空格路径安全（porcelain partition 首空格）；
//  · FIND-C 大小写口径：盘符大小写翻转的 sink 读同一注册表/台账不产生假 miss
//    （注册表键查找/根过滤/m1 canonical 复校；无盘符形态整组诚实 SKIP）；
//  · FIND-B 非目录伪装：文件顶替 worktree 目录 → registeredEntry 判 missing、
//    resolver fail-closed 钉死串、remove 门①拒、注册表 isDir 过滤剔除；
//  · Gate③ MINOR-4 异步壳 startCreateWorktreeAsync：门拒走 done 同步折叠（同栈可证），
//    同名并发 in-flight QSet 承重拒止（同步径/异步径双向互斥），信号驱动终局
//    成功文案 + 注册表/绑定回读 + 收口后在途名出清（第三击不再报 in progress）。
//
// 需要 git 可用 + LITE_TEST_TMPROOT 合规，二者缺一整组 SKIP（返回 0，不假造通过）。
// 夹具 = 临时根下 git init 的真实小仓库（git worktree 语义只有真 git 能证）。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "AgentConstants.h" // kWorktreesDirName 单源（拼法断言用）
#include "WorktreeManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

namespace {

// git 可用性探针（一次性）；测试进程内补 author 身份 env（新 init 仓库无 user.name/email
// 时 commit 会失败——env 注入只影响本测试拉起的子进程，不碰全局配置）
bool probeGit()
{
    static const bool available = []() {
        QProcess proc;
        proc.start(QStringLiteral("git"), {QStringLiteral("--version")});
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(10000))
            return false;
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    }();
    return available;
}

// 在 dir 下跑 git（列表参数无 shell），收集并流输出；返回是否成功
bool runGitIn(const QString &dir, const QStringList &args, QString *out)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("lite-harness-test"));
    env.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), QStringLiteral("test@example.invalid"));
    env.insert(QStringLiteral("GIT_COMMITTER_NAME"), QStringLiteral("lite-harness-test"));
    env.insert(QStringLiteral("GIT_COMMITTER_EMAIL"), QStringLiteral("test@example.invalid"));
    proc.setProcessEnvironment(env);
    proc.setWorkingDirectory(dir);
    proc.start(QStringLiteral("git"), args);
    if (!proc.waitForStarted(10000) || !proc.waitForFinished(60000)) {
        if (out)
            *out = proc.errorString();
        return false;
    }
    if (out)
        *out = QString::fromUtf8(proc.readAll()).trimmed();
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// runCreateTask 成功文本 'Created <id>: <subject>' → 取 id
// 生产锚（TaskStore::runCreateTask）：id token 形如 'task_<hex8>:'，尾冒号随
// section(' ',1,1) 一并取回——必须剥除，否则门① 'Task …: not found' 级联全红
// （实证：tst_worktree 首轮 20 FAIL 唯一根因，探针复现）。
// id 本身（task_<hex8>）无冒号，token 内唯一冒号即尾随者。
QString taskIdFromCreated(const QString &text)
{
    return text.section(QLatin1Char(' '), 1, 1).remove(QLatin1Char(':'));
}

// 按 id 查快照（M3 唯一视图）；found=false = 不在台账
bool findSnapshot(const TaskStore &store, const QString &taskId, TaskStore::TaskSnapshot *out)
{
    QVector<TaskStore::TaskSnapshot> snapshots;
    QString error;
    if (!store.listTaskSnapshots(&snapshots, &error))
        return false;
    for (const TaskStore::TaskSnapshot &snapshot : snapshots) {
        if (snapshot.id == taskId) {
            *out = snapshot;
            return true;
        }
    }
    return false;
}

// 夹具：临时根下建 git 仓库（repo）+ 会话根（repo/.lite-harness/sessions/t1，
// 满足「会话根 ⊂ workDir」的路径门）+ 装配 TaskStore/WorktreeManager + P3 同款
// cwdResolver 接线（堆对象 unique_ptr 输出——两引擎均 explicit 构造且互相引用，栈占位不可行）。
// 返回 false = 夹具未成，调用方跳过后续断言。
bool makeFixture(const QString &root, std::unique_ptr<TaskStore> *store,
                 std::unique_ptr<WorktreeManager> *manager, QString *repo)
{
    *repo = QDir(root).filePath(QStringLiteral("repo"));
    QDir().mkpath(*repo);
    const QString session = QDir(*repo).filePath(QStringLiteral(".lite-harness/sessions/t1"));
    QDir().mkpath(session);

    QString out;
    if (!runGitIn(*repo, {QStringLiteral("init"), QStringLiteral("-q")}, &out)
        || !runGitIn(*repo, {QStringLiteral("commit"), QStringLiteral("-q"),
                             QStringLiteral("--allow-empty"), QStringLiteral("-m"),
                             QStringLiteral("seed")}, &out)) {
        return false; // git init/seed commit 未成——不假造通过
    }

    const QString repoCopy = *repo;
    const QString sessionCopy = session;
    store->reset(new TaskStore([sessionCopy]() { return sessionCopy; },
                               [repoCopy]() { return repoCopy; }));
    manager->reset(new WorktreeManager(store->get(), [sessionCopy]() { return sessionCopy; },
                                       [repoCopy]() { return repoCopy; }));
    // P3 接线形态（gate① M4）：TaskStore cwdResolver ← WorktreeManager::resolveWorktreeCwd
    WorktreeManager *rawManager = manager->get();
    store->get()->setCwdResolver([rawManager](const TaskStore::TaskSnapshot &task, QString *error) {
        return rawManager->resolveWorktreeCwd(task, error);
    });
    return true;
}

// ---------------------------------------------------------------------------
// 名字正则边界（纯函数，无 fs）
// ---------------------------------------------------------------------------
void testNameRegex()
{
    // 合法：首字符字母数字，中段允许 . _ -，1~64 字符
    TestHarness::check(WorktreeManager::isValidWorktreeName(QStringLiteral("a")), "单字符名合法");
    TestHarness::check(WorktreeManager::isValidWorktreeName(QStringLiteral("v1.2-beta_x")), "含点/横线/下划线合法");
    TestHarness::check(WorktreeManager::isValidWorktreeName(QString(64, QLatin1Char('z'))), "64 字符边界合法");
    TestHarness::check(WorktreeManager::isValidWorktreeName(QStringLiteral("9lead")), "数字开头合法");

    // 非法：内嵌 ".."（负向先行断言）、单独 '.'/'..'（首字符规则）、非字母数字开头、超长、分隔符、非 ASCII
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("a..b")), "内嵌 .. 拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral(".")), "单独 . 拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("..")), "单独 .. 拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("-lead")), "非字母数字开头拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QString(65, QLatin1Char('a'))), "65 字符拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("a/b")), "正斜杠拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("a\\b")), "反斜杠拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QStringLiteral("sp ace")), "空格拒绝");
    TestHarness::check(!WorktreeManager::isValidWorktreeName(QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87")), "中文拒绝");
}

// ---------------------------------------------------------------------------
// resolveWorktreeCwd：空绑定早退（协议核心：零 git 进程、零错误）
// ---------------------------------------------------------------------------
void testResolveEarlyReturn(WorktreeManager &manager)
{
    TaskStore::TaskSnapshot task;
    task.id = QStringLiteral("t-early");
    QString error = QStringLiteral("sentinel"); // 预置脏值验证「成功路径清错」
    const QString cwd = manager.resolveWorktreeCwd(task, &error);
    TestHarness::check(cwd.isEmpty(), "空绑定返回空串（交 TaskStore 回落链）");
    TestHarness::check(error.isEmpty(), "空绑定不置错");
}

// ---------------------------------------------------------------------------
// create 成功链 + 注册表 + resolver 指回 + 拒绝门若干
// ---------------------------------------------------------------------------
void testCreateChain(TaskStore &store, WorktreeManager &manager, const QString &repo)
{
    const QString created = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                             QStringLiteral("do work")}});
    const QString taskId = taskIdFromCreated(created);
    TestHarness::check(!taskId.isEmpty(), "夹具任务已建");

    const QString text = manager.createWorktree(QStringLiteral("w1"), taskId);
    TestHarness::check(text.startsWith(QStringLiteral("Created worktree 'w1' at ")),
                       "成功文案锚（lcc :312）");
    TestHarness::check(text.contains(taskId), "成功文案带 task id");

    QString path;
    QString pathError;
    TestHarness::check(manager.worktreePath(QStringLiteral("w1"), &path, &pathError), "路径三关放行");
    TestHarness::check(QFileInfo(path).isDir(), "worktree 目录已检出");
    TestHarness::check(text == QStringLiteral("Created worktree 'w1' at %1 for task %2").arg(path, taskId),
                       "成功文案 path/id 逐字");

    QString out;
    TestHarness::check(runGitIn(repo, {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                                       QStringLiteral("refs/heads/wt/w1")}, &out),
                       "分支 wt/w1 已建");

    // 绑定存「名字」非路径（gate① M1）
    TaskStore::TaskSnapshot snap;
    TestHarness::check(findSnapshot(store, taskId, &snap), "快照可读");
    TestHarness::check(snap.worktree == QStringLiteral("w1"), "worktree 字段=名字（M1）");

    // 注册表查询面
    QString regError;
    const QMap<QString, WorktreeManager::Entry> registered = manager.registeredWorktrees(&regError);
    TestHarness::check(regError.isEmpty(), "注册表可读无错");
    TestHarness::check(registered.contains(QStringLiteral("w1")), "w1 在在册表");
    TestHarness::check(manager.isWorktreeRegistered(QStringLiteral("w1")), "isWorktreeRegistered 真");
    TestHarness::check(!manager.isWorktreeRegistered(QStringLiteral("ghost")), "未注册名假");

    // resolver 指回 worktree 目录
    QString resolveError;
    const QString cwd = manager.resolveWorktreeCwd(snap, &resolveError);
    TestHarness::check(cwd == QDir::cleanPath(path), "resolveWorktreeCwd 指回目录");
    TestHarness::check(resolveError.isEmpty(), "解析无错");

    // ---- 拒绝门 ----
    // 门①：任务不存在
    const QString notFound = manager.createWorktree(QStringLiteral("nx"), QStringLiteral("nope123"));
    TestHarness::check(notFound == QStringLiteral("Error: Task nope123 not found"), "门① not found 逐字");

    // 门②：claimed（in_progress+owner）任务
    const QString created2 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("claimed")}});
    const QString id2 = taskIdFromCreated(created2);
    QString claimResult;
    QString claimError;
    TestHarness::check(store.claimTask(id2, QStringLiteral("solo"), &claimResult, &claimError)
                           && claimResult.startsWith(QStringLiteral("Claimed ")),
                       "门②夹具认领成功");
    const QString notPending = manager.createWorktree(QStringLiteral("np"), id2);
    TestHarness::check(notPending == QStringLiteral("Error: Task %1 must be pending and unowned").arg(id2),
                       "门② pending&unowned 逐字");

    // 门③：pending&unowned 任务已绑别的 worktree → 拒。
    // （原形在已 claim 的 id2 上 setWorktree 后断言门③是时序缺陷：门序②先于③，
    //   in_progress 任务永不可达门③——门②拦截（实证 [diag]：actual=must be pending
    //   and unowned）。setWorktree 无状态门（TaskStore.cpp:724 C3 钉桩），pending 即可直绑。）
    const QString created5 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("bound-binder")}});
    const QString id5 = taskIdFromCreated(created5);
    QString bindError;
    TestHarness::check(store.setWorktree(id5, QStringLiteral("other-wt"), &bindError)
                           && bindError.isEmpty(),
                       "门③夹具绑定");
    const QString rebinding = manager.createWorktree(QStringLiteral("w-new"), id5);
    TestHarness::check(rebinding == QStringLiteral("Error: Task %1 already uses worktree 'other-wt'").arg(id5),
                       "门③ already uses 逐字");

    // 门④：名字被别的任务占用
    const QString created3 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("third")}});
    const QString id3 = taskIdFromCreated(created3);
    const QString taken = manager.createWorktree(QStringLiteral("other-wt"), id3);
    TestHarness::check(taken == QStringLiteral("Error: Worktree 'other-wt' is already bound to another task"),
                       "门④ bound to another 逐字");

    // 门⓪：坏名折叠文案（合一正则：统一第一条 lcc 文案，登记偏差）
    const QString badName = manager.createWorktree(QStringLiteral("a..b"), id3);
    TestHarness::check(badName.startsWith(QStringLiteral("Error: worktree name must be 1-64")),
                       "门⓪ 坏名文案锚");
}

// ---------------------------------------------------------------------------
// 非仓库 toplevel 拒（门⑥）：独立小夹具（repo2 是普通目录）
// ---------------------------------------------------------------------------
void testNonToplevel(const QString &root)
{
    const QString repo2 = QDir(root).filePath(QStringLiteral("repo2"));
    QDir().mkpath(repo2);
    const QString session2 = QDir(repo2).filePath(QStringLiteral(".lite-harness/s2"));
    QDir().mkpath(session2);

    const QString repo2Copy = repo2;
    const QString session2Copy = session2;
    TaskStore store2([session2Copy]() { return session2Copy; },
                     [repo2Copy]() { return repo2Copy; });
    WorktreeManager manager2(&store2, [session2Copy]() { return session2Copy; },
                             [repo2Copy]() { return repo2Copy; });
    const QString created = store2.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("x")}});
    const QString id = taskIdFromCreated(created);
    const QString text = manager2.createWorktree(QStringLiteral("toplevel-check"), id);
    TestHarness::check(text == QStringLiteral("Error: Working directory must be the root of a Git repository"),
                       "门⑥ 非 toplevel 逐字");
}

// ---------------------------------------------------------------------------
// 破损绑定 fail-closed（钉死文案，M1）+ 门⑧分支已存在（remove 后重建同名）
// 与 remove 五连门共用时序：w1 上跑 claim→active 拒→complete→lease 拒→
// release→dirty 拒→commit 干净→成功移除（分支保留）→重建同名校验门⑧
// ---------------------------------------------------------------------------
void testRemoveLifecycle(TaskStore &store, WorktreeManager &manager, const QString &repo)
{
    // 找回 w1 绑定的任务（testCreateChain 建的第一条）
    QVector<TaskStore::TaskSnapshot> snapshots;
    QString listError;
    if (!store.listTaskSnapshots(&snapshots, &listError)) {
        TestHarness::check(false, "remove 夹具：台账不可读");
        return;
    }
    QString taskId;
    for (const TaskStore::TaskSnapshot &snapshot : snapshots) {
        if (snapshot.worktree == QStringLiteral("w1")) {
            taskId = snapshot.id;
            break;
        }
    }
    if (taskId.isEmpty()) {
        TestHarness::check(false, "remove 夹具：w1 绑定任务缺失");
        return;
    }

    QString path;
    QString pathError;
    TestHarness::check(manager.worktreePath(QStringLiteral("w1"), &path, &pathError), "remove 夹具路径可解");

    // 破损绑定 fail-closed：给别的任务绑一个不存在的名字，resolver 必须置钉死错误
    const QString createdGhost = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                                  QStringLiteral("ghosty")}});
    const QString ghostId = taskIdFromCreated(createdGhost);
    QString ghostBindError;
    TestHarness::check(store.setWorktree(ghostId, QStringLiteral("nope"), &ghostBindError), "破夹具绑定落地");
    TaskStore::TaskSnapshot ghostSnap;
    TestHarness::check(findSnapshot(store, ghostId, &ghostSnap), "破快照可读");
    QString brokenError;
    const QString brokenCwd = manager.resolveWorktreeCwd(ghostSnap, &brokenError);
    TestHarness::check(brokenCwd.isEmpty(), "破损绑定返回空串");
    TestHarness::check(brokenError == QStringLiteral("worktree 'nope' is not available for task %1").arg(ghostId),
                       "破损绑定钉死文案（M1）");

    // ---- claim → 租约指向 worktree ----
    QString claimResult;
    QString claimError;
    TestHarness::check(store.claimTask(taskId, QStringLiteral("w1owner"), &claimResult, &claimError)
                           && claimResult.startsWith(QStringLiteral("Claimed ")),
                       "认领 w1 任务成功（lease cwd=resolver 值）");
    TestHarness::check(!claimResult.startsWith(QStringLiteral("Cannot claim")), "认领未被破损 resolver 拦截");

    // 门②③：活跃（in_progress）任务绑定 → 拒
    QString error;
    const bool activeReject = manager.removeWorktree(QStringLiteral("w1"), false, &error);
    TestHarness::check(!activeReject, "remove 门③：活跃任务拒");
    TestHarness::check(error == QStringLiteral("Error: Worktree 'w1' is bound to active task %1; complete it before removal").arg(taskId),
                       "remove 门③ 文案逐字");

    // complete：s13 铁律「完成故意不释放租约」→ 门④仍应拒
    QString completeResult;
    TestHarness::check(store.completeTask(taskId, QStringLiteral("w1owner"), &completeResult, &claimError)
                           && claimError.isEmpty(),
                       "完成 w1 任务成功");
    const bool leaseReject = manager.removeWorktree(QStringLiteral("w1"), false, &error);
    TestHarness::check(!leaseReject, "remove 门④：租约在手指向拒");
    TestHarness::check(error.contains(QStringLiteral("still in use by w1owner; wait for the turn to end")),
                       "remove 门④ 文案（owners 逗号列）");

    // 回合边界释放租约
    QString releaseError;
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("w1owner"), &releaseError),
                       "释放已完成租约成功");

    // 门⑤：脏（worktree 目录里写未跟踪文件）→ 拒且计数；命令本身没坏
    {
        QFile dirty(QDir(path).filePath(QStringLiteral("dirt.txt")));
        TestHarness::check(dirty.open(QIODevice::WriteOnly), "脏夹具文件可写");
        dirty.write("dirt");
        dirty.close();
    }
    const bool dirtyReject = manager.removeWorktree(QStringLiteral("w1"), false, &error);
    TestHarness::check(!dirtyReject, "remove 门⑤：脏拒");
    TestHarness::check(error.startsWith(QStringLiteral("Error: Worktree 'w1' has "))
                           && error.contains(QStringLiteral("uncommitted change(s); preserve or discard them manually")),
                       "remove 门⑤ 文案（1 change 计数形）");
    TestHarness::check(QFileInfo(path).isDir(), "脏拒后目录仍在（宁拒不删）");

    // 提交变干净 → 成功移除；分支保留、绑定清空、目录消失
    QString out;
    TestHarness::check(runGitIn(path, {QStringLiteral("add"), QStringLiteral("-A")}, &out)
                           && runGitIn(path, {QStringLiteral("commit"), QStringLiteral("-q"),
                                             QStringLiteral("-m"), QStringLiteral("dirt")}, &out),
                       "脏夹具已在 worktree 内提交");
    const bool removed = manager.removeWorktree(QStringLiteral("w1"), false, &error);
    TestHarness::check(removed, "remove 成功");
    TestHarness::check(error == QStringLiteral("Worktree 'w1' removed; branch 'wt/w1' retained"),
                       "成功文案走 *error（钉死形）");
    TestHarness::check(!QFileInfo(path).exists(), "worktree 目录已移除");
    TestHarness::check(runGitIn(repo, {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                                        QStringLiteral("refs/heads/wt/w1")}, &out),
                       "分支永不删除（lcc :385）");
    TaskStore::TaskSnapshot afterSnap;
    TestHarness::check(findSnapshot(store, taskId, &afterSnap) && afterSnap.worktree.isEmpty(),
                       "解绑落地（worktree 字段清空）");

    // 门⑧：分支还在、目录没了 → **新 pending 任务**同名重建被「分支已存在」拒。
    // （原形用上方已 completed 的 taskId 复验是设计缺陷：门② pending&unowned 先于门⑧
    //   拦截，永远到不了分支门——复验必须造一个能穿过门①~⑦的新任务。）
    const QString created4 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("reuse")}});
    const QString id4 = taskIdFromCreated(created4);
    const QString recreate = manager.createWorktree(QStringLiteral("w1"), id4);
    TestHarness::check(recreate == QStringLiteral("Error: Branch 'wt/w1' already exists"),
                       "门⑧ 分支已存在逐字");
}

// ---------------------------------------------------------------------------
// discardChanges=true 只豁免脏门（①~④寸步不让已由上文覆盖；此处脏+force 成功）
// ---------------------------------------------------------------------------
void testDiscardChanges(TaskStore &store, WorktreeManager &manager)
{
    const QString created = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                             QStringLiteral("discardme")}});
    const QString taskId = taskIdFromCreated(created);
    const QString text = manager.createWorktree(QStringLiteral("d1"), taskId);
    if (!text.startsWith(QStringLiteral("Created worktree"))) {
        TestHarness::check(false, "discard 夹具 create 未成");
        return;
    }
    QString path;
    QString pathError;
    TestHarness::check(manager.worktreePath(QStringLiteral("d1"), &path, &pathError), "discard 路径可解");

    // 脏 + 租约已释放 + 任务 completed → 门⑤本应拒，discardChanges=true 豁免之并追加 --force。
    // （①~④寸步不让已由 testRemoveLifecycle 的门序覆盖，此处不重复。）
    QString claimResult;
    QString claimError;
    TestHarness::check(store.claimTask(taskId, QStringLiteral("downer"), &claimResult, &claimError)
                           && claimResult.startsWith(QStringLiteral("Claimed ")), "discard 夹具认领");
    QString completeResult;
    TestHarness::check(store.completeTask(taskId, QStringLiteral("downer"), &completeResult, &claimError),
                       "discard 夹具完成");
    QString releaseError;
    TestHarness::check(store.releaseCompletedAssignment(QStringLiteral("downer"), &releaseError),
                       "discard 夹具释放");
    {
        QFile dirty(QDir(path).filePath(QStringLiteral("trash.txt")));
        dirty.open(QIODevice::WriteOnly);
        dirty.write("junk");
        dirty.close();
    }
    QString error;
    const bool forced = manager.removeWorktree(QStringLiteral("d1"), true, &error);
    TestHarness::check(forced, "discardChanges=true 豁免脏门成功移除");
    TestHarness::check(error.startsWith(QStringLiteral("Worktree 'd1' removed")), "discard 成功文案");
    TestHarness::check(!QFileInfo(path).exists(), "脏 worktree 目录已 --force 移除");
}

// ---------------------------------------------------------------------------
// 含空格**路径**的注册表解析安全（porcelain partition 首空格 + git 引号形态）。
// 注意：worktree **名字**禁含空格（lcc :33-45 字符集不含空格——原形用 "wt space"
// 名必被门⓪拒，属测试缺陷）。空格压力由仓库目录名 "repo B with space" 供给：
// worktree 全路径 = <root>/repo B with space/.lite-harness/sb/.worktrees/wtb，
// 祖先目录含空格即触发 git porcelain 的引号/原样歧义域。
// ---------------------------------------------------------------------------
void testSpaceInPath(const QString &root)
{
    const QString repoB = QDir(root).filePath(QStringLiteral("repo B with space"));
    QDir().mkpath(repoB);
    const QString sessionB = QDir(repoB).filePath(QStringLiteral(".lite-harness/sb"));
    QDir().mkpath(sessionB);
    QString out;
    if (!runGitIn(repoB, {QStringLiteral("init"), QStringLiteral("-q")}, &out)
        || !runGitIn(repoB, {QStringLiteral("commit"), QStringLiteral("-q"),
                             QStringLiteral("--allow-empty"), QStringLiteral("-m"),
                             QStringLiteral("seed")}, &out)) {
        TestHarness::check(false, "空格夹具 git init 未成");
        return;
    }
    const QString repoBCopy = repoB;
    const QString sessionBCopy = sessionB;
    TaskStore storeB([sessionBCopy]() { return sessionBCopy; },
                     [repoBCopy]() { return repoBCopy; });
    WorktreeManager managerB(&storeB, [sessionBCopy]() { return sessionBCopy; },
                             [repoBCopy]() { return repoBCopy; });
    const QString created = storeB.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("spacy")}});
    const QString taskId = taskIdFromCreated(created);
    const QString text = managerB.createWorktree(QStringLiteral("wtb"), taskId);
    TestHarness::check(text.startsWith(QStringLiteral("Created worktree 'wtb' at ")),
                       "含空格路径 create 成功");

    QString regError;
    const QMap<QString, WorktreeManager::Entry> registered = managerB.registeredWorktrees(&regError);
    TestHarness::check(regError.isEmpty(), "含空格路径注册表无错");
    TestHarness::check(registered.contains(QStringLiteral("wtb")), "含空格路径注册表解析安全");
    // resolve 指回含空格目录（M1 协议面闭环）
    TaskStore::TaskSnapshot snapB;
    if (findSnapshot(storeB, taskId, &snapB)) {
        QString resolveError;
        const QString cwdB = managerB.resolveWorktreeCwd(snapB, &resolveError);
        TestHarness::check(cwdB.contains(QStringLiteral("repo B with space")) && resolveError.isEmpty(),
                           "含空格路径 resolve 指回");
    }
}

// ---------------------------------------------------------------------------
// FIND-C（Gate② minor）大小写口径收口：盘符大小写翻转的 sink（Windows 大小写不敏感
// 文件系统下指向同一目录、字符串口径却不同）读同一真台账/注册表，不得产生假 miss。
// 修复前：registeredWorktrees 根过滤（原 isWithinPath 大小写敏感）把 git OS 拼写的
// 条目假剔除=空表；registeredEntry 的 constFind（QMap 键本征 CS）对翻转拼写派生路径
// 假报 not registered——即 lcc Path.resolve() 键 + python 大小写语义所不存在的问题，
// lite 词法键口径必须由 CI 比较点补足（findCi/withinCi）。root 无盘符形态（如 CI 把
// LITE_TEST_TMPROOT 指成相对串）时压力不可构造 → 整组诚实 SKIP（printf，不假造通过）。
// ---------------------------------------------------------------------------
QString flipDriveCase(const QString &path)
{
    QString flipped = path;
    const QChar head = flipped.at(0);
    flipped[0] = head.isUpper() ? head.toLower() : head.toUpper();
    return flipped;
}

void testDriveCaseRegistry(TaskStore &store, WorktreeManager &manager, const QString &repo)
{
    if (!(repo.size() >= 2 && repo.at(1) == QLatin1Char(':') && repo.at(0).isLetter())) {
        std::printf("SKIP: drive-case shape unavailable (non-drive root)\n");
        return;
    }
    const QString session = QDir(repo).filePath(QStringLiteral(".lite-harness/sessions/t1"));
    const QString flippedRepo = flipDriveCase(repo);
    const QString flippedSession = flipDriveCase(session);

    // ① 主侧建册 c1（压力对象的来源；主拼写=后续 alt 压力的「另一侧」）
    const QString created1 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("drivecase-1")}});
    const QString id1 = taskIdFromCreated(created1);
    const QString text1 = manager.createWorktree(QStringLiteral("c1"), id1);
    if (!text1.startsWith(QStringLiteral("Created worktree 'c1'"))) {
        TestHarness::check(false, "drive-case 夹具：主侧 c1 建册未成");
        return;
    }

    // ② alt 装配：翻转盘符的 sink 字符串读同一批目录（unique_ptr 形态同 makeFixture）
    const QString fRepoCopy = flippedRepo;
    const QString fSessionCopy = flippedSession;
    std::unique_ptr<TaskStore> altStore(new TaskStore([fSessionCopy]() { return fSessionCopy; },
                                                      [fRepoCopy]() { return fRepoCopy; }));
    std::unique_ptr<WorktreeManager> altManager(new WorktreeManager(
        altStore.get(), [fSessionCopy]() { return fSessionCopy; },
        [fRepoCopy]() { return fRepoCopy; }));

    QString altRegError;
    const QMap<QString, WorktreeManager::Entry> altRegistered =
        altManager->registeredWorktrees(&altRegError);
    TestHarness::check(altRegError.isEmpty(), "drive-case：alt 注册表可读无错");
    TestHarness::check(altRegistered.contains(QStringLiteral("c1")),
                       "drive-case：alt 注册表根过滤不假剔（withinCi 口径）");
    TestHarness::check(altManager->isWorktreeRegistered(QStringLiteral("c1")),
                       "drive-case：alt isWorktreeRegistered 命中（名键 CI 扫描）");

    // ③ alt resolver：registeredEntry findCi 命中；且 m1 canonical 复校不得因盘符
    //    拼写差假拒（child 以 OS 拼写参与复校、与 helper 内 parent canonical 同源）
    TaskStore::TaskSnapshot snap1;
    if (!findSnapshot(*altStore, id1, &snap1)) {
        TestHarness::check(false, "drive-case 夹具：alt 台账快照不可读");
        return;
    }
    QString altResolveError;
    const QString cwd1 = altManager->resolveWorktreeCwd(snap1, &altResolveError);
    const QString expected1 = QDir::cleanPath(QDir(fSessionCopy).filePath(AgentConst::kWorktreesDirName)
                                             + QLatin1Char('/') + QStringLiteral("c1"));
    TestHarness::check(!cwd1.isEmpty() && altResolveError.isEmpty(),
                       "drive-case：alt resolve 命中（findCi 注册表键查找）");
    TestHarness::check(cwd1.compare(expected1, Qt::CaseInsensitive) == 0,
                       "drive-case：alt resolve 指回（大小写等值）");

    // ④ alt 侧全门通过新建 c2（门⓪~⑩混拼写放行：门⑥ toplevel eqCi、门⑤ OS 天然 CI、
    //    m1 复校同源——「配置了却处处假失配」的正向对照）
    const QString created2 = altStore->runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                                  QStringLiteral("drivecase-2")}});
    const QString id2 = taskIdFromCreated(created2);
    const QString text2 = altManager->createWorktree(QStringLiteral("c2"), id2);
    TestHarness::check(text2.startsWith(QStringLiteral("Created worktree 'c2'")),
                       "drive-case：alt 侧建 c2 成功（混拼写过全门）");

    // ⑤ 反向：git 对 c2 存哪种盘符拼写不保证（OS 归一 or 原样留存），主侧按另一侧
    //    混拼读回必须命中——两向都过才证明口径统一不是单向巧合
    QString mainRegError;
    const QMap<QString, WorktreeManager::Entry> mainRegistered =
        manager.registeredWorktrees(&mainRegError);
    TestHarness::check(mainRegError.isEmpty() && mainRegistered.contains(QStringLiteral("c2")),
                       "drive-case：主侧注册表命中 alt 建册（反向混拼）");
    TaskStore::TaskSnapshot snap2;
    if (findSnapshot(store, id2, &snap2)) {
        QString mainResolveError;
        const QString cwd2 = manager.resolveWorktreeCwd(snap2, &mainResolveError);
        TestHarness::check(!cwd2.isEmpty() && mainResolveError.isEmpty(),
                           "drive-case：主侧 resolve 命中 c2");
    } else {
        TestHarness::check(false, "drive-case 夹具：主侧 c2 快照不可读");
    }
}

// ---------------------------------------------------------------------------
// FIND-B（lcc :154/:137 not path.is_dir()）+ m1 收口断言：worktree 路径退化为普通
// 文件（目录被外部删掉、同名文件顶上）时 registeredEntry 必须按 missing 拒绝——
// 修复前 exists() 把文件误判在世、branch 校验照过 → resolver 交出不可用 cwd（破口
// 本体）。删除三重守卫（非空 + 叶段=fp1 + isDir + 前缀=<会话根>/.worktrees/），守卫
// 不过=一个字节都不删（毁仓教训）；删除属本套件既有 tmp 内脏夹具同族操作（仅编排者
// 执行）。git 未 prune 前注册表仍列 fp1——正是 isDir 过滤与 missing 判定的压力源。
// ---------------------------------------------------------------------------
void testFilePretender(TaskStore &store, WorktreeManager &manager, const QString &repo)
{
    const QString created = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                             QStringLiteral("pretender")}});
    const QString taskId = taskIdFromCreated(created);
    const QString text = manager.createWorktree(QStringLiteral("fp1"), taskId);
    if (!text.startsWith(QStringLiteral("Created worktree 'fp1'"))) {
        TestHarness::check(false, "pretender 夹具：create 未成");
        return;
    }
    QString path;
    QString pathError;
    const QString session = QDir(repo).filePath(QStringLiteral(".lite-harness/sessions/t1"));
    const QString wtRoot = QDir::cleanPath(QDir(session).filePath(AgentConst::kWorktreesDirName));
    if (!manager.worktreePath(QStringLiteral("fp1"), &path, &pathError)
        || path.isEmpty()
        || path.section(QLatin1Char('/'), -1) != QStringLiteral("fp1")
        || !path.startsWith(wtRoot + QLatin1Char('/'), Qt::CaseInsensitive)
        || !QFileInfo(path).isDir()) {
        TestHarness::check(false, "pretender 夹具：路径三重守卫未过");
        return; // 守卫不过 = 绝不删除
    }

    QDir remover(path);
    if (!remover.removeRecursively()) {
        TestHarness::check(false, "pretender 夹具：目录清理未成");
        return;
    }
    QFile pretender(path);
    if (!pretender.open(QIODevice::WriteOnly)) {
        TestHarness::check(false, "pretender 夹具：伪装文件落位未成");
        return;
    }
    pretender.write("not-a-dir");
    pretender.close();

    // resolver fail-closed：registeredEntry 判 missing → 折叠为 M1 钉死串。
    // （修复前 exists() 误判在世 → 此处会交出非空 cwd——本断言是 FIND-B 判别器。）
    TaskStore::TaskSnapshot snap;
    if (findSnapshot(store, taskId, &snap)) {
        QString resolveError;
        const QString cwd = manager.resolveWorktreeCwd(snap, &resolveError);
        TestHarness::check(cwd.isEmpty(), "pretender：resolver 拒绝文件伪装（修复前假放行点）");
        TestHarness::check(resolveError == QStringLiteral("worktree 'fp1' is not available for task %1").arg(taskId),
                           "pretender：fail-closed 钉死文案（M1）");
    } else {
        TestHarness::check(false, "pretender 夹具：快照不可读");
    }

    // remove 门①拒（missing 文案）；公开注册表 isDir 过滤剔除（lcc :137 口径）
    QString removeError;
    const bool removed = manager.removeWorktree(QStringLiteral("fp1"), false, &removeError);
    TestHarness::check(!removed, "pretender：remove 门①拒");
    TestHarness::check(removeError.contains(QStringLiteral("is missing at")),
                       "pretender：missing 文案在");
    QString regError;
    const QMap<QString, WorktreeManager::Entry> registered = manager.registeredWorktrees(&regError);
    TestHarness::check(!registered.contains(QStringLiteral("fp1")),
                       "pretender：注册表 isDir 过滤剔除");
}

// ---------------------------------------------------------------------------
// Gate③ MINOR-4 异步壳 startCreateWorktreeAsync（P4b Lane B）
// 时序纪律：门拒路径的 done 在调用栈内同步触发（可用 fired 标志直接证）；
// 成功路径的 done 只经事件循环送达——发起后**不泵**则必未到，同名第二击
// 因此确定性地落在发起窗口内（in-flight QSet 承重证明），随后有界泵送收口。
// 泵送只用 QCoreApplication::processEvents（tests/main.cpp 裸 main 无 app 实例、
// 属禁改文件——本组内惰性造 scoped 实例、组尾归还；禁 QThread/嵌套 exec）。
// ---------------------------------------------------------------------------
void testAsyncCreateShell(TaskStore &store, WorktreeManager &manager, const QString &repo)
{
    static char progName[] = "lite-harness-tests";
    int argc = 1;
    char *argv[] = {progName, nullptr};
    std::unique_ptr<QCoreApplication> app(
        QCoreApplication::instance() ? nullptr : new QCoreApplication(argc, argv));

    auto pumpUntil = [&app](const std::function<bool()> &ready, int maxMs) {
        QElapsedTimer clock;
        clock.start();
        while (!ready() && clock.elapsed() < maxMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    };

    // ---- 门⓪：非法名 → done 同栈折叠（foldError 逐字）----
    {
        bool firedDuringCall = false;
        QString result;
        manager.startCreateWorktreeAsync(
            QStringLiteral("a..bad"), QStringLiteral("task_deadbeef"),
            [&](const QString &r) { result = r; firedDuringCall = true; });
        TestHarness::check(firedDuringCall, "异步壳门⓪：done 在调用栈内触发");
        TestHarness::check(
            result == QStringLiteral(
                "Error: worktree name must be 1-64 letters, digits, dots, underscores, "
                "or dashes, and start with a letter or digit"),
            "异步壳门⓪：折叠文案逐字");
    }

    // ---- 门①：任务不存在 → done 同步折叠；在途名出清（第二击仍 not found，未卡 in progress）----
    {
        bool fired = false;
        QString result;
        manager.startCreateWorktreeAsync(QStringLiteral("nxas"), QStringLiteral("nope123"),
                                         [&](const QString &r) { result = r; fired = true; });
        TestHarness::check(fired && result == QStringLiteral("Error: Task nope123 not found"),
                           "异步壳门①：not found 同步折叠");
        bool fired2 = false;
        QString result2;
        manager.startCreateWorktreeAsync(QStringLiteral("nxas"), QStringLiteral("nope123"),
                                         [&](const QString &r) { result2 = r; fired2 = true; });
        TestHarness::check(
            fired2 && result2 == QStringLiteral("Error: Task nope123 not found"),
            "异步壳门①出清：同名第二击仍 not found（未卡在途）");
    }

    // ---- 在途门承重（异步发起 → 同步/异步双向拒止）+ 成功终局 ----
    const QString created = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                             QStringLiteral("async job")}});
    const QString tid = taskIdFromCreated(created);
    TestHarness::check(!tid.isEmpty(), "异步壳夹具：任务已建");

    // 终态写入堆持有状态（shared_ptr）而非栈引用捕获：若 20s 泵送超时时子进程
    // 仍在途，done 可能在后续任意事件泵中晚归——届栈已亡，引用捕获即悬垂（UB）。
    // 断言文本与条数不变，仅状态载体堆化（健壮化，不掩盖任何失败面）。
    const auto terminal = std::make_shared<std::pair<QString, bool>>(QString(), false);
    manager.startCreateWorktreeAsync(QStringLiteral("wa"), tid,
                                     [terminal](const QString &r) {
                                         terminal->first = r;
                                         terminal->second = true;
                                     });
    TestHarness::check(!terminal->second, "异步壳：终态不同步返回（事件环未转）");

    // 同步径命中异步填充的 QSet（承重门双向互斥的『sync 看 async』半区）
    const QString secondSync = manager.createWorktree(QStringLiteral("wa"), tid);
    TestHarness::check(
        secondSync == QStringLiteral("Error: Worktree 'wa' creation is already in progress"),
        "异步壳 in-flight 承重：同步径拒同名第二击逐字");

    bool secondAsyncFired = false;
    QString secondAsyncResult;
    manager.startCreateWorktreeAsync(QStringLiteral("wa"), tid,
                                     [&](const QString &r) { secondAsyncResult = r; secondAsyncFired = true; });
    TestHarness::check(
        secondAsyncFired &&
            secondAsyncResult ==
                QStringLiteral("Error: Worktree 'wa' creation is already in progress"),
        "异步壳 in-flight 承重：异步径拒同名第二击（done 同步折叠）");

    pumpUntil([terminal]() { return terminal->second; }, 20000);
    TestHarness::check(terminal->second, "异步壳终局：done 回调 20s 内送达（信号驱动）");
    TestHarness::check(terminal->first.startsWith(QStringLiteral("Created worktree 'wa' at ")),
                       "异步壳终局：成功文案锚");
    TestHarness::check(terminal->first.contains(tid), "异步壳终局：文案带任务 id");

    QString path;
    QString pathError;
    TestHarness::check(manager.worktreePath(QStringLiteral("wa"), &path, &pathError),
                       "异步壳终局：路径三关放行");
    TestHarness::check(
        terminal->first == QStringLiteral("Created worktree 'wa' at %1 for task %2").arg(path, tid),
        "异步壳终局：文案逐字=同步径锚（finishCreateSuccess 单源）");

    QString gitOut;
    TestHarness::check(runGitIn(repo, {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                                        QStringLiteral("refs/heads/wt/wa")},
                                &gitOut),
                       "异步壳终局：git 分支 wt/wa 已建（真实子进程产物）");

    QString regError;
    const QMap<QString, WorktreeManager::Entry> registered = manager.registeredWorktrees(&regError);
    TestHarness::check(registered.contains(QStringLiteral("wa")), "异步壳终局：注册表在册");
    TestHarness::check(manager.isWorktreeRegistered(QStringLiteral("wa")),
                       "异步壳终局：isWorktreeRegistered 真");
    TaskStore::TaskSnapshot snap;
    TestHarness::check(findSnapshot(store, tid, &snap) && snap.worktree == QStringLiteral("wa"),
                       "异步壳终局：任务绑定=名字");

    // ---- 收口后：同名第三击不再报 in progress（settled 闩出清证明），落门③折叠 ----
    const QString third = manager.createWorktree(QStringLiteral("wa"), tid);
    TestHarness::check(third == QStringLiteral("Error: Task %1 already uses worktree 'wa'").arg(tid),
                       "异步壳出清：收口后 in-flight 已空（第三击落门③而非 in progress）");

    // ---- 门④（异步径）：名字被别任务占用 → done 同步折叠逐字 ----
    const QString created2 = store.runCreateTask(QJsonObject{{QStringLiteral("subject"),
                                                              QStringLiteral("collider")}});
    const QString tid2 = taskIdFromCreated(created2);
    bool collideFired = false;
    QString collideResult;
    manager.startCreateWorktreeAsync(QStringLiteral("wa"), tid2,
                                     [&](const QString &r) { collideResult = r; collideFired = true; });
    TestHarness::check(
        collideFired &&
            collideResult == QStringLiteral("Error: Worktree 'wa' is already bound to another task"),
        "异步壳门④：别任务占用同名同步折叠逐字");
}

} // namespace

int tst_worktree()
{
    ScopedTempRoot tmp(QStringLiteral("worktree"));
    if (!tmp.isValid()) {
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return 0;
    }
    if (!probeGit()) {
        std::printf("SKIP: git unavailable\n");
        return 0;
    }

    const int before = TestHarness::failCount();
    const QString root = tmp.path();

    // 主夹具（堆装配；跨测试函数共享演进状态，按各函数注释时序跑；unique_ptr 自动回收）
    std::unique_ptr<TaskStore> store;
    std::unique_ptr<WorktreeManager> manager;
    QString repo;
    if (!makeFixture(root, &store, &manager, &repo)) {
        std::printf("SKIP: git fixture init failed\n");
        return TestHarness::failCount() - before; // 夹具不成不跑断言，不假造通过
    }

    testNameRegex();
    testResolveEarlyReturn(*manager);
    testCreateChain(*store, *manager, repo);
    testNonToplevel(root);
    testRemoveLifecycle(*store, *manager, repo);
    testDiscardChanges(*store, *manager);
    testSpaceInPath(root);
    testDriveCaseRegistry(*store, *manager, repo);
    testFilePretender(*store, *manager, repo);
    testAsyncCreateShell(*store, *manager, repo);

    return TestHarness::failCount() - before;
}
