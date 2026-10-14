// 工作流执行环实现（lcc s16 42a555d 移植；偏差编号 9 起接续 WorkflowManager.h 的 1-8，见本头注释）
//
// 形态纪律：零线程、零嵌套事件循环；全部编排经 runner 的 done 回调续跑（§10-9 的 lite 异步形）。
// 单线程主循环下各闭包只会同步重入、不会并发，故门闩用朴素 bool/int 即可（lcc 的 threading.Lock
// 对应物在 lite 无并发读者，省略——偏差登记于头注释 9/12 段的延伸）。

#include "WorkflowRuntime.h"
#include "WorkflowManager.h"

#include <QJsonDocument>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include "LiteLog.h"

namespace {

// ---------------------------------------------------------------------------
// 逐字契约串（禁翻区：进日志/快照/journal/runner 请求，禁 tr()）
// ---------------------------------------------------------------------------

QString cancelErrorText() // lite 增项（lcc 无停止路径）；终态折叠为 "workflow: run cancelled"
{
    return QStringLiteral("run cancelled");
}

// 单元素数组包装再剥外层括号 = Lane A agentKey 的 schemaCanonical 同口径；
// object/array 剥括号后即其自身 Compact 形。末尾 '\n' 剥除（偏差18）。
QString jsonCompactText(const QJsonValue &value)
{
    const QJsonArray wrapped{value}; // 统一形状，标量/null 也可承载
    QByteArray raw = QJsonDocument(wrapped).toJson(QJsonDocument::Compact);
    if (raw.startsWith('['))
        raw.remove(0, 1);
    if (raw.endsWith(']'))
        raw.chop(1);
    QString text = QString::fromUtf8(raw);
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text;
}

// {"high":0,"medium":1,"low":2}.get(severity,3)（lcc :571）
int severityRank(const QString &severity)
{
    if (severity == QLatin1String("high"))
        return 0;
    if (severity == QLatin1String("medium"))
        return 1;
    if (severity == QLatin1String("low"))
        return 2;
    return 3;
}

QJsonObject findingsSchema()
{
    // 逐字 lcc FINDINGS_SCHEMA :510-515（JSON 字面量，禁翻区）：
    // {"type":"object","required":["findings"],"properties":{"findings":
    //   {"type":"array","items":{"type":"object","required":["title","severity"],
    //    "properties":{"title":{"type":"string"},
    //                  "severity":{"type":"string","enum":["high","medium","low"]}}}}}}
    static const QJsonObject schema = QJsonDocument::fromJson(
        QByteArrayLiteral(R"({"type":"object","required":["findings"],"properties":{"findings":{"type":"array","items":{"type":"object","required":["title","severity"],"properties":{"title":{"type":"string"},"severity":{"type":"string","enum":["high","medium","low"]}}}}}})"))
        .object();
    return schema;
}

QJsonObject verdictSchema()
{
    // 逐字 lcc VERDICT_SCHEMA :516-521：
    // {"type":"object","required":["isReal","reason"],"properties":
    //   {"isReal":{"type":"boolean"},"reason":{"type":"string"}}}
    static const QJsonObject schema = QJsonDocument::fromJson(
        QByteArrayLiteral(R"({"type":"object","required":["isReal","reason"],"properties":{"isReal":{"type":"boolean"},"reason":{"type":"string"}}})"))
        .object();
    return schema;
}

const QStringList &reviewDimensions() // lcc :525 DIMENSIONS 逐字
{
    static const QStringList dims{QStringLiteral("correctness"), QStringLiteral("security"),
                                  QStringLiteral("performance"), QStringLiteral("style")};
    return dims;
}

void reviewChangesScript(WfExecState &ctx, const QJsonObject &args, const WfDone &done);

} // namespace

// ---------------------------------------------------------------------------
// WfExecState 私有件（头内前向声明的 pimpl）
// ---------------------------------------------------------------------------

class WfExecState::Private
{
public:
    QSet<QString> phasesSeen; // lcc _phases_seen :407（同名 phase 只播一次）
};

WfExecState::~WfExecState() = default;

// ---------------------------------------------------------------------------
// 每运行堆态（续跑闭包经 shared_ptr 引用链保活；WorkflowRuntime::m_current 供取消触达。
// WfRun 不持有 ctx 引用、ctx 持有 WfRun——无环；宿主析构前应先 requestCancel（Lane C 接线）。
// ---------------------------------------------------------------------------

struct WfJob
{
    QString prompt;      // 基础 prompt（重试时追加逐字续跑后缀；拼装由宿主 runner 经 buildRunnerPrompt）
    QString schemaJson;  // canonical 序列化（"null"=无 schema）
    QString label;
    QString phase;       // 已折叠 phase or self._phase（lcc :425）
    QString key;         // journal 语义 key（Lane A agentKey）
    QJsonValue schema;
    bool hasSchema = false;
    qint64 tokens = 0;   // 首轮 + 重试累计（lcc :447/:452）
    bool retried = false;
    WfDone done;         // 空 = 已收口（防双收）
};

struct WfRun : std::enable_shared_from_this<WfRun>
{
    // ---- 运行持有（lcc _callLocked :807-833 的局部五件套 + 记账）----
    WorkflowManager *manager = nullptr;
    WfAgentRunner runner;
    WfTask task;
    WfJournal journal;
    std::unique_ptr<WfBudget> budget;
    WfExecutionLimits limits;
    QJsonObject args;
    QString runId;
    QString taskId;
    QString snapshotPath;
    QString outputPath;
    bool resume = false;
    QJsonObject launched;   // 执行前定稿信封（lcc :815-817）
    QJsonValue resultValue; // 成功终态 result（脚本 done 的 value）
    std::function<void()> scriptStarter;
    std::function<void(const QJsonObject &, const QString &)> finalDone; // 恰一次（settled 门闩）

