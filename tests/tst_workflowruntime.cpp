// s16 工作流执行环单测（lcc s16 42a555d 移植对照；WorkflowRuntime.h/.cpp）
//
// 覆盖：六原语（agent HIT/MISS/脏缓存拒用/唯一重试、parallel 屏障一败俱败只回一次、
// pipeline 段间无屏障与 items 并发、workflow 嵌套共享五本账与深度/未知闸、phase 去重/log）、
// 一次运行状态机（launched 信封、progress 事件族、output/snapshot/last_run 产物、
// resume 全 cached 二跑与四闸串、already-active 抢占串、deny 串不可达说明）、
// review-changes 全环（mock 按 label 分派 findings/verdict → confirmed 过滤 + severity 稳定排序 +
// 恰 N 次调用 1 条结果）、runWorkflowTool 工具边界（"Error: " 折叠族与逐字参数名）。
//
// 形态纪律（偏差10/12 的测试面）：rt.setInlineDispatch(true) 使全部 defer 退化为栈内直调，
// Mock runner 同步在调用栈内调 done——测试零事件循环驱动、零网络、零定时器。
// 并发断言局限（登记）：同步 mock 下 CONCURRENCY=8 计数门不可观测为「并行」，
// 断言面退化为「调用序 + 结果序 + pipeline 手控回调的段间无屏障序」，非真实并发度证明。
//
// 夹具：文件产物组依赖 ScopedTempRoot（LITE_TEST_TMPROOT）；缺失时整组 SKIP 不计失败。
// 嵌套/小型编排经测试 seam overrideReviewChangesScriptForTests 替换 review-changes 执行体，
// 每个用例尾必须还原（RAII guard）——seam 为进程级 static，勿泄漏到其它套件。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "WorkflowRuntime.h"
#include "WorkflowManager.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPair>
#include <QStringList>
#include <QVector>

#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>

namespace {

// ---------------- Mock runner ----------------

struct MockCall
{
    QString prompt;
    QString schemaJson;
    QString label;
};

struct MockRunner
{
    QList<MockCall> calls;
    qint64 tokensPerCall = 7;
    QString forceError; // 非空 = 每次调用都以 API 层错误回（lcc 裸调用异常折叠面）
    bool manual = false;
    // manual 模式的在途回调（label + done），reply 逐个放行
    QList<QPair<QString, std::function<void(const QJsonValue &, qint64, const QString &)>>> pending;
    // 应答提供器：(label, 该 label 的第几次调用 1 起) → value
    std::function<QJsonValue(const QString &, int)> responder;

    int countFor(const QString &label) const
    {
        int n = 0;
        for (const MockCall &c : calls)
            if (c.label == label)
                ++n;
        return n;
    }

    WfAgentRunner runner()
    {
        return [this](const QString &prompt, const QString &schemaJson, const QString &label,
                      std::function<void(const QJsonValue &, qint64, const QString &)> done) {
            calls.append(MockCall{prompt, schemaJson, label});
            if (!forceError.isEmpty())
            {
                done(QJsonValue(), 0, forceError);
                return;
            }
            if (manual)
            {
                pending.append(qMakePair(label, std::move(done)));
                return;
            }
            const int nth = countFor(label);
            const QJsonValue v = responder ? responder(label, nth) : QJsonValue(QStringLiteral("x"));
            done(v, tokensPerCall, QString());
        };
    }

