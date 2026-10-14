#pragma once

// 工作流执行环（lcc s16 42a555d 移植；规格参照 workflow_manager.py s16 时点 892 行）
//
// 定位：asyncio 编排执行环（ExecutionState 六原语 :391-494、review-changes 脚本 :510-575、
// 一次运行状态机 call/_callLocked :784-855、工具边界 run/run_workflow :859-892）的 lite 异步
// 转译。与 Lane A 的 WorkflowManager（注册表/身份/台账/文件产物/进程内抢占）手工对账：
// 本头只消费 Lane A 公开声明，不 include 其实现细节；整合期主线程编译核对消费点清单。
// 形态：纯回调 + lambda 续跑（仿 TeammateRuntime 宿主环形与 AgentLoopTeam settled 门闩形），
// 零线程、零嵌套事件循环；WorkflowRuntime 为一次性轻对象（可栈上），每次 run 的存活由
// 堆态 WfRun 的 shared_ptr 捕获链保证——defer 队列持有自身引用，宿主析构不悬空。
//
// 偏差登记（编号，行锚 lcc s16 42a555d；编号 9 起接续 Lane A 的 1-8，避免同文件族混淆）：
//   9.  同步阻塞 turn（lcc run_workflow :879-892 的 asyncio.run，§10-9）→ lite 异步延迟回填：
//       start() 立即返回，编排经 runner 的 done 续跑，终局经 out 回调交宿主 onToolFinished。
//   10. runner 同步/异步双形态：MockAgentRunner 的 to_thread 同步语义在 lite 测试以
//       setInlineDispatch(true)（defer=栈内直调）复刻；生产默认 QTimer::singleShot(0) 派发，
//       测试零事件循环驱动。
//   11. asyncio.Semaphore(8)（lcc :384）→ 计数门 + FIFO 队列（inFlight < kWorkflowConcurrency
//       立即派发，超限入队、释槽续派）。与「silently waiting 会交织两份 journal」的抢占纪律
//       （§10-4）无关——那是同一 runId 的 run 级独占，属 Lane A tryAcquireRunActive。
//   12. 深栈防护：释槽后的「下一批」派发经 defer（生产 singleShot(0) 下一拍，仿 FluentUI
//       Request 的防深栈先例）；测试内联模式下退化为栈内续跑（测试图规模深度可控）。
//   13. WorkflowInputError 的 f"{type}: {msg}" 终态前缀（lcc :837）：lite 无异常类型，错误自带
//       文案，脚本层错误统一折叠为 "workflow: {msg}"，终态 result={"error":"workflow: {msg}"}。
//   14. _displayPath（lcc :851，workDir 相对）→ outputFile 记绝对路径（Windows 反斜杠）。
//       仅进 event 日志行，不进 JSON 产物。
//   15. Python bool 的 repr True/False 出现在 event 日志 kv（resume=True）→ lite 记 "true"/
//       "false"。仅日志形，无契约影响。
//   16. args.budget 非数值（lcc int+str TypeError → 泛型失败形）→ lite 视为不限（防御登记）。
//   17. review-changes 的 out["findings"] 与 f["title"] 在 lcc 是裸下标（KeyError 即败）：
//       lite 防御——findings 缺失/类型不符走校验兜底错误串、title 非串取空串（key 失配代价 =
//       resume 重跑该步，行为安全）。
//   18. json.dumps(ensure_ascii=True)（lcc :491/:258）→ Qt toJson 直出 UTF-8（Lane A 偏差⑧同源）；
//       QJsonDocument::toJson(Compact) 末尾 \n 剥除后与 json.dumps 单行形一致。
//   19. lcc _writeJson 失败会抛断运行：lite writeJsonAtomic 返回 false 时静默继续（产物缺失
//       只影响审计/resume，不打断终局闭环——工具边界纪律 §10-3 优先）。
//
// 已知坑照采（§10-2/3/4/7）：journal 只在 agent() 层记录，parallel/pipeline 中间聚合值不落盘
// （结构重跑零成本）；失败也落终态、主循环不中断；同一 runId 抢占立刻失败不排队；label 参与
// journal key——改 label 或 prompt 前 24 字符即使该步缓存失效。
//
// 注册表手工同步债（§10-6）：脚本注册表（本头 scriptFor，review-changes 唯一条目）与 Lane A
// WorkflowManager 的 meta 注册表（lcc :622-624 单 dict 拆两半）以及宿主策略表必须逐名对账；
// 忘登记策略 = 永久 confirm（异步回合等于直接拒绝，fail-closed 的必然代价）。
//
// 禁翻区纪律：发往模型/runner、进 journal/快照/日志的英文逐字串一律 QStringLiteral，禁 tr()。
// 参数名与 schema 属性名逐字纪律（lcc :881-885）：name / args / resume_from_run_id 三个工具
// 参数名是跨层契约（toolInfo schema 属 Lane A、解析属本头 start()、接线属 Lane C），改名三处同炸。

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