    // ---- 调度与门闩 ----
    bool inlineDispatch = false; // 测试形：defer 退化为栈内直调（偏差10）
    bool hasRun = false;         // 已过同步闸段（取消在此之前无义）
    bool running = false;        // 执行期（脚本或其续跑尚未收口）
    bool settled = false;        // 终局门闩（含取消路径，恰一次）
    bool cancelRequested = false;
    int inFlight = 0;                            // CONCURRENCY 计数门（替代 Semaphore，偏差11）
    std::deque<std::shared_ptr<WfJob>> queue;    // 超限 FIFO 队列
    bool dispatching = false;                    // 派发环重入防抖

    void defer(std::function<void()> fn)
    {
        if (inlineDispatch) {
            fn();
            return;
        }
        // 生产防深栈：释槽/续派一律挪到下一拍（仿 FluentUI Request 的 singleShot(0) 先例，偏差12）
        QTimer::singleShot(0, std::move(fn));
    }

    // 终局闭环（lcc :834-855：close→output→终态快照→saveLastRun→notification→return）。
    // 失败同样走完全部写入（§10-3 错误留结果不打断主循环）；writeJsonAtomic false 静默继续（偏差19）。
    void terminal(bool failed, const QString &errorText)
    {
        if (settled)
            return;
        settled = true;
        running = false;
        if (failed)
            task.status = QStringLiteral("failed");
        journal.close();
        QJsonValue result = resultValue;
        if (failed) {
            // {Type} 前缀折叠为 "workflow: {msg}"（偏差13）+ LOG_WARN（lcc :838-839）
            QJsonObject errObj;
            errObj.insert(QStringLiteral("error"), QStringLiteral("workflow: ") + errorText);
            result = QJsonValue(errObj);
            LOG_WARN << "[wf] workflow " << task.workflowName << " failed: " << errorText;
        }
        WorkflowManager::writeJsonAtomic(outputPath, result); // lcc :842：output 先于终态快照
        QJsonObject snapshot;
        snapshot.insert(QStringLiteral("runId"), runId);
        snapshot.insert(QStringLiteral("workflowName"), task.workflowName);
        snapshot.insert(QStringLiteral("args"), args);
        snapshot.insert(QStringLiteral("task"), manager->serializeTask(task));
        WorkflowManager::writeJsonAtomic(snapshotPath, QJsonValue(snapshot));
        manager->saveLastRun(runId);
        QStringList kv;
        kv << QStringLiteral("status=") + task.status
           << QStringLiteral("agents=") + QString::number(task.agents)
           << QStringLiteral("tokens=") + QString::number(task.tokens)
           << QStringLiteral("outputFile=") + outputPath; // 绝对路径（偏差14，仅日志形）
        task.event(QStringLiteral("task_notification"), kv);
        manager->releaseRunActive(runId); // 锁覆盖到全部写入之后（lcc with 语义 :791-855）
        if (finalDone) {
            QJsonObject envelope;
            envelope.insert(QStringLiteral("launched"), launched);
            envelope.insert(QStringLiteral("result"), result);
            envelope.insert(QStringLiteral("task"), manager->serializeTask(task));
            finalDone(envelope, QString());
        }
    }

    void runScript()
    {
        if (!scriptStarter)
            return;
        if (settled || cancelRequested) {
            scriptStarter = nullptr;
            if (cancelRequested)
                terminal(true, cancelErrorText());
            return;
        }
        const std::function<void()> starter = std::move(scriptStarter);
        scriptStarter = nullptr;
        starter();
    }

    // ---- CONCURRENCY 计数门 + FIFO 队列（lcc ExecutionLimits.semaphore :384/:442 的等价形）----

    void dispatchLoop()
    {
        dispatching = true;
        while (!cancelRequested && !settled && inFlight < kWorkflowConcurrency && !queue.empty()) {
            const std::shared_ptr<WfJob> job = queue.front();
            queue.pop_front();
            startJob(job); // inline 模式下同步穿完整个回调链；释槽的重入被 dispatching 旗挡住
        }
        dispatching = false;
        // inline-sync 派发期间释槽重入的尾部补派（正常情况下循环条件已覆盖，此为保险）
        if (!cancelRequested && !settled && inFlight < kWorkflowConcurrency && !queue.empty())
            requestDispatch();
    }

    void requestDispatch()
    {
        if (settled)
            return;
        if (cancelRequested) {
            drainCancelled();
            return;
        }
        if (dispatching)
            return; // 外层循环还在转，继续消费队列
        if (queue.empty() || inFlight >= kWorkflowConcurrency)
            return;
        defer([self = shared_from_this()] { self->dispatchLoop(); });
    }

    void startJob(const std::shared_ptr<WfJob> &job)
    {
        ++inFlight;
        dispatchRunner(job);
    }

    void dispatchRunner(const std::shared_ptr<WfJob> &job)
    {
        const std::shared_ptr<WfRun> self = shared_from_this();
        // 契约：宿主 runner 以 (prompt, schemaJson, label) 经 buildRunnerPrompt 拼装请求（lcc :258-261
        // 的拼装权在 runner 侧），done 恰一次（value/tokens/error 三态收口）。
        runner(job->prompt, job->schemaJson, job->label,
               [self, job](const QJsonValue &value, qint64 tokens, const QString &error) {
                   self->onRunnerResult(job, value, tokens, error);
               });
    }

    void releaseSlot()
    {
        --inFlight;
        requestDispatch();
    }