    void reply(const QString &label, const QJsonValue &value)
    {
        for (int i = 0; i < pending.size(); ++i)
        {
            if (pending[i].first == label)
            {
                auto d = pending[i].second;
                pending.removeAt(i);
                d(value, tokensPerCall, QString());
                return;
            }
        }
    }
};

// ---------------- 断言/回放小工具 ----------------

struct Recorded
{
    QString out;
    int outCount = 0;
};

// 解析 start() 工具输出（成功=envelope 紧凑 JSON；失败="Error: " 前缀串）
QJsonObject parseEnvelope(const QString &out)
{
    return QJsonDocument::fromJson(out.toUtf8()).object();
}

void startOnce(WorkflowRuntime &rt, WorkflowManager &mgr, const QJsonObject &toolArgs,
               MockRunner &mock, Recorded &rec)
{
    rt.start(mgr, toolArgs, mock.runner(), [&rec](const QString &out) {
        ++rec.outCount;
        rec.out = out;
    });
}

QJsonObject toolArgsFor(const QString &name, const QJsonObject &args)
{
    QJsonObject t;
    t[QStringLiteral("name")] = name;
    t[QStringLiteral("args")] = args;
    return t;
}

QString runIdOf(const Recorded &rec)
{
    return parseEnvelope(rec.out).value(QStringLiteral("task")).toObject()
        .value(QStringLiteral("runId")).toString();
}

QJsonObject resultOf(const Recorded &rec)
{
    return parseEnvelope(rec.out).value(QStringLiteral("result")).toObject();
}

QJsonObject taskOf(const Recorded &rec)
{
    return parseEnvelope(rec.out).value(QStringLiteral("task")).toObject();
}

// 在 progress 数组里按 type 收集条目
QJsonArray progressOfType(const QJsonObject &task, const QString &type)
{
    QJsonArray got;
    const QJsonArray progress = task.value(QStringLiteral("progress")).toArray();
    for (const QJsonValue &e : progress)
        if (e.toObject().value(QStringLiteral("type")).toString() == type)
            got.append(e);
    return got;
}

// ---------------- 测试脚本工厂（override seam 用） ----------------

// 简单 object schema：{"type":"object","required":["ok"],"properties":{"ok":{"type":"boolean"}}}
QJsonObject okSchema()
{
    return QJsonDocument::fromJson(
               "{\"type\":\"object\",\"required\":[\"ok\"],"
               "\"properties\":{\"ok\":{\"type\":\"boolean\"}}}")
        .object();
}

// 单 agent 小脚本：phase("T") + agent(prompt,label) → 值直接透传为 result
WfScriptFn oneAgentScript(const QString &prompt, const QJsonValue &schema, const QString &label)
{
    return [prompt, schema, label](WfExecState &ctx, const QJsonObject &, const WfDone &done) {
        auto self = ctx.shared_from_this();
        self->phase(QStringLiteral("T"));
        self->agent(prompt, schema, label, QString(), done);
    };
}

// seam RAII：设覆盖、离开作用域必还原（进程级 static，防跨用例泄漏）
struct OverrideGuard
{
    explicit OverrideGuard(const WfScriptFn &fn) { WorkflowRuntime::overrideReviewChangesScriptForTests(fn); }
    ~OverrideGuard() { WorkflowRuntime::overrideReviewChangesScriptForTests(WfScriptFn()); }
};

// ---------------- 无夹具组：WfExecutionLimits 直测 ----------------

void testLimitsDirect()
{
    WfExecutionLimits lim;
    bool firstOk = false;
    bool lastOk = true;
    QString lastErr;
    bool allOkBefore = true;
    for (int i = 1; i <= kWorkflowAgentCap + 1; ++i)
    {
        QString err;
        const bool ok = lim.claimAgent(&err);
        if (i == 1)
            firstOk = ok;
        if (i <= kWorkflowAgentCap && !ok)
            allOkBefore = false;
        if (i == kWorkflowAgentCap + 1)
        {
            lastOk = ok;
            lastErr = err;
        }
    }
    TestHarness::check(firstOk && allOkBefore, "cap: 前 1000 次 claimAgent 全过");
    TestHarness::check(!lastOk && lastErr == QStringLiteral("agent() cap reached (1000)"),
                       "cap: 第 1001 次失败且文案逐字（lcc :388）");
    TestHarness::check(lim.agentsUsed() == kWorkflowAgentCap + 1,
                       "cap: 先自增再判限——失败后停在 cap+1（lcc :386-387）");
}

// ---------------- 夹具组：一次运行状态机 + 六原语 ----------------

void testSingleRunEndToEnd(const QString &root)
{
    OverrideGuard guard(oneAgentScript(
        QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789"), QJsonValue(), QString()));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock;
    mock.responder = [](const QString &, int) { return QJsonValue(QStringLiteral("ok")); };
    Recorded rec;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);
    }

    TestHarness::check(rec.outCount == 1, "E2E: 终局回调恰一次");
    TestHarness::check(!rec.out.startsWith(QStringLiteral("Error:")), "E2E: 成功路径非 Error 前缀");
    const QJsonObject env = parseEnvelope(rec.out);
    const QJsonObject launched = env.value(QStringLiteral("launched")).toObject();
    const QJsonObject task = env.value(QStringLiteral("task")).toObject();
    const QString runId = task.value(QStringLiteral("runId")).toString();

    TestHarness::check(launched.value(QStringLiteral("status")).toString() == QStringLiteral("async_launched")
                           && launched.value(QStringLiteral("taskType")).toString() == QStringLiteral("local_workflow"),
                       "E2E: launched 信封 status/taskType 逐字（lcc :815-817）");
    TestHarness::check(launched.value(QStringLiteral("runId")).toString() == runId
                           && launched.value(QStringLiteral("workflowName")).toString() == QStringLiteral("review-changes"),
                       "E2E: launched 的 runId/workflowName 与 task 一致");
    TestHarness::check(launched.value(QStringLiteral("taskId")).toString()
                           == WorkflowManager::createTaskId(runId),
                       "E2E: taskId = local_workflow_{runId}（lcc create_task_id）");
    TestHarness::check(task.value(QStringLiteral("status")).toString() == QStringLiteral("completed"),
                       "E2E: task.status=completed（lcc :833）");
    TestHarness::check(task.value(QStringLiteral("usage")).toObject().value(QStringLiteral("agents")).toInt() == 1
                           && task.value(QStringLiteral("usage")).toObject().value(QStringLiteral("tokens")).toInt() == 7,
                       "E2E: usage agents=1 tokens=7");
    TestHarness::check(env.value(QStringLiteral("result")).toString() == QStringLiteral("ok"),
                       "E2E: result 透传 agent 值");

    // 默认 label 与 runner 收到形
    TestHarness::check(mock.calls.size() == 1, "E2E: runner 恰一次调用");
    TestHarness::check(mock.calls[0].label
                           == QStringLiteral("abcdefghijklmnopqrstuvwx..."),
                       "E2E: 默认 label = prompt 前 24 字符 + \"...\"（lcc :424）");
    TestHarness::check(mock.calls[0].schemaJson == QStringLiteral("null"),
                       "E2E: 无 schema 时 runner 收 canonical \"null\"");

    // progress 事件族
    const QJsonArray phases = progressOfType(task, QStringLiteral("workflow_phase"));
    TestHarness::check(phases.size() == 1
                           && phases[0].toObject().value(QStringLiteral("title")).toString() == QStringLiteral("T"),
                       "E2E: workflow_phase 同名只播一次（lcc :409-415）");
    const QJsonArray agents = progressOfType(task, QStringLiteral("workflow_agent"));
    TestHarness::check(agents.size() == 1
                           && agents[0].toObject().value(QStringLiteral("status")).toString() == QStringLiteral("done"),
                       "E2E: workflow_agent status=done（lcc :465）");

    // 文件产物三件套
    TestHarness::check(QFile::exists(storeDir + QStringLiteral("/") + runId + QStringLiteral(".json")),
                       "E2E: <runId>.json 快照落盘");
    // 标量 result 的落盘形由 Lane A writeJsonAtomic 决定（QJsonDocument 不收顶层标量），
    // 内容等值断言放到对象 result 的全环组；此处只验存在与非空。
    const QString outputPath = storeDir + QStringLiteral("/") + runId + QStringLiteral(".output.json");
    bool outputNonEmpty = false;
    {
        QFile of(outputPath);
        if (of.open(QIODevice::ReadOnly))
            outputNonEmpty = !of.readAll().trimmed().isEmpty();
    }
    TestHarness::check(outputNonEmpty, "E2E: <runId>.output.json 落盘且非空（lcc :843）");
    QFile lr(storeDir + QStringLiteral("/last_run.txt"));
    QString lastRun;
    if (lr.open(QIODevice::ReadOnly))
        lastRun = QString::fromUtf8(lr.readAll()).trimmed();
    TestHarness::check(lastRun == runId, "E2E: last_run.txt = runId（lcc _saveLastRun）");
}

void testResumeAllCached(const QString &root)
{
    const QString prompt = QStringLiteral("cached-prompt");
    OverrideGuard guard(oneAgentScript(prompt, QJsonValue(), QStringLiteral("L1")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock1;
    mock1.responder = [](const QString &, int) { return QJsonValue(true); };
    Recorded rec1;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock1, rec1);
    }
    const QString runId = runIdOf(rec1);
    TestHarness::check(mock1.calls.size() == 1, "resume: 首跑 runner 一次");

    MockRunner mock2;
    Recorded rec2;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        QJsonObject toolArgs;
        toolArgs[QStringLiteral("name")] = QStringLiteral("review-changes");
        toolArgs[QStringLiteral("resume_from_run_id")] = runId; // args 省略 = 沿用快照 args
        startOnce(rt, mgr, toolArgs, mock2, rec2);
    }

