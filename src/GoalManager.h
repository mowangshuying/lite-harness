#pragma once

// GoalManager.h — s17 goal loop 引擎（对照 lcc s17 a20fdec goal_manager.py L243-458 GoalController 移植）
//
// 职责：per-会话 goal 台账（条件/轮次/计时/token 基线/consecutiveBlocks/events）+ 停止闸七决策表
// 记账。评估器 prompt 物料、六门解析与转录渲染全部在 GoalRules.h 纯头；本类零网络、零 QOpenAi
// 依赖、纯内存不落盘（无 storeDir，对照 lcc——restore 交给宿主是 port ⑥ 裁决）。
//
// 与 lcc 的偏差登记（本头单源；①-⑬ 引 .slim/deepwork/port-s17-goal-loop.md，只列引擎层相关项）：
//  1.（port ①b）lcc evaluateAfterTurn 内同步调用评估器；lite 零线程红线拆三闸 API——
//     stopGate()（同步产出 allow/defer/needEval + prompt 物料）、onEvaluatorText()（六门解析 +
//     记账 + 终决策）、onEvaluatorError()（决策③ error 形态）。七决策表记账 100% 在引擎内，
//     异步往返归宿主（P2），判据矩阵因此可进 ctest 直测（QtCore-only 测试纪律）。
//     宿主义务：stopGate 单飞——同代际并发两次 NeedEval 会双记账，宿主必须保证在途评估至多
//     一个（P2 接线兑现，Gate2 核验）。
//  2.（port ⑫）lcc 评估器异常 reason 形 f"{type(error).__name__}: {error}"；lite 无异常——
//     解析失败折叠为 GoalRules::goalErrorReason()（"GoalError: " + 六门串），传输终态失败由
//     宿主以同形文本喂 onEvaluatorError()。goal 保持 active、计数不动，与 lcc 决策③逐字段一致。
//  3. lcc 构造 block_cap<1 → raise GoalError("block_cap must be at least 1")；lite 构造不失败——
//     回退默认 kGoalStopHookBlockCap 并 LOG_WARN 该逐字串。
//  4.（port ④）background_running 判据经 std::function 注入；空函数 = 恒 False（lcc or-lambda 同形）。
//     宿主接线用 BackgroundTasksManager::has_running()（≠hasPending，两口径分工见该头注释）。
//  5.（port ⑩⑪）消息形态适配 OpenAI 形；payload 与 tool_use arguments 的 JSON 空白差异
//     （Qt Compact 无分隔空格 vs json.dumps 默认 ', '/': '，纯空白差、契约语义不变——本处为
//     单源表述，GoalRules.h 两处行内注释均指向本偏差）；标量探针细分门2/门3；计量单位 UTF-16 单元。
//  6. lcc duration/elapsed 用 time.time() 浮点秒；lite 整秒（QDateTime 毫秒差 /1000 钳非负）。
//     转录/展示语义等价，测试对表按整秒口径。
 //  7. lcc parseInput 四 kind；setGoal 两门抛 GoalError 在 lcc 组装根无 catch、CLI 直接崩；
 //     lite 增 Kind::Error——两门（空/超 4000）逐字错误串经 input.error 折叠回传，不抛不崩。
 //     空 arg 回落 status 与 lcc 一致（goal_manager.py:280-281 本就是 status 回落、非 raise）。
//  8. 异步窗口代际守卫：lcc 同步评测无「评估器在途 goal 被改」态；lite 增 m_generation——
//     setGoal（含 replace 路径）与 clear 成功路径递增，stopGate 返回 NeedEval 时快照进
//     StopGateResult.generation；onEvaluatorText/onEvaluatorError 的入参代际与当前值不符一律
//     静默放行（零记账、不动 last_reason、不发 record 事件），防迟到旧代际回填把 replace 后的
//     新 goal 误标 achieved、或在 clear 后给旧 goal 记错误账。同代际但无 state（achieved/failed
//     内部清账不升代际）保留 !active 第二层兜底回 allow。
//  9.（port ⑥）restore 静态工厂已备好但 v1 宿主不接线（纯内存重启作废 = 登记）；复活规则照 lcc：
//     倒序首条 goal_status 定 lastStatus，仅 active=true 复活，轮次/计时/token 基线重开。
// 10.（port ③）observeUsage 收 usage 对象（prompt_tokens/completion_tokens 各自独立容错 0 累加，
//     对照 lcc usage_total 的 input+output 逐项 or-0；不经 UsageTokens::fromUsage 有效性门。
//     lcc 的 or-0 会放行负数，lite 负数经 UsageTokens::numberOrZero 钳 0——方向安全，账只增不胀）。
//     tokens_at_start 的 current 同源读点由宿主接线（estimatedContextTokens），引擎收 qint64 形参。
//
// 零线程纪律：无 QTimer/QThread/QNetwork；一切状态推进由宿主在事件回调里调用本类方法完成。

#include "GoalRules.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

#include <functional>
#include <optional>