    // 唯一收口点：done 先摘除再释槽再回调（防双收；lcc 的 semaphore 在 return/raise 出 with 块时释放，
    // 此处释槽先于脚本续跑——仅影响队列续派与 done 的相对拍序，语义等价）
    void finishJob(const std::shared_ptr<WfJob> &job, const QJsonValue &value, const QString &error)
    {
        WfDone sink;
        std::swap(sink, job->done);
        if (!sink)
            return;
        releaseSlot();
        sink(value, error);
    }

    void failJob(const std::shared_ptr<WfJob> &job, const QString &error)
    {
        finishJob(job, QJsonValue(), error);
    }

    void onRunnerResult(const std::shared_ptr<WfJob> &job, const QJsonValue &value,
                        qint64 tokens, const QString &error)
    {
        if (settled)
            return; // 迟到回调被终局门闩丢弃（含取消路径）
        if (cancelRequested) {
            --inFlight;
            drainCancelled();
            return;
        }
        job->tokens += tokens;
        if (!error.isEmpty()) { // runner API 层失败（lcc 裸调用异常 → failed 终态同形，§10-3）
            failJob(job, error);
            return;
        }
        if (job->hasSchema) {
            QString verr;
            if (!WorkflowManager::validateJsonSchema(value, job->schema.toObject(), &verr)) {
                if (!job->retried) {
                    // 唯一一次重试（lcc :448-452）：续跑串逐字 :450；重试沿用当前槽位
                    // （lcc 的 async with 覆盖 retry 全程）；tokens 已累计，重试 tokens 再累加。
                    job->retried = true;
                    job->prompt += QStringLiteral("\n\nReturn valid JSON.");
                    dispatchRunner(job);
                    return;
                }
                failJob(job, QStringLiteral("agent(...) invalid output: ") + verr); // 逐字形态 :455
                return;
            }
        }
        QString berr;
        if (!budget->add(job->tokens, &berr)) { // 超支即拒且不计数（lcc :460 raise-before-+=）
            failJob(job, berr);
            return;
        }
        task.agents += 1;
        task.tokens += job->tokens;
        journal.record(job->key, value); // journal 只在 agent() 层记录（§10-2）
        QJsonObject meta;
        meta.insert(QStringLiteral("label"), job->label);
        meta.insert(QStringLiteral("phase"), job->phase);
        meta.insert(QStringLiteral("status"), QStringLiteral("done"));
        task.progressEvent(QStringLiteral("workflow_agent"), meta);
        finishJob(job, value, QString());
    }

    void drainCancelled()
    {
        while (!queue.empty() && !settled) {
            const std::shared_ptr<WfJob> job = queue.front();
            queue.pop_front();
            failJob(job, cancelErrorText()); // 首条错误沿屏障/续跑链收口脚本，其余丢弃
        }
        if (!settled && running && inFlight <= 0)
            terminal(true, cancelErrorText());
    }

    void requestCancel()
    {
        if (settled || cancelRequested || !hasRun)
            return;
        cancelRequested = true;
        if (inFlight > 0)
            return; // 最后一个 runner 回调经 cancel 分支闭环
        drainCancelled();
    }
};

// ---------------------------------------------------------------------------
// WfExecState 六原语（lcc ExecutionState :391-494）
// ---------------------------------------------------------------------------

QString WfExecState::currentPhase() const
{
    return m_phase;
}

int WfExecState::depth() const
{
    return m_depth;
}

void WfExecState::phase(const QString &title) // lcc :409-415：同名只播一次（经 _phases_seen）
{
    m_phase = title;
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (!run || run->settled)
        return;
    if (m_priv->phasesSeen.contains(title))
        return;
    m_priv->phasesSeen.insert(title);
    QJsonObject data;
    data.insert(QStringLiteral("title"), title);
    run->task.progressEvent(QStringLiteral("workflow_phase"), data);
}

void WfExecState::log(const QString &message) // lcc :417-418
{
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (!run || run->settled)
        return;
    QJsonObject data;
    data.insert(QStringLiteral("message"), message);
    run->task.progressEvent(QStringLiteral("workflow_log"), data);
}

void WfExecState::agent(const QString &prompt, const QJsonValue &schema,
                        const QString &label, const QString &phase, const WfDone &done)
{
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (!run || run->settled) { // 迟到续跑丢弃形（调用方屏障已收口时无人消费此串）
        done(QJsonValue(), QStringLiteral("run already settled"));
        return;
    }
    if (run->cancelRequested) {
        done(QJsonValue(), cancelErrorText());
        return;
    }

    // lcc :424：label = label or prompt[:24] + "..."（QString::left 按 UTF-16 码元切，
    // 代理对恰在 24 位切点时与 Python 字符数差 1——label 只进 key/日志，行为安全，登记注释）
    const QString effLabel = label.isEmpty() ? prompt.left(24) + QStringLiteral("...") : label;

    QString claimErr;
    if (!run->limits.claimAgent(&claimErr)) { // 先自增再判限（lcc :426 → ExecutionLimits :386-388）
        done(QJsonValue(), claimErr);
        return;
    }
    if (run->budget->remaining() <= 0) { // lcc :427-428 逐字串
        done(QJsonValue(), QStringLiteral("token budget exceeded"));
        return;
    }

    const bool hasSchema = schema.isObject(); // 非 object 型 schema 视同无（防御；lane 契约 schema 为对象）
    const QString schemaJson = WorkflowRuntime::canonicalSchemaJson(schema);
    const QString key = WorkflowManager::agentKey(QStringLiteral("agent"), effLabel, prompt, schema);

    QJsonValue cached;
    if (run->journal.cached(key, &cached)) { // HIT（lcc :431-440）：零 runner、零 tokens
        if (hasSchema) {
            QString verr;
            if (!WorkflowManager::validateJsonSchema(cached, schema.toObject(), &verr)) {
                // 脏值不续用（逐字 :437）
                done(QJsonValue(), QStringLiteral("cached agent output failed schema validation: ") + verr);
                return;
            }
        }
        QJsonObject meta;
        meta.insert(QStringLiteral("label"), effLabel);
        meta.insert(QStringLiteral("phase"), phase.isEmpty() ? m_phase : phase); // lcc :425 or 形
        meta.insert(QStringLiteral("status"), QStringLiteral("cached"));
        run->task.progressEvent(QStringLiteral("workflow_agent"), meta);
        done(cached, QString());
        return;
    }

    // MISS（lcc :442-466）：并发门 → runner → 校验/唯一重试 → 预算入账 → record → progress → done
    auto job = std::make_shared<WfJob>();
    job->prompt = prompt;
    job->schemaJson = schemaJson;
    job->label = effLabel;
    job->phase = phase.isEmpty() ? m_phase : phase;
    job->key = key;
    job->schema = schema;
    job->hasSchema = hasSchema;
    job->done = done;
    if (run->inFlight < kWorkflowConcurrency)
        run->startJob(job);
    else
        run->queue.push_back(job); // 超限入队，释槽续派（Semaphore 的计数门等价形，偏差11）
}