    TestHarness::check(rec2.outCount == 1 && !rec2.out.startsWith(QStringLiteral("Error:")),
                       "resume: 二跑成功闭环");
    TestHarness::check(mock2.calls.isEmpty(), "resume: HIT 全命中，runner 零调用（lcc :436-440）");
    TestHarness::check(resultOf(rec2) == resultOf(rec1), "resume: result 与首跑逐字一致");
    const QJsonObject usage = taskOf(rec2).value(QStringLiteral("usage")).toObject();
    TestHarness::check(usage.value(QStringLiteral("agents")).toInt() == 0
                           && usage.value(QStringLiteral("tokens")).toInt() == 0,
                       "resume: cached 步零入账 agents/tokens=0");
    const QJsonArray agents = progressOfType(taskOf(rec2), QStringLiteral("workflow_agent"));
    TestHarness::check(agents.size() == 1
                           && agents[0].toObject().value(QStringLiteral("status")).toString() == QStringLiteral("cached"),
                       "resume: workflow_agent status=cached（lcc :439-440）");
    // task_started 的 resume 旗标（日志面走 event，产物面走快照——此处验证信封仍带 launched）
    TestHarness::check(parseEnvelope(rec2.out).value(QStringLiteral("launched")).toObject()
                           .value(QStringLiteral("runId")).toString() == runId,
                       "resume: launched.runId = 被续跑的 id");
}

void testDirtyCachedReject(const QString &root)
{
    const QString prompt = QStringLiteral("dirty-probe");
    OverrideGuard guard(oneAgentScript(prompt, okSchema(), QStringLiteral("D1")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock1;
    mock1.responder = [](const QString &, int) {
        return QJsonValue(QJsonObject{{QStringLiteral("ok"), true}});
    };
    Recorded rec1;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock1, rec1);
    }
    const QString runId = runIdOf(rec1);
    TestHarness::check(taskOf(rec1).value(QStringLiteral("status")).toString() == QStringLiteral("completed"),
                       "脏缓存: 首跑正常完成");

    // 用同 key 的脏值改写 journal（label/prompt/schema 与首跑一致 → key 必命中）
    const QString key = WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("D1"),
                                                  prompt, okSchema());
    QFile jf(storeDir + QStringLiteral("/") + runId + QStringLiteral(".journal.jsonl"));
    bool rewrote = false;
    if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        const QJsonObject line{{QStringLiteral("key"), key},
                               {QStringLiteral("value"), QStringLiteral("dirty")}};
        jf.write(QJsonDocument(line).toJson(QJsonDocument::Compact));
        jf.write("\n");
        // 必须显式 close：QIODevice 缓冲未 flush 前，二跑 WfJournal::open
        // 的独立 probe 句柄只会读到 Truncate 后的空文件（resume cache 恒空）。
        jf.close();
        rewrote = true;
    }
    TestHarness::check(rewrote, "脏缓存: journal 改写成功");

    MockRunner mock2;
    Recorded rec2;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        QJsonObject toolArgs;
        toolArgs[QStringLiteral("name")] = QStringLiteral("review-changes");
        toolArgs[QStringLiteral("resume_from_run_id")] = runId;
        startOnce(rt, mgr, toolArgs, mock2, rec2);
    }
    TestHarness::check(mock2.calls.isEmpty(), "脏缓存: 拒用后不回退重跑（零 runner 调用）");
    TestHarness::check(resultOf(rec2).value(QStringLiteral("error")).toString()
                           .startsWith(QStringLiteral("workflow: cached agent output failed schema validation:")),
                       "脏缓存: 承重串逐字前缀（lcc :434）");
    TestHarness::check(taskOf(rec2).value(QStringLiteral("status")).toString() == QStringLiteral("failed"),
                       "脏缓存: 终态 failed 也落产物（§10-3）");
    TestHarness::check(rec2.outCount == 1, "脏缓存: 终局恰一次");
}