class GoalManager
{
public:
    // lcc GoalState（mutable dataclass）；setAtMs 为 epoch 毫秒
    struct GoalState
    {
        QString condition;
        int iterations = 0;
        qint64 setAtMs = 0;
        qint64 tokensAtStart = 0;
        QString lastReason;
    };

    // lcc parseInput 返回 (kind, payload)；lite 形：kind + 文本载荷 + Error 折叠串（偏差 7）
    struct GoalInput
    {
        enum class Kind
        {
            None,    // 非 /goal 输入，text 原样交还
            Status,  // text = status() 回执（零模型回显，port ②）
            Cleared, // text = clear() 回执
            Set,     // text = 已生效的条件
            Error    // error = setGoal 两门逐字串
        };
        Kind kind = Kind::None;
        QString text;
        QString error;
    };

    // 三闸 API 第一闸（偏差 1）：needEval 时宿主用两段 prompt 发起评估器调用，
    // 回包喂 onEvaluatorText、终态失败喂 onEvaluatorError
    struct StopGateResult
    {
        enum class Outcome
        {
            Allow,   // 无 active goal：decision 默认形即 allow
            Defer,   // bg 在跑：decision = defer(deferReason)，评估器 0 调用
            NeedEval // 需要评估器往返：两段 prompt 已装配
        };
        Outcome outcome = Outcome::Allow;
        GoalRules::StopDecision decision;
        QString evaluatorSystemPrompt;
        QString evaluatorUserPrompt;
        quint64 generation = 0; // 装配时代际快照（偏差 8）：回填须原样送回 onEvaluatorText/onEvaluatorError
    };

    explicit GoalManager(int blockCap = AgentConst::kGoalStopHookBlockCap,
                         std::function<bool()> backgroundRunning = nullptr,
                         const QJsonArray &events = QJsonArray());

    // lcc restore classmethod（L436-458，偏差 9）：events 账本拷贝后倒序扫首条 goal_status
    static GoalManager restore(int blockCap,
                               std::function<bool()> backgroundRunning,
                               const QJsonArray &events);

    // ---- 台账读侧 ----
    int blockCap() const { return m_blockCap; }
    qint64 totalTokens() const { return m_totalTokens; }
    int consecutiveBlocks() const { return m_consecutiveBlocks; }
    // 偏差 8 代际读点：回填入参的权威来源是 stopGate 快照；本读点供宿主日志与测试同代际回填
    quint64 generation() const { return m_generation; }
    const GoalState *active() const { return m_active ? &*m_active : nullptr; }
    const QJsonArray &events() const { return m_events; }
    const QJsonObject &lastStatus() const { return m_lastStatus; }

    // ---- 记账（lcc 同名方法逐条对应）----
    // lcc observe_usage L262-264：只在真拿到响应时由宿主调用；返回累计总量
    qint64 observeUsage(const QJsonObject &usage);
    // lcc parse_input L268-284：/goal 四类分流（空 arg 回落 status 与 lcc 同形），会改账本状态（clear/setGoal 记账）
    GoalInput parseInput(const QString &text);
    // lcc set_goal L286-307：两门失败经 error 出参折叠回传（偏差 7）；tokensAtStart<0 = 取当前 totalTokens
    bool setGoal(const QString &condition, qint64 tokensAtStart = -1, QString *error = nullptr);
    // lcc clear L309-317：reason 空 = "cleared"；无 active 回 "No goal set"
    QString clear(const QString &reason = QString());
    // lcc status L319-344：currentTokens<0 = 取当前 totalTokens
    QString status(qint64 currentTokens = -1) const;
    // lcc begin_query L346-349：consecutiveBlocks 按「一个查询」计而非会话总；宿主每回合入口调
    void beginQuery();

    // ---- 停止闸三闸 API（偏差 1）----
    StopGateResult stopGate(const QVector<QJsonObject> &messages) const;
    // 第二/三闸：generation 送回 stopGate 快照，代际不符一律静默 allow（偏差 8：迟到旧代际回填零记账）
    GoalRules::StopDecision onEvaluatorText(const QString &rawText, quint64 generation);
    GoalRules::StopDecision onEvaluatorError(const QString &errorText, quint64 generation);

private:
    // lcc _record L421-434：goal_status 事件追加 events 并覆盖 lastStatus
    void record(bool active, bool met, bool failed, const QString &reason);
    // lcc stop_hook L399-417 的日志纪律：allow 不记；block 记 "block n/cap: reason[:120]"；
    // 其余 "{action}: reason[:160]"
    void logDecision(const GoalRules::StopDecision &decision) const;

    int m_blockCap;
    std::function<bool()> m_backgroundRunning;
    QJsonArray m_events;
    QJsonObject m_lastStatus; // 空对象 = lcc lastStatus None
    std::optional<GoalState> m_active;
    qint64 m_totalTokens = 0;
    int m_consecutiveBlocks = 0;
    // 偏差 8：setGoal（含 replace）与 clear 成功路径递增；回填代际不符即旧评估，静默放行
    quint64 m_generation = 0;
};