void WfExecState::parallel(const std::vector<WfThunk> &thunks, const WfDone &done)
{
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (thunks.empty()) { // gather([]) 立即回空数组
        done(QJsonValue(QJsonArray()), QString());
        return;
    }
    if (!run || run->settled) {
        done(QJsonValue(), QStringLiteral("run already settled"));
        return;
    }
    if (run->cancelRequested) {
        done(QJsonValue(), cancelErrorText());
        return;
    }

    // gather 屏障（lcc :468-470）：settled 门闩仿 AgentLoopTeam 形——全成才成（values 按 thunk 序），
    // 任一败即整体败且只回一次（先到先得，迟到者丢弃——在途 agent 照常入账/落 journal，§10-2）。
    struct Barrier
    {
        int remaining = 0;
        std::vector<QJsonValue> values;
        bool settled = false;
    };
    const auto bar = std::make_shared<Barrier>();
    bar->remaining = static_cast<int>(thunks.size());
    bar->values.resize(thunks.size());
    const auto finalSink = std::make_shared<WfDone>(done);

    for (std::size_t i = 0; i < thunks.size(); ++i) {
        const WfThunk thunk = thunks[i];
        const int index = static_cast<int>(i);
        const WfDone node = [run, bar, finalSink, index](const QJsonValue &value, const QString &error) {
            if (bar->settled) // 迟到者丢弃（错误短路后其余结果不再有意义）
                return;
            if (!error.isEmpty()) {
                bar->settled = true;
                (*finalSink)(QJsonValue(), error); // 一败俱败，只回一次
                return;
            }
            bar->values[index] = value;
            if (--bar->remaining == 0) {
                bar->settled = true;
                QJsonArray arr;
                for (const QJsonValue &v : bar->values)
                    arr.append(v);
                (*finalSink)(QJsonValue(arr), QString());
            }
        };
        // 生产防深栈：仅首枚随调用栈派发，其余挪到下一拍（偏差12；inline 测试形下等价直调）
        if (i == 0)
            thunk(node);
        else
            run->defer([thunk, node]() { thunk(node); });
    }
}

void WfExecState::pipeline(const QJsonArray &items, const std::vector<WfStage> &stages, const WfDone &done)
{
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (items.isEmpty()) {
        done(QJsonValue(QJsonArray()), QString());
        return;
    }
    if (!run || run->settled) {
        done(QJsonValue(), QStringLiteral("run already settled"));
        return;
    }
    if (run->cancelRequested) {
        done(QJsonValue(), cancelErrorText());
        return;
    }

    // lcc :472-481：items 间并发（gather）、各自顺序穿段、段间无屏障；任一 stage 败整体败一次。
    struct Pipe
    {
        int remaining = 0;
        std::vector<QJsonValue> values;
        bool settled = false;
    };
    const auto pipe = std::make_shared<Pipe>();
    pipe->remaining = static_cast<int>(items.size());
    pipe->values.resize(items.size());
    const auto finalSink = std::make_shared<WfDone>(done);
    const auto itemsShared = std::make_shared<QJsonArray>(items);
    const auto stagesShared = std::make_shared<std::vector<WfStage>>(stages);

    const auto nodeSink = std::make_shared<std::function<void(int, const QJsonValue &, const QString &)>>(
        [pipe, finalSink](int index, const QJsonValue &value, const QString &error) {
            if (pipe->settled)
                return;
            if (!error.isEmpty()) {
                pipe->settled = true;
                (*finalSink)(QJsonValue(), error); // 整体败一次（先到先得）
                return;
            }
            pipe->values[index] = value;
            if (--pipe->remaining == 0) {
                pipe->settled = true;
                QJsonArray arr;
                for (const QJsonValue &v : pipe->values)
                    arr.append(v);
                (*finalSink)(QJsonValue(arr), QString());
            }
        });

    // 单件穿段器：runItem(index, stageIndex, value)；递归经共享 std::function 引用自身
    auto runItem = std::make_shared<std::function<void(int, int, const QJsonValue &)>>();
    *runItem = [run, itemsShared, stagesShared, nodeSink, runItem](int index, int stageIndex,
                                                                    const QJsonValue &value) {
        if (run->settled)
            return; // 屏障已收口，迟到链丢弃
        if (run->cancelRequested) {
            (*nodeSink)(index, QJsonValue(), cancelErrorText());
            return;
        }
        if (stageIndex >= static_cast<int>(stagesShared->size())) {
            (*nodeSink)(index, value, QString());
            return;
        }
        const WfStage stage = (*stagesShared)[stageIndex];
        const QJsonValue item = (*itemsShared)[index];
        stage(value, item, index,
              [run, runItem, nodeSink, index, stageIndex](const QJsonValue &next, const QString &error) {
                  if (!error.isEmpty()) {
                      (*nodeSink)(index, QJsonValue(), error);
                      return;
                  }
                  // 段间续跑挪到下一拍防深栈（偏差12；inline 测试形下直调）
                  run->defer([runItem, index, stageIndex, next]() {
                      (*runItem)(index, stageIndex + 1, next);
                  });
              });
    };

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const QJsonValue item = items.at(i);
        if (i == 0)
            (*runItem)(i, 0, item); // 首件随调用栈起步（lcc :481 gather 的等价派发序）
        else
            run->defer([runItem, i, item]() { (*runItem)(i, 0, item); });
    }
}