void testSchemaRetryOnce(const QString &root)
{
    OverrideGuard guard(oneAgentScript(QStringLiteral("retry-me"), okSchema(), QStringLiteral("RT")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock;
    mock.responder = [](const QString &, int nth) {
        if (nth == 1)
            return QJsonValue(QStringLiteral("not json")); // 解析失败回原文=两层分工（lcc :274-279）
        return QJsonValue(QJsonObject{{QStringLiteral("ok"), true}});
    };
    Recorded rec;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);
    }
    TestHarness::check(mock.calls.size() == 2, "重试: 唯一一次重试=恰 2 次调用（lcc :449-452）");
    TestHarness::check(mock.calls[1].prompt
                           == mock.calls[0].prompt + QStringLiteral("\n\nReturn valid JSON."),
                       "重试: 续打提示串逐字（lcc :450）");
    TestHarness::check(mock.calls[1].schemaJson == mock.calls[0].schemaJson,
                       "重试: schema 原样随行（runner 侧重拼由宿主适配）");
    TestHarness::check(rec.outCount == 1
                           && taskOf(rec).value(QStringLiteral("status")).toString() == QStringLiteral("completed"),
                       "重试: 第二次通过→completed");
    const QJsonObject usage = taskOf(rec).value(QStringLiteral("usage")).toObject();
    TestHarness::check(usage.value(QStringLiteral("agents")).toInt() == 1
                           && usage.value(QStringLiteral("tokens")).toInt() == 14,
                       "重试: agents 计 1、tokens 两次累加（lcc :452/:462-463）");
}

void testSchemaRetryStillBad(const QString &root)
{
    OverrideGuard guard(oneAgentScript(QStringLiteral("doomed"), okSchema(), QStringLiteral("BAD")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock; // 永远回坏值
    Recorded rec;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);
    }
    TestHarness::check(mock.calls.size() == 2, "重试仍败: 只重试一次");
    TestHarness::check(resultOf(rec).value(QStringLiteral("error")).toString()
                           .startsWith(QStringLiteral("workflow: agent(...) invalid output:")),
                       "重试仍败: 承重串形态逐字（lcc :455）");
    TestHarness::check(rec.outCount == 1
                           && taskOf(rec).value(QStringLiteral("status")).toString() == QStringLiteral("failed"),
                       "重试仍败: failed 终局恰一次");
}

void testBudgetGates(const QString &root)
{
    OverrideGuard guard(oneAgentScript(QStringLiteral("budget-me"), QJsonValue(), QStringLiteral("B")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    // 闸一：remaining()<=0 拒绝——零调用
    {
        MockRunner mock;
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("budget"), 0}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(mock.calls.isEmpty(), "budget=0: runner 零调用（lcc :429）");
        TestHarness::check(resultOf(rec).value(QStringLiteral("error")).toString()
                               == QStringLiteral("workflow: token budget exceeded"),
                           "budget=0: 拒绝串逐字（lcc :429）");
    }
    // 闸二：入账超支——Budget.add 折叠
    {
        MockRunner mock;
        mock.tokensPerCall = 50;
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("budget"), 10}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(resultOf(rec).value(QStringLiteral("error")).toString()
                               .contains(QStringLiteral("token budget exceeded (50 > 10)")),
                           "超支: 折叠串含 (spent > total)（Lane A WfBudget :345）");
        TestHarness::check(rec.outCount == 1, "超支: 终局恰一次");
    }
    // 偏差16：非数值 budget 视为不限
    {
        MockRunner mock;
        mock.tokensPerCall = 50;
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("budget"), QStringLiteral("ten")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(taskOf(rec).value(QStringLiteral("status")).toString() == QStringLiteral("completed"),
                           "非数值 budget: 视为不限（偏差16 登记）");
    }
}

void testParallelBarrier(const QString &root)
{
    struct ParScript
    {
        static WfScriptFn make()
        {
            return [](WfExecState &ctx, const QJsonObject &, const WfDone &done) {
                auto self = ctx.shared_from_this();
                std::vector<WfThunk> thunks;
                for (int i = 0; i < 3; ++i)
                {
                    const QString p = QStringLiteral("p%1").arg(i);
                    const QString l = QStringLiteral("L%1").arg(i);
                    thunks.push_back([self, p, l](const WfDone &d) {
                        self->agent(p, QJsonValue(), l, QString(), d);
                    });
                }
                self->parallel(thunks, done);
            };
        }
    };
    OverrideGuard guard(ParScript::make());

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    // 全成：结果按 thunk 序
    {
        MockRunner mock;
        mock.responder = [](const QString &label, int) {
            return QJsonValue(label + QStringLiteral("v"));
        };
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);
        const QJsonArray arr = QJsonDocument::fromJson(rec.out.toUtf8()).object()
                                   .value(QStringLiteral("result")).toArray();
        TestHarness::check(arr.size() == 3 && arr[0].toString() == QStringLiteral("L0v")
                               && arr[1].toString() == QStringLiteral("L1v") && arr[2].toString() == QStringLiteral("L2v"),
                           "parallel: 全成按 thunk 序收口（lcc :468-470）");
        TestHarness::check(rec.outCount == 1, "parallel: 终局恰一次");
    }

    // 一败俱败且只回一次：thunk0 先败后，迟到者丢弃、零额外 runner 调用
    {
        MockRunner mock;
        mock.forceError = QStringLiteral("boom");
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);
        TestHarness::check(mock.calls.size() == 1, "parallel: 首败后 settled→后续零调用");
        TestHarness::check(rec.outCount == 1
                               && resultOf(rec).value(QStringLiteral("error")).toString() == QStringLiteral("workflow: boom"),
                           "parallel: 一败即整体败、只回一次（settled 门闩）");
    }
}