class WorkflowManager;
struct WfExecState;
struct WfRun; // 堆态运行实例（定义在 .cpp：task/journal/budget/limits/计数门/门闩的每运行持有者）

// agent 侧信道调用器（宿主 runner 适配属 Lane C；lcc AnthropicAgentRunner :242-283 的 lite 形）。
// 契约（done 恰好一次）：
//   value  = parseRunnerJson 产物；解析失败回原文本值（不抛——两层分工：schema 校验触发
//          ExecutionState 层的唯一一次重试，lcc :274-279）；
//   tokens = 本次调用的 usage 合计（重试的 tokens 由执行环累加，lcc :447/:452）；
//   error  非空 = API 层失败（lcc 裸调用异常 → 终态 failed 的同形折叠，§10-3），此时 value 无意义。
using WfAgentRunner = std::function<void(
    const QString &prompt, const QString &schemaJson, const QString &label,
    std::function<void(const QJsonValue &value, qint64 tokens, const QString &error)> done)>;

// 执行环续跑形：value 与 error 恰居其一（error 非空 = lcc raise 的折叠）
using WfDone = std::function<void(const QJsonValue &value, const QString &error)>;
using WfThunk = std::function<void(const WfDone &done)>;
using WfStage = std::function<void(const QJsonValue &prev, const QJsonValue &item, int index, const WfDone &done)>;
using WfScriptFn = std::function<void(WfExecState &ctx, const QJsonObject &args, const WfDone &done)>;

// 六原语执行上下文（lcc ExecutionState :391-494）：一次运行一个根实例；嵌套 workflow() 派生
// 子实例（共享 WfRun 的 task/journal/runner/budget/limits 五本账，depth+1、phase/_phases_seen
// 各自新建——lcc :488-492 子 ExecutionState 同形）。
// 生命周期：必须以 make_shared 堆建（enable_shared_from_this 供脚本续跑闭包持自身引用，
// 全链路 shared_ptr 捕获链保活——WfRun 不反向持有 ctx，无引用环）。
struct WfExecState : std::enable_shared_from_this<WfExecState>
{
    // 任务续跑（lcc agent :420-466 全状态机的 C++ 形）。
    // schema：Null/Undefined=无约束；Object=校验 + runner 拼装。
    // label 空 = prompt 前 24 字符 + "..."（lcc :424——参与 journal key，改 prompt 头部即失效，§10-7）。
    // phase 空 = 当前 phase（lcc :425 的 phase or self._phase）。
    void agent(const QString &prompt, const QJsonValue &schema = QJsonValue(),
               const QString &label = QString(), const QString &phase = QString(),
               const WfDone &done = [](const QJsonValue &, const QString &) {});

    // gather 屏障（lcc :468-470）：全成才成（values 按 thunk 序）；任一败即整体败且只回一次
    // （settled 门闩仿 AgentLoopTeam 形；迟到者结果丢弃）。空列表立即回空数组。
    void parallel(const std::vector<WfThunk> &thunks, const WfDone &done);

    // items 间并发、各自顺序穿段、段间无屏障（lcc :472-481）；stage 收 (上一段值, 原 item, 下标)；
    // 任一 stage 败 → 整体败一次（先到先得），成功 values 按 item 序。
    void pipeline(const QJsonArray &items, const std::vector<WfStage> &stages, const WfDone &done);

    // 同名工作流嵌套内联（lcc :483-494）：depth>=1 → "workflow() nesting is one level only"；
    // 查无（Lane A 注册表）→ "unknown workflow '{name}'"；子 ctx 共享五本账 + depth+1。
    // 忠实 lcc：嵌套不再过 deny 闸（lcc 嵌套直取脚本字典项，不经 call()）。
    void workflow(const QString &name, const QJsonObject &args, const WfDone &done);

    // 阶段宣告（lcc :409-415）：同名只播一次 workflow_phase（upsert 语义经 _phases_seen）。
    void phase(const QString &title);
    // 进度日志（lcc :417-418）：workflow_log 事件。
    void log(const QString &message);

    QString currentPhase() const;
    int depth() const;

    WfExecState() = default;
    ~WfExecState();
    WfExecState(WfExecState &&) = delete;
    WfExecState &operator=(WfExecState &&) = delete;
    WfExecState(const WfExecState &) = delete;
    WfExecState &operator=(const WfExecState &) = delete;

private:
    friend class WorkflowRuntime; // 根 ctx 装配（run()/workflow() 内构造并直填私有）
    std::shared_ptr<void> m_run; // 实际为 shared_ptr<WfRun>（.cpp 内 static_pointer_cast）
    QJsonObject m_args;
    int m_depth = 0;
    QString m_phase;
    class Private;
    std::unique_ptr<Private> m_priv; // _phases_seen（QSet<QString>）等小状态，pimpl 防头文件膨胀
};