void WfExecState::workflow(const QString &name, const QJsonObject &args, const WfDone &done)
{
    const std::shared_ptr<WfRun> run = std::static_pointer_cast<WfRun>(m_run);
    if (!run || run->settled) {
        done(QJsonValue(), QStringLiteral("run already settled"));
        return;
    }
    if (run->cancelRequested) {
        done(QJsonValue(), cancelErrorText());
        return;
    }
    if (m_depth >= 1) { // 逐字 :485
        done(QJsonValue(), QStringLiteral("workflow() nesting is one level only"));
        return;
    }
    if (!run->manager->hasWorkflow(name)) { // 逐字 :487（Lane A 注册表为名字单源）
        done(QJsonValue(), QStringLiteral("unknown workflow '") + name + QStringLiteral("'"));
        return;
    }
    const WfScriptFn script = WorkflowRuntime::scriptFor(name);
    if (!script) { // meta 登记而脚本漏登记 = 注册表手工同步违规（§10-6），按查无 fail-closed
        done(QJsonValue(), QStringLiteral("unknown workflow '") + name + QStringLiteral("'"));
        return;
    }
    // 子 ctx 共享五本账（task/journal/runner/budget/limits 全在 WfRun 上）、depth+1、
    // phase/_phases_seen 各自新建（lcc :488-492）；忠实 lcc：嵌套不再过 deny 闸。
    auto child = std::make_shared<WfExecState>();
    child->m_run = m_run;
    child->m_args = args;
    child->m_depth = m_depth + 1;
    child->m_priv = std::make_unique<WfExecState::Private>();
    script(*child, args, [child, done](const QJsonValue &value, const QString &error) {
        done(value, error); // child 随闭包保活至脚本收口
    });
}

// ---------------------------------------------------------------------------
// review-changes 脚本（lcc :510-575 全逐字转译；四维 × audit+verify pipeline = 9 次 agent 一次结果）
// ---------------------------------------------------------------------------