void testPipelineOrdering(const QString &root)
{
    OverrideGuard guard([](WfExecState &ctx, const QJsonObject &, const WfDone &done) {
        auto self = ctx.shared_from_this();
        const WfStage s0 = [self](const QJsonValue &, const QJsonValue &item, int, const WfDone &d) {
            const QString s = item.toString();
            self->agent(QStringLiteral("s0-") + s, QJsonValue(), QStringLiteral("s0:") + s, QString(), d);
        };
        const WfStage s1 = [self](const QJsonValue &prev, const QJsonValue &item, int, const WfDone &d) {
            const QString s = item.toString(); // label 挂 item 不挂 prev：手控 reply 键稳定
            self->agent(QStringLiteral("s1-") + prev.toString(), QJsonValue(),
                        QStringLiteral("s1:") + s, QString(), d);
        };
        const QJsonArray items{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        self->pipeline(items, {s0, s1}, done);
    });

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    MockRunner mock;
    mock.manual = true;
    Recorded rec;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), mock, rec);

        // items 并发穿段：三个 s0 全部先派发（同步 mock 下的并发退化断言形，头注释登记局限）
        TestHarness::check(mock.calls.size() == 3 && rec.outCount == 0,
                           "pipeline: items 间无屏障，s0 全先派发");
        mock.reply(QStringLiteral("s0:c"), QStringLiteral("vc"));
        // c 先行穿段：s1:c 必须已派发而 a/b 的 s1 尚未——段间无屏障（lcc :472-481）
        TestHarness::check(mock.calls.size() == 4 && mock.calls[3].label == QStringLiteral("s1:c"),
                           "pipeline: 先完成的 item 立刻穿下一段");
        mock.reply(QStringLiteral("s0:a"), QStringLiteral("va"));
        mock.reply(QStringLiteral("s0:b"), QStringLiteral("vb"));
        TestHarness::check(mock.calls.size() == 6, "pipeline: a/b 随后穿段");
        mock.reply(QStringLiteral("s1:c"), QStringLiteral("Rc"));
        mock.reply(QStringLiteral("s1:a"), QStringLiteral("Ra"));
        mock.reply(QStringLiteral("s1:b"), QStringLiteral("Rb"));
    }
    TestHarness::check(rec.outCount == 1, "pipeline: 终局恰一次");
    const QJsonArray arr = parseEnvelope(rec.out).value(QStringLiteral("result")).toArray();
    TestHarness::check(arr.size() == 3 && arr[0].toString() == QStringLiteral("Ra")
                           && arr[1].toString() == QStringLiteral("Rb") && arr[2].toString() == QStringLiteral("Rc"),
                       "pipeline: 结果按 item 原序（完成序无关）");
    TestHarness::check(mock.pending.isEmpty(), "pipeline: 手控回调全部放行");
}

// 嵌套脚本：args.mode 驱动 parent/child/deep/unknown 四形
WfScriptFn nestingScript()
{
    return [](WfExecState &ctx, const QJsonObject &args, const WfDone &done) {
        auto self = ctx.shared_from_this();
        const QString mode = args.value(QStringLiteral("mode")).toString();
        if (mode == QStringLiteral("parent"))
        {
            self->agent(QStringLiteral("pa"), QJsonValue(), QStringLiteral("PA"), QString(),
                        [self, done](const QJsonValue &, const QString &e) {
                            if (!e.isEmpty())
                            {
                                done(QJsonValue(), e);
                                return;
                            }
                            self->workflow(QStringLiteral("review-changes"),
                                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("child")}},
                                           done);
                        });
            return;
        }
        if (mode == QStringLiteral("child"))
        {
            self->agent(QStringLiteral("ca"), QJsonValue(), QStringLiteral("CA"), QString(),
                        [done](const QJsonValue &v, const QString &e) {
                            QJsonObject r;
                            r[QStringLiteral("child")] = v;
                            done(r, e);
                        });
            return;
        }
        if (mode == QStringLiteral("deep"))
        {
            // depth0 → workflow(deep1) 允许；deep1 里再 workflow → 深度闸
            self->workflow(QStringLiteral("review-changes"),
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("deep1")}}, done);
            return;
        }
        if (mode == QStringLiteral("deep1"))
        {
            self->workflow(QStringLiteral("review-changes"),
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("child")}}, done);
            return;
        }
        if (mode == QStringLiteral("unknown"))
        {
            self->workflow(QStringLiteral("nope"), QJsonObject(), done);
            return;
        }
        done(QJsonValue(QStringLiteral("plain")), QString());
    };
}

void testNesting(const QString &root)
{
    OverrideGuard guard(nestingScript());

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    // 子共享 task/journal：父 1 agent + 子 1 agent，同一次运行台账
    {
        MockRunner mock;
        mock.responder = [](const QString &label, int) { return QJsonValue(label); };
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("mode"), QStringLiteral("parent")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(mock.calls.size() == 2 && mock.calls[0].label == QStringLiteral("PA")
                               && mock.calls[1].label == QStringLiteral("CA"),
                           "嵌套: 父→子两笔调用同环续跑");
        const QJsonObject usage = taskOf(rec).value(QStringLiteral("usage")).toObject();
        TestHarness::check(usage.value(QStringLiteral("agents")).toInt() == 2,
                           "嵌套: 子共享父 task 台账（五本账合一）");
        TestHarness::check(resultOf(rec).value(QStringLiteral("child")).toString() == QStringLiteral("CA"),
                           "嵌套: 子结果回父");
        TestHarness::check(!runIdOf(rec).isEmpty(), "嵌套: 父子的账落在同一次运行（单 runId）");
    }

    // 深度闸：depth>=1 再嵌套 → "workflow() nesting is one level only"
    {
        MockRunner mock;
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("mode"), QStringLiteral("deep")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(rec.outCount == 1
                               && resultOf(rec).value(QStringLiteral("error")).toString() ==
                                      QStringLiteral("workflow: workflow() nesting is one level only"),
                           "嵌套: 深度闸串逐字（lcc :485）");
    }

    // 查无子名
    {
        MockRunner mock;
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("mode"), QStringLiteral("unknown")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
        TestHarness::check(mock.calls.isEmpty()
                               && resultOf(rec).value(QStringLiteral("error")).toString() ==
                                      QStringLiteral("workflow: unknown workflow 'nope'"),
                           "嵌套: 查无串逐字（lcc :487）");
    }
}