// 一次性编排轻对象：每 start()/run() 至多驱动一次运行；可栈上持有（终局后运行态随
// defer 链自然释放）。宿主接线（工具分发/权限门/回填）属 Lane C。
class WorkflowRuntime
{
public:
    WorkflowRuntime();
    ~WorkflowRuntime();
    WorkflowRuntime(const WorkflowRuntime &) = delete;
    WorkflowRuntime &operator=(const WorkflowRuntime &) = delete;

    // 测试形：defer 退化为栈内直调（Mock runner 同步 done 时全链路无需事件循环；
    // 生产保持默认 false——释槽续派与并行首批之外的派发经 QTimer::singleShot(0) 防深栈，偏差12）。
    void setInlineDispatch(bool on);

    // 状态机入口（lcc call/_callLocked :784-855，同步闸段）：policy deny→第二道闸；resume 四闸
    // （全在 journal 截断前）；reserve/抢占/open journal；信封与事件、running 快照落盘后启动脚本。
    // done 恰一次（终局门闩含取消路径）：
    //   error 非空 = 工具层错误（未进执行期，无产物写入）；
    //   error 空   = 已进执行期，envelope={launched,result,task}（result 内 {"error":...} 即失败终态）。
    using RunDone = std::function<void(const QJsonObject &envelope, const QString &error)>;
    void run(WorkflowManager &manager, const QString &name, const QJsonValue &args,
             const QString &resumeFromRunId, const WfAgentRunner &runner, const RunDone &done);

    // 工具边界入口（lcc run + run_workflow :859-892）：toolArgs 取 name / args / resume_from_run_id
    // （逐字参数名，见头注释契约）。全部错误折叠为 "Error: {msg}" 输出串、绝不打断宿主回合
    // （§9-9 纪律：lcc 双层 catch 的全量兜住形）；成功输出 = envelope 的紧凑 JSON 序列化。
    // out 恰好一次（inline 模式下可能在 start 返回前同步回圈）。
    using ToolOutputDone = std::function<void(const QString &toolOutput)>;
    void start(WorkflowManager &manager, const QJsonObject &toolArgs,
               const WfAgentRunner &runner, const ToolOutputDone &out);

    // 宿主停止钩子（§10-3 闭环的 lite 增项，lcc 无停止路径——偏差登记见 .cpp）：置旗后在途
    // 续跑点在下一拍折叠为 failed 终态（result.error="workflow: run cancelled"）；若此刻无在途
    // 直接即时终态。迟到的 runner 回调被终局门闩丢弃。
    void requestCancel();

    // ---- runner 适配辅助（交付 7：AnthropicAgentRunner :242-283 的宿主侧常量与拼装）----

    // schema 确定性序列化：Null→"null"；其余经单元素数组 Compact 剥外层括号（与 Lane A
    // agentKey 的 schemaCanonical 同口径；QJsonObject 键天然字典序 = sort_keys=True；
    // UTF-8 直出不转 \uXXXX，ensure_ascii 差异见 Lane A 偏差⑧ / 本头偏差18）。
    static QString canonicalSchemaJson(const QJsonValue &schema);
    // 发往 runner 的请求拼装（逐字 lcc :258-261）：schemaJson 非空且非 "null" 时
    // prompt + "\n\nReturn only one JSON object matching this schema:\n" + schemaJson。
    static QString buildRunnerPrompt(const QString &prompt, const QString &schemaJson);

    // 宿主 runner 侧常量（对齐 lcc AnthropicAgentRunner :242-283；消费方=宿主 AsyncRequest 适配）。
    static const int kAgentMaxTokens = 2000;
    // focused system 逐字串（lcc :247-250 禁翻区）。
    static const QString kFocusedSystemPrompt();

    // ---- 脚本注册表（对照 lcc workflows dict :622-624：meta 拆给 Lane A、fn 留本头）----

    // 查无返回空 std::function。新增工作流必须三处同步：本注册表、Lane A meta 表、宿主策略表。
    static WfScriptFn scriptFor(const QString &name);

    // 单测 seam（生产禁止调用）：替换 review-changes 的执行体——小型脚本直接驱动六原语与各闸；
    // 传 nullptr 还原内置脚本。只影响 scriptFor("review-changes") 的返回，闸段/状态机全走真实链路。
    static void overrideReviewChangesScriptForTests(const WfScriptFn &fn);

private:
    bool m_inlineDispatch = false;
    std::shared_ptr<WfRun> m_current; // requestCancel 触达的运行态（一次对象至多一次运行）
};