namespace {

void reviewChangesScript(WfExecState &ctx, const QJsonObject &args, const WfDone &done)
{
    auto ctxPtr = ctx.shared_from_this(); // 续跑链持自身引用（异步 stage 在脚本函数返回后才跑）
    ctxPtr->phase(QStringLiteral("Review")); // lcc :533

    // lcc :534-537：changes = args.get("changes","")；非 str raise（null 亦非 str——同判）。
    const QJsonValue changesVal = args.value(QStringLiteral("changes"));
    if (!changesVal.isUndefined() && !changesVal.isString()) {
        done(QJsonValue(), QStringLiteral("args.changes must be a string"));
        return;
    }
    QString reviewInput = changesVal.toString().trimmed(); // .strip()
    if (reviewInput.isEmpty()) // `or` 回落（lcc :537）
        reviewInput = QStringLiteral("No change context was supplied.");

    QJsonArray items; // DIMENSIONS 以 QJsonValue 序列承载（stage 收原 item）
    for (const QString &dim : reviewDimensions())
        items.append(QJsonValue(dim));

    std::vector<WfStage> stages;

    // audit 段（lcc :539-548）：产 {dimension, findings}
    stages.push_back([reviewInput, ctxPtr](const QJsonValue &, const QJsonValue &item, int,
                                           const WfDone &next) {
        const QString dimension = item.toString();
        // prompt 逐字 :541-544
        const QString prompt = QStringLiteral("Review this change context for ") + dimension
                               + QStringLiteral(" issues. Report only issues supported by the supplied text.\n\n")
                               + reviewInput;
        ctxPtr->agent(prompt, QJsonValue(findingsSchema()),
                      QStringLiteral("audit:") + dimension,
                      QStringLiteral("Review"), // 显式 phase（lcc :546 同名参数）
                      [next, dimension](const QJsonValue &value, const QString &error) {
                          if (!error.isEmpty()) {
                              next(QJsonValue(), error);
                              return;
                          }
                          // out["findings"] 在 lcc 是裸下标（KeyError 即败）；lite 防御（偏差17）
                          const QJsonObject obj = value.toObject();
                          if (!obj.contains(QStringLiteral("findings"))
                              || !obj.value(QStringLiteral("findings")).isArray()) {
                              next(QJsonValue(), QStringLiteral("agent output missing findings array"));
                              return;
                          }
                          QJsonObject out;
                          out.insert(QStringLiteral("dimension"), dimension);
                          out.insert(QStringLiteral("findings"), obj.value(QStringLiteral("findings")));
                          next(out, QString());
                      });
    });

    // verify 段（lcc :550-568）：逐 finding 构 parallel 对质，isReal 过滤
    stages.push_back([reviewInput, ctxPtr](const QJsonValue &prev, const QJsonValue &item, int,
                                           const WfDone &next) {
        ctxPtr->phase(QStringLiteral("Verify")); // lcc :551
        const QString dimension = item.toString();
        const QJsonArray findings = prev.toObject().value(QStringLiteral("findings")).toArray();

        std::vector<WfThunk> thunks;
        thunks.reserve(static_cast<std::size_t>(findings.size()));
        for (int i = 0; i < static_cast<int>(findings.size()); ++i) {
            const QJsonValue f = findings.at(i);
            const QString findingJson = jsonCompactText(f); // json.dumps ensure_ascii 等价见偏差18
            const QString title = f.toObject().value(QStringLiteral("title")).toString();
            // 非 str title 在 lcc 只是 f-string 拼空串下标（KeyError 才败）；防御取空串（偏差17）
            thunks.push_back([ctxPtr, reviewInput, dimension, findingJson, title](const WfDone &vdone) {
                // prompt 逐字 :557-561
                const QString prompt = QStringLiteral("Adversarially verify this ") + dimension
                                       + QStringLiteral(" finding against the supplied change context.\n\nChange context:\n")
                                       + reviewInput + QStringLiteral("\n\nFinding:\n") + findingJson;
                ctxPtr->agent(prompt, QJsonValue(verdictSchema()),
                              QStringLiteral("verify:") + dimension + QStringLiteral(":") + title,
                              QStringLiteral("Verify"),
                              vdone); // 循环变量全部按值捕获（C++ 无 Python 晚绑定坑，纪律仍显式）
            });
        }

        ctxPtr->parallel(thunks, [next, findings, dimension](const QJsonValue &values, const QString &error) {
            if (!error.isEmpty()) {
                next(QJsonValue(), error);
                return;
            }
            const QJsonArray verdicts = values.toArray();
            QJsonArray confirmed;
            const int n = qMin(static_cast<int>(findings.size()), static_cast<int>(verdicts.size())); // zip 截断等价
            for (int i = 0; i < n; ++i) {
                const QJsonValue v = verdicts.at(i);
                // lcc `if v and v.get("isReal")`：verdict 经 schema 校验必为对象、isReal 必为布尔
                if (v.isObject() && v.toObject().value(QStringLiteral("isReal")).toBool())
                    confirmed.append(findings.at(i));
            }
            QJsonObject out;
            out.insert(QStringLiteral("dimension"), dimension);
            out.insert(QStringLiteral("confirmed"), confirmed);
            next(out, QString());
        });
    });

    ctxPtr->pipeline(items, stages, [ctxPtr, done](const QJsonValue &values, const QString &error) {
        if (!error.isEmpty()) {
            done(QJsonValue(), error);
            return;
        }
        // flatten + severity 稳定排序（lcc :570-573）：{"dimension": r["dimension"], **f}
        const QJsonArray results = values.toArray();
        struct Ranked { int rank; int order; QJsonObject entry; };
        std::vector<Ranked> listed;
        int order = 0;
        for (const QJsonValue &rv : results) {
            if (!rv.isObject()) // lcc `if r` 真值筛（对象恒真，null/缺失才滤——等价形）
                continue;
            const QJsonObject r = rv.toObject();
            const QString dimension = r.value(QStringLiteral("dimension")).toString();
            for (const QJsonValue &fv : r.value(QStringLiteral("confirmed")).toArray()) {
                QJsonObject entry = fv.toObject();
                entry.insert(QStringLiteral("dimension"), dimension);
                listed.push_back({severityRank(entry.value(QStringLiteral("severity")).toString()),
                                  order++, entry});
            }
        }
        std::stable_sort(listed.begin(), listed.end(),
                         [](const Ranked &a, const Ranked &b) { return a.rank < b.rank; });
        QJsonArray confirmed;
        for (const Ranked &r : listed)
            confirmed.append(r.entry);
        // log 逐字 :574；result 形 :575
        ctxPtr->log(QStringLiteral("confirmed ") + QString::number(confirmed.size())
                    + QStringLiteral(" real finding(s)"));
        QJsonObject result;
        result.insert(QStringLiteral("confirmed"), confirmed);
        done(QJsonValue(result), QString());
    });
}

} // namespace

// 单测 seam 存储（overrideReviewChangesScriptForTests 写入；生产恒空）。
// 进程级静态：测试组之间必须显式还原（传空 std::function），勿在宿主代码路径赋值。
static WfScriptFn g_reviewChangesOverride;

// ---------------------------------------------------------------------------
// WorkflowRuntime：脚本注册表与辅助
// ---------------------------------------------------------------------------

WorkflowRuntime::WorkflowRuntime() = default;

WorkflowRuntime::~WorkflowRuntime()
{
    // 运行态由续跑闭包链持有；宿主析构前应先 requestCancel()（Lane C 接线纪律）。
    // 未 requestCancel 而对象先亡：defer 队列仍会走完并终态回填——finalDone 指向的宿主
    // 闭包若已随宿主析构，则属 Lane C 生命周期职责（同 settleTeamOnExit 析构红线一族）。
}

void WorkflowRuntime::setInlineDispatch(bool on)
{
    m_inlineDispatch = on;
}

WfScriptFn WorkflowRuntime::scriptFor(const QString &name)
{
    // 对照 lcc workflows dict :622-624：{"review-changes": review_changes} 唯一条目。
    // 手工同步债（§10-6）：新增工作流三处同步——本表、Lane A meta 表、宿主策略表。
    if (name == QLatin1String("review-changes")) {
        if (g_reviewChangesOverride) // 单测 seam（生产恒空）：小型脚本驱动六原语与闸
            return g_reviewChangesOverride;
        return &reviewChangesScript;
    }
    return WfScriptFn();
}

void WorkflowRuntime::overrideReviewChangesScriptForTests(const WfScriptFn &fn)
{
    g_reviewChangesOverride = fn;
}

QString WorkflowRuntime::canonicalSchemaJson(const QJsonValue &schema)
{
    // 与 Lane A agentKey 的 schemaCanonical 同口径：Null→"null"；其余数组包装剥括号 Compact。
    if (schema.isNull() || schema.isUndefined())
        return QStringLiteral("null");
    return jsonCompactText(schema);
}