// ---------------- review-changes 全环（内置脚本，不设 override） ----------------

QJsonObject findingsArr(std::initializer_list<std::pair<const char *, const char *>> fs)
{
    QJsonArray arr;
    for (const auto &f : fs)
        arr.append(QJsonObject{{QStringLiteral("title"), QString::fromUtf8(f.first)},
                               {QStringLiteral("severity"), QString::fromUtf8(f.second)}});
    return QJsonObject{{QStringLiteral("findings"), arr}};
}

QJsonObject verdict(bool real)
{
    return QJsonObject{{QStringLiteral("isReal"), real},
                       {QStringLiteral("reason"), QStringLiteral("r")}};
}

void testReviewChangesFullRing(const QString &root)
{
    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);
    // 策略闸说明：Lane A 内置策略表 review-changes=allow、无 deny 条目——
    // "denied by host policy" 串在内置注册表下 E2E 不可达（闸代码存在，属不可证区，冒烟兜底）。
    TestHarness::check(mgr.policyFor(QStringLiteral("review-changes")) == QStringLiteral("allow"),
                       "策略: 内置表 review-changes=allow（deny 形 E2E 不可达，注释登记）");

    MockRunner mock;
    mock.responder = [](const QString &label, int) -> QJsonValue {
        if (label == QStringLiteral("audit:correctness"))
            return findingsArr({{"c1", "low"}, {"c2", "high"}});
        if (label == QStringLiteral("audit:security"))
            return findingsArr({{"s1", "medium"}});
        if (label.startsWith(QStringLiteral("audit:")))
            return findingsArr({});
        if (label == QStringLiteral("verify:correctness:c2"))
            return verdict(true);
        if (label == QStringLiteral("verify:security:s1"))
            return verdict(true);
        return verdict(false); // verify:correctness:c1 等 → 非真
    };
    Recorded rec;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("changes"),
                                QStringLiteral("def f(x):\n    return db.query(x)")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), mock, rec);
    }

    // 恰 4 audit + 3 verify = 7 次调用、一条工具结果
    TestHarness::check(mock.calls.size() == 7 && rec.outCount == 1,
                       "全环: 9 段结构收敛为 7 次调用 1 条结果（维度 4 + 发现 3）");
    TestHarness::check(taskOf(rec).value(QStringLiteral("usage")).toObject()
                           .value(QStringLiteral("agents")).toInt() == 7,
                       "全环: agents=7 全入账");

    // confirmed 过滤 + severity 稳定排序 + dimension 字段回挂
    const QJsonArray confirmed = resultOf(rec).value(QStringLiteral("confirmed")).toArray();
    bool shapeOk = confirmed.size() == 2
                   && confirmed[0].toObject().value(QStringLiteral("title")).toString() == QStringLiteral("c2")
                   && confirmed[0].toObject().value(QStringLiteral("dimension")).toString() == QStringLiteral("correctness")
                   && confirmed[1].toObject().value(QStringLiteral("title")).toString() == QStringLiteral("s1")
                   && confirmed[1].toObject().value(QStringLiteral("dimension")).toString() == QStringLiteral("security");
    TestHarness::check(shapeOk, "全环: isReal 过滤 + high<medium 排序 + dimension 回挂（lcc :569-574）");

    // phase 去重：Review/Verify 各一次；workflow_log 汇总行
    const QJsonObject task = taskOf(rec);
    QJsonArray ph = progressOfType(task, QStringLiteral("workflow_phase"));
    QStringList titles;
    for (const QJsonValue &e : ph)
        titles.append(e.toObject().value(QStringLiteral("title")).toString());
    TestHarness::check(titles.size() == 2 && titles.contains(QStringLiteral("Review"))
                           && titles.contains(QStringLiteral("Verify")),
                       "全环: workflow_phase Review/Verify 各恰一次");
    const QJsonArray logs = progressOfType(task, QStringLiteral("workflow_log"));
    bool logOk = false;
    for (const QJsonValue &e : logs)
        if (e.toObject().value(QStringLiteral("message")).toString()
                == QStringLiteral("confirmed 2 real finding(s)"))
            logOk = true;
    TestHarness::check(logOk, "全环: 汇总 log 串逐字（lcc :573）");

    // audit/verify 提示逐字（抽查 correctness）：label 与 prompt 模板
    bool auditPromptOk = false;
    for (const MockCall &c : mock.calls)
    {
        if (c.label == QStringLiteral("audit:correctness"))
            auditPromptOk = c.prompt ==
                            QStringLiteral("Review this change context for correctness issues. "
                                           "Report only issues supported by the supplied text.\n\n")
                                + QStringLiteral("def f(x):\n    return db.query(x)");
        if (c.label == QStringLiteral("verify:correctness:c1"))
            TestHarness::check(c.prompt.startsWith(QStringLiteral(
                                   "Adversarially verify this correctness finding against the supplied "
                                   "change context.\n\nChange context:\n")) &&
                                   c.prompt.contains(QStringLiteral("Finding:")),
                               "全环: verify 提示逐字头 + Finding 段（lcc :548-552）");
    }
    TestHarness::check(auditPromptOk, "全环: audit 提示逐字（lcc :536-538）");

    // 空 changes → 兜底上下文（第二次独立运行，换新 manager 防台账串扰）
    {
        WorkflowManager mgr2;
        QDir(root).mkpath(QStringLiteral(".workflows2"));
        mgr2.setStoreDir(root + QStringLiteral("/.workflows2"));
        MockRunner m2;
        m2.responder = [](const QString &, int) { return findingsArr({}); };
        Recorded r2;
        WorkflowRuntime rt2;
        rt2.setInlineDispatch(true);
        startOnce(rt2, mgr2, toolArgsFor(QStringLiteral("review-changes"), QJsonObject{}), m2, r2);
        bool fallbackOk = r2.outCount == 1 && m2.calls.size() == 4
                          && m2.calls[0].prompt.contains(QStringLiteral("No change context was supplied."));
        TestHarness::check(fallbackOk, "全环: 缺 changes → 兜底上下文逐字（lcc :533）");
        bool zeroLog = false;
        for (const QJsonValue &e : taskOf(r2).value(QStringLiteral("progress")).toArray())
            if (e.toObject().value(QStringLiteral("message")).toString()
                    == QStringLiteral("confirmed 0 real finding(s)"))
                zeroLog = true;
        TestHarness::check(zeroLog, "全环: 零确认汇总行");
    }

    // changes 非串 = 脚本层错误 → failed 信封（非 "Error:" 前缀，§10-3 错误留结果）
    {
        WorkflowManager mgr3;
        QDir(root).mkpath(QStringLiteral(".workflows3"));
        mgr3.setStoreDir(root + QStringLiteral("/.workflows3"));
        MockRunner m3;
        Recorded r3;
        WorkflowRuntime rt3;
        rt3.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("changes"), 5}};
        startOnce(rt3, mgr3, toolArgsFor(QStringLiteral("review-changes"), args), m3, r3);
        TestHarness::check(m3.calls.isEmpty() && r3.outCount == 1
                               && !r3.out.startsWith(QStringLiteral("Error:"))
                               && resultOf(r3).value(QStringLiteral("error")).toString() ==
                                      QStringLiteral("workflow: args.changes must be a string"),
                           "全环: changes 非串→failed 信封内嵌错误（lcc :531）");
    }
}

// ---------------- 工具边界与闸串（start() 折叠族） ----------------

void testAdapterStrings(const QString &root)
{
    WorkflowManager mgr;
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(root + QStringLiteral("/.workflows"));

    auto call = [&mgr](const QJsonObject &toolArgs, MockRunner &mock) {
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        startOnce(rt, mgr, toolArgs, mock, rec);
        return rec.out;
    };

    {
        MockRunner m;
        TestHarness::check(call(QJsonObject{}, m) == QStringLiteral("Error: workflow name must be a string"),
                           "边界: name 非串折叠串逐字（lcc :861）");
    }
    {
        MockRunner m;
        QJsonObject t;
        t[QStringLiteral("name")] = QStringLiteral("nope");
        TestHarness::check(call(t, m) ==
                               QStringLiteral("Error: unknown workflow 'nope'. Available: review-changes"),
                           "边界: 未知名 + Available 清单逐字（lcc :863-865）");
    }
    {
        MockRunner m;
        QJsonObject t;
        t[QStringLiteral("name")] = QStringLiteral("review-changes");
        t[QStringLiteral("args")] = 5;
        TestHarness::check(call(t, m) == QStringLiteral("Error: workflow args must be an object"),
                           "边界: args 非 object 折叠串逐字（lcc :867-869）");
    }
    {
        MockRunner m;
        QJsonObject t;
        t[QStringLiteral("name")] = QStringLiteral("review-changes");
        t[QStringLiteral("resume_from_run_id")] = QStringLiteral("nope");
        TestHarness::check(call(t, m) == QStringLiteral("Error: invalid workflow runId"),
                           "边界: resume id 非法串逐字（lcc validate_run_id）");
    }
    {
        MockRunner m;
        QJsonObject t;
        t[QStringLiteral("name")] = QStringLiteral("review-changes");
        t[QStringLiteral("resume_from_run_id")] = WorkflowManager::createRunId(QStringLiteral("review-changes"));
        const QString out = call(t, m);
        TestHarness::check(out.startsWith(QStringLiteral("Error: resume snapshot not found for ")),
                           "边界: 快照缺失串逐字（lcc _readSnapshot）");
    }
}