QString WorkflowRuntime::buildRunnerPrompt(const QString &prompt, const QString &schemaJson)
{
    // 逐字 lcc :258-261：有 schema 才追加约束块；"null"/空 = 无 schema（lcc `if schema:` 转译）。
    if (schemaJson.isEmpty() || schemaJson == QLatin1String("null"))
        return prompt;
    return prompt + QStringLiteral("\n\nReturn only one JSON object matching this schema:\n") + schemaJson;
}

const QString WorkflowRuntime::kFocusedSystemPrompt()
{
    // 逐字 lcc :247-250（宿主 runner 适配消费；禁翻区）。
    return QStringLiteral("You are a focused workflow agent. Complete only the supplied step. "
                          "Do not claim access to files or results not included in the prompt.");
}

// ---------------------------------------------------------------------------
// 状态机入口（lcc call + _callLocked :784-855）
// ---------------------------------------------------------------------------

void WorkflowRuntime::run(WorkflowManager &manager, const QString &name, const QJsonValue &args,
                          const QString &resumeFromRunId, const WfAgentRunner &runner,
                          const RunDone &done)
{
    const auto runState = std::make_shared<WfRun>();
    m_current = runState;
    runState->manager = &manager;
    runState->runner = runner;
    runState->inlineDispatch = m_inlineDispatch;
    runState->finalDone = done;

    const bool argsProvided = !args.isNull() && !args.isUndefined();
    const bool resuming = !resumeFromRunId.isEmpty();
    QString err;

    // 1. meta + deny 第二道闸（lcc :770-782；deny 逐字 :779——嵌套兜底形，宿主确认门在 Lane C）
    const WfMeta meta = manager.metaFor(name);
    if (!WorkflowManager::validateMeta(meta, &err)) {
        done(QJsonObject(), err);
        return;
    }
    if (manager.policyFor(name) == QLatin1String("deny")) {
        done(QJsonObject(), QStringLiteral("workflow '") + name + QStringLiteral("' denied by host policy"));
        return;
    }

    // 2. 运行身份：resume 校验 / 新建预留（先于 journal 截断，lcc :670-683/:784-795）
    QString runId;
    if (resuming) {
        if (!WorkflowManager::validateRunId(resumeFromRunId, &err)) {
            done(QJsonObject(), err);
            return;
        }
        runId = resumeFromRunId;
    } else {
        runId = WorkflowManager::createRunId(meta.name);
        if (!manager.reserveRunId(&runId, &err)) {
            done(QJsonObject(), err);
            return;
        }
    }

    // 3. 进程内抢占：不排队故意的（§10-4：静默等待会让两份写入交织同一个 journal）
    if (!manager.tryAcquireRunActive(runId)) {
        done(QJsonObject(), QStringLiteral("workflow run ") + runId + QStringLiteral(" is already active"));
        return;
    }

    const QString storeDir = manager.storeDir();
    runState->snapshotPath = storeDir + QLatin1Char('/') + runId + QStringLiteral(".json");
    runState->outputPath = storeDir + QLatin1Char('/') + runId + QStringLiteral(".output.json");

    // 4. resume 四闸（全部在 journal 截断前——快照读 + 名字/args 对账，lcc :789-800）
    QJsonObject mergedArgs;
    if (resuming) {
        QString serr;
        // A 层定稿形 = (storeDir, runId, err)：path 由 A 内拼 storeDir+'/'+runId+'.json'；
        // 传完整路径会双重拼接致 resume 永远 snapshot-not-found（整合期实测修正）。
        const QJsonObject snap = WorkflowManager::readSnapshot(storeDir, runId, &serr);
        if (!serr.isEmpty()) {
            manager.releaseRunActive(runId);
            done(QJsonObject(), serr);
            return;
        }
        if (snap.value(QStringLiteral("workflowName")).toString() != meta.name) {
            manager.releaseRunActive(runId);
            done(QJsonObject(), QStringLiteral("resume runId does not match workflow meta"));
            return;
        }
        const QJsonObject savedArgs = snap.value(QStringLiteral("args")).toObject();
        if (!argsProvided) {
            mergedArgs = savedArgs; // 省略 = 沿用快照 args（lcc :796-797）
        } else if (args.toObject() != savedArgs) {
            manager.releaseRunActive(runId);
            done(QJsonObject(), QStringLiteral("resume args do not match the original run"));
            return;
        } else {
            mergedArgs = args.toObject();
        }
    } else {
        mergedArgs = argsProvided ? args.toObject() : QJsonObject{}; // `args or {}`（lcc :802）
    }

    // 5. journal open（resume=续写 / 新建=截断；open 失败即工具层错误，无产物写入，lcc :800-806）
    QString jerr;
    if (!runState->journal.open(storeDir, runId, resuming, &jerr)) {
        manager.releaseRunActive(runId);
        done(QJsonObject(), jerr);
        return;
    }

    // 6. 台账与执行前定稿信封（lcc :807-817）
    runState->runId = runId;
    runState->taskId = WorkflowManager::createTaskId(runId);
    runState->resume = resuming;
    runState->args = mergedArgs;
    runState->task.taskId = runState->taskId;
    runState->task.runId = runId;
    runState->task.workflowName = meta.name;
    runState->task.status = QStringLiteral("running");
    runState->launched.insert(QStringLiteral("status"), QStringLiteral("async_launched"));
    runState->launched.insert(QStringLiteral("taskId"), runState->taskId);
    runState->launched.insert(QStringLiteral("taskType"), QStringLiteral("local_workflow"));
    runState->launched.insert(QStringLiteral("runId"), runId);
    runState->launched.insert(QStringLiteral("workflowName"), meta.name);

    QStringList kv;
    kv << QStringLiteral("runId=") + runId << QStringLiteral("taskId=") + runState->taskId;
    runState->task.event(QStringLiteral("async_launched"), kv); // lcc :819
    kv.clear();
    kv << QStringLiteral("workflow=") + meta.name
       << QStringLiteral("phases=") + (meta.phases.isEmpty() ? QStringLiteral("-") : meta.phases.join(QLatin1Char(',')))
       << QStringLiteral("resume=") + (resuming ? QStringLiteral("true") : QStringLiteral("false")); // True/False→true/false（偏差15）
    runState->task.event(QStringLiteral("task_started"), kv); // lcc :820-821

    // 7. running 态快照（lcc :822-828；写失败静默继续，偏差19）
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("runId"), runId);
    snapshot.insert(QStringLiteral("workflowName"), meta.name);
    snapshot.insert(QStringLiteral("args"), mergedArgs);
    snapshot.insert(QStringLiteral("task"), manager.serializeTask(runState->task));
    WorkflowManager::writeJsonAtomic(runState->snapshotPath, QJsonValue(snapshot));

    // 8. 脚本查表（meta 在 Lane A 注册而本表漏登记 = 注册表手工同步违规，按工具层错误折叠，
    //    文案对齐 run() 的 unknown 族；lcc 此处是 KeyError → 泛型 except——偏差13 同族）
    const WfScriptFn script = WorkflowRuntime::scriptFor(meta.name);
    if (!script) {
        runState->journal.close();
        manager.releaseRunActive(runId);
        done(QJsonObject(), QStringLiteral("unknown workflow '") + meta.name + QStringLiteral("'"));
        return;
    }

    // 9. 预算 + 根 ctx + 启动（lcc :829-833；budget 非数值视为不限=偏差16）
    qint64 budgetTotal = -1;
    const QJsonValue budgetVal = mergedArgs.value(QStringLiteral("budget"));
    if (budgetVal.isDouble())
        budgetTotal = static_cast<qint64>(budgetVal.toDouble());
    runState->budget = std::make_unique<WfBudget>(budgetTotal);

    auto ctx = std::make_shared<WfExecState>(); // 根 ctx：friend 直填私有（lcc ExecutionState 构造 :831）
    ctx->m_run = runState;
    ctx->m_args = mergedArgs;
    ctx->m_depth = 0;
    ctx->m_priv = std::make_unique<WfExecState::Private>();

    runState->hasRun = true;
    runState->running = true;
    runState->scriptStarter = [runState, ctx, script, mergedArgs]() {
        script(*ctx, mergedArgs, [runState](const QJsonValue &value, const QString &error) {
            if (runState->settled)
                return; // 取消/迟到链已由门闩收口
            if (!error.isEmpty()) {
                runState->terminal(true, error); // 失败也落终态（§10-3）
                return;
            }
            runState->resultValue = value;
            runState->task.status = QStringLiteral("completed");
            runState->terminal(false, QString());
        });
    };
    runState->runScript();
}