void testResumeGatesAndPreemption(const QString &root)
{
    OverrideGuard guard(oneAgentScript(QStringLiteral("gate-probe"), QJsonValue(), QStringLiteral("G")));

    WorkflowManager mgr;
    const QString storeDir = root + QStringLiteral("/.workflows");
    QDir(root).mkpath(QStringLiteral(".workflows"));
    mgr.setStoreDir(storeDir);

    // 首跑带 args，留下快照（args.changes="A"）
    MockRunner m0;
    m0.responder = [](const QString &, int) { return QJsonValue(QStringLiteral("g")); };
    Recorded rec0;
    {
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        const QJsonObject args{{QStringLiteral("changes"), QStringLiteral("A")}};
        startOnce(rt, mgr, toolArgsFor(QStringLiteral("review-changes"), args), m0, rec0);
    }
    const QString runId = runIdOf(rec0);
    TestHarness::check(rec0.outCount == 1 && !runId.isEmpty(), "闸: 首跑打底成功");

    auto resume = [&mgr, &runId](MockRunner &m, const QJsonObject *args) {
        Recorded rec;
        WorkflowRuntime rt;
        rt.setInlineDispatch(true);
        QJsonObject t;
        t[QStringLiteral("name")] = QStringLiteral("review-changes");
        t[QStringLiteral("resume_from_run_id")] = runId;
        if (args)
            t[QStringLiteral("args")] = *args;
        startOnce(rt, mgr, t, m, rec);
        return rec;
    };

    {
        MockRunner m;
        const QJsonObject bad{{QStringLiteral("changes"), QStringLiteral("B")}};
        TestHarness::check(resume(m, &bad).out ==
                               QStringLiteral("Error: resume args do not match the original run"),
                           "闸: args 不符串逐字且在 journal 截断前（lcc :798-806）");
        TestHarness::check(m.calls.isEmpty(), "闸: args 不符零调用");
    }

    // journal 缺失 → "resume journal not found for"
    {
        const QString journalPath = storeDir + QStringLiteral("/") + runId + QStringLiteral(".journal.jsonl");
        QFile::remove(journalPath);
        MockRunner m;
        const Recorded rec = resume(m, nullptr);
        TestHarness::check(rec.out.startsWith(QStringLiteral("Error: resume journal not found for ")),
                           "闸: journal 缺失串逐字（lcc :296-297）");
    }
    // journal 坏行 → "invalid resume journal record at line 1"
    {
        QFile jf(storeDir + QStringLiteral("/") + runId + QStringLiteral(".journal.jsonl"));
        if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            jf.write("not-json\n");
            jf.close();
        }
        MockRunner m;
        const Recorded rec = resume(m, nullptr);
        TestHarness::check(rec.out.contains(QStringLiteral("invalid resume journal record at line 1")),
                           "闸: 坏行 fail-closed 串逐字（lcc :307-310）");
    }

    // 同 runId 抢占：resume 用 manual mock 挂住在途 → 再次 resume 立刻失败不排队（§10-4）。
    // 挂住法：journal 重置为空文件（零行合法、缓存空）→ 唯一 agent 走 MISS → manual 扣住 done。
    {
        const QString journalPath = storeDir + QStringLiteral("/") + runId + QStringLiteral(".journal.jsonl");
        QFile::remove(journalPath);
        {
            QFile jf(journalPath);
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jf.close(); // 空文件 = 合法零行
        }

        MockRunner holder;
        holder.manual = true;
        Recorded recH;
        WorkflowRuntime rtA;
        rtA.setInlineDispatch(true);
        {
            QJsonObject t;
            t[QStringLiteral("name")] = QStringLiteral("review-changes");
            t[QStringLiteral("resume_from_run_id")] = runId;
            startOnce(rtA, mgr, t, holder, recH);
        }
        TestHarness::check(holder.calls.size() == 1 && holder.pending.size() == 1 && recH.outCount == 0,
                           "抢占: 挂住的 resume 在途未终局");

        {
            MockRunner second;
            const Recorded rec2 = resume(second, nullptr);
            TestHarness::check(rec2.out ==
                                   QStringLiteral("Error: workflow run %1 is already active").arg(runId),
                               "抢占: already-active 串逐字、不排队（lcc :714/§10-4）");
            TestHarness::check(second.calls.isEmpty(), "抢占: 后到者零 runner 调用");
        }

        // 收口挂住运行：先 cancel（inFlight>0 → 折叠推迟到回调）后放行迟到的 done
        rtA.requestCancel();
        holder.reply(QStringLiteral("G"), QStringLiteral("late"));
        TestHarness::check(recH.outCount == 1
                               && resultOf(recH).value(QStringLiteral("error")).toString() ==
                                      QStringLiteral("workflow: run cancelled"),
                           "抢占: 取消路径终局恰一次（lite 增项，lcc 无停止路径）");

        // 释放后同 id 可再续跑（active 集合已清）——用缓存行让 HIT 直完成
        {
            const QString key = WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("G"),
                                                           QStringLiteral("gate-probe"), QJsonValue());
            QFile jf(journalPath);
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                jf.write(QJsonDocument(QJsonObject{{QStringLiteral("key"), key},
                                                   {QStringLiteral("value"), QStringLiteral("g")}})
                             .toJson(QJsonDocument::Compact));
                jf.write("\n");
                jf.close();
            }
            MockRunner third;
            const Recorded rec3 = resume(third, nullptr);
            TestHarness::check(rec3.outCount == 1 && third.calls.isEmpty()
                                   && taskOf(rec3).value(QStringLiteral("status")).toString() == QStringLiteral("completed"),
                               "抢占: release 后同 id 再 resume 走缓存完成");
        }
    }
}

} // namespace

int tst_workflowruntime()
{
    const int before = TestHarness::failCount();

    // 无夹具组：任何环境全量执行
    testLimitsDirect();

    ScopedTempRoot tmp(QStringLiteral("workflowruntime"));
    if (!tmp.isValid())
    {
        TestHarness::skip("SKIP: LITE_TEST_TMPROOT unset/unwritable，跳过 workflow 执行环文件产物组\n");
        return TestHarness::failCount() - before;
    }

    testSingleRunEndToEnd(tmp.path());
    testResumeAllCached(tmp.path());
    testDirtyCachedReject(tmp.path());
    testSchemaRetryOnce(tmp.path());
    testSchemaRetryStillBad(tmp.path());
    testBudgetGates(tmp.path());
    testParallelBarrier(tmp.path());
    testPipelineOrdering(tmp.path());
    testNesting(tmp.path());
    testReviewChangesFullRing(tmp.path());
    testAdapterStrings(tmp.path());
    testResumeGatesAndPreemption(tmp.path());

    return TestHarness::failCount() - before;
}