// ---------------------------------------------------------------------------
// 工具边界入口（lcc run + run_workflow :859-892）
// ---------------------------------------------------------------------------

void WorkflowRuntime::start(WorkflowManager &manager, const QJsonObject &toolArgs,
                            const WfAgentRunner &runner, const ToolOutputDone &out)
{
    // 逐字参数名契约（lcc :881-885）：name / args / resume_from_run_id。
    // 全部错误折叠 "Error: {msg}"、绝不打断宿主回合（§9-9 双层 catch 的全量兜住形；
    // lcc {Type} 前缀在 lite 无异常类型——串以 "Error:" 开头天然被 isToolFailure 覆盖，对齐 s15 文案族）。
    QString inputErr;
    const QJsonValue nameVal = toolArgs.value(QStringLiteral("name"));
    if (!nameVal.isString()) {
        out(QStringLiteral("Error: ") + QStringLiteral("workflow name must be a string"));
        return;
    }
    const QString name = nameVal.toString();
    if (!manager.hasWorkflow(name)) {
        out(QStringLiteral("Error: unknown workflow '") + name
            + QStringLiteral("'. Available: ") + manager.workflowNames().join(QStringLiteral(", ")));
        return;
    }
    const QJsonValue argsVal = toolArgs.value(QStringLiteral("args"));
    QJsonValue args;
    if (!argsVal.isUndefined() && !argsVal.isNull()) {
        if (!argsVal.isObject()) {
            out(QStringLiteral("Error: ") + QStringLiteral("workflow args must be an object"));
            return;
        }
        args = argsVal;
    }
    const QJsonValue resumeVal = toolArgs.value(QStringLiteral("resume_from_run_id"));
    QString resumeId;
    if (!resumeVal.isUndefined() && !resumeVal.isNull()) {
        if (!resumeVal.isString()) {
            // lcc：非 str 由 validate_run_id 的 isinstance 前置拒绝（:83）——直出同串
            out(QStringLiteral("Error: ") + QStringLiteral("invalid workflow runId"));
            return;
        }
        resumeId = resumeVal.toString();
    }

    run(manager, name, args, resumeId, runner,
        [out](const QJsonObject &envelope, const QString &error) {
            if (!error.isEmpty()) {
                out(QStringLiteral("Error: ") + error); // 工具层错误（未进执行期）
                return;
            }
            // 成功/失败终态都是合法 JSON 输出（§10-3：模型侧只见一条 tool_result）
            out(QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
            // 形态注：lcc json.dumps 默认分隔符含空格、Qt Compact 无空格——JSON 等价（登记）
        });
}

void WorkflowRuntime::requestCancel()
{
    if (m_current)
        m_current->requestCancel();
}
