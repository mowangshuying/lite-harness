#include "GoalManager.h"

#include "LiteLog.h"
#include "UsageTokens.h"

#include <QDateTime>

// 对照 lcc s17 a20fdec goal_manager.py L243-458；逐条方法映射与偏差登记见 GoalManager.h 头注释。

GoalManager::GoalManager(int blockCap,
                         std::function<bool()> backgroundRunning,
                         const QJsonArray &events)
    : m_blockCap(blockCap)
    , m_backgroundRunning(std::move(backgroundRunning))
    , m_events(events)
{
    // 偏差 3：lcc raise GoalError(block_cap must be at least 1)；lite 构造不失败——
    // 回退默认并 LOG_WARN 同一逐字串（串源 GoalRules::blockCapError，测试逐字对表）
    if (m_blockCap < 1)
    {
        LOG_WARN << QStringLiteral("goal ") << GoalRules::blockCapError();
        m_blockCap = AgentConst::kGoalStopHookBlockCap;
    }
}

GoalManager GoalManager::restore(int blockCap,
                                 std::function<bool()> backgroundRunning,
                                 const QJsonArray &events)
{
    // lcc L436-458：账本拷贝（构造内已复制 events）；倒序扫**首条** goal_status 即停——
    // 该条 active=true 才复活 GoalState，且轮次/计时/token 基线全部重开（iterations=0、
    // set_at=now、tokens_at_start=0、last_reason 空）；met/failed/cleared 事件只定 lastStatus 不复活
    GoalManager manager(blockCap, std::move(backgroundRunning), events);
    for (int i = manager.m_events.size() - 1; i >= 0; --i)
    {
        const QJsonObject event = manager.m_events.at(i).toObject();
        if (event.value(QStringLiteral("type")).toString() != QLatin1String("goal_status"))
            continue;
        manager.m_lastStatus = event;
        if (event.value(QStringLiteral("active")).toBool())
        {
            GoalState state;
            state.condition = event.value(QStringLiteral("condition")).toString();
            state.iterations = 0;
            state.setAtMs = QDateTime::currentMSecsSinceEpoch();
            state.tokensAtStart = 0;
            manager.m_active = state;
        }
        break;
    }
    return manager;
}

qint64 GoalManager::observeUsage(const QJsonObject &usage)
{
    // 偏差 10 / lcc usage_total L100-106：两键各自独立容错 0 相加（缺失/非数/负数→该项 0），
    // 刻意不走 UsageTokens::fromUsage 的 prompt<=0 有效性门——计量账不是命中率账
    m_totalTokens += UsageTokens::numberOrZero(usage, QStringLiteral("prompt_tokens"))
                   + UsageTokens::numberOrZero(usage, QStringLiteral("completion_tokens"));
    return m_totalTokens;
}

GoalManager::GoalInput GoalManager::parseInput(const QString &text)
{
    // lcc L268-284。判定全在小写化后的 argument 上（isClearAlias 内部 lower）；
    // 空 arg 回落 status 与 lcc 一致（goal_manager.py:280-281 本就是 status 回落、非 raise）；
    // GoalError 仅在 setGoal 两门（空/超 4000），lcc 组装根不 catch 即崩 CLI，lite 折叠为 Kind::Error（偏差 7）
    GoalInput input;
    const QString stripped = text.trimmed();

    if (stripped == QLatin1String("/goal"))
    {
        input.kind = GoalInput::Kind::Status;
        input.text = status();
        return input;
    }
    if (stripped.startsWith(QLatin1String("/goal ")))
    {
        const QString argument = stripped.mid(6).trimmed();
        if (GoalRules::isClearAlias(argument))
        {
            input.kind = GoalInput::Kind::Cleared;
            input.text = clear();
            return input;
        }
        if (argument.isEmpty())
        {
            input.kind = GoalInput::Kind::Status;
            input.text = status();
            return input;
        }
        QString error;
        if (!setGoal(argument, -1, &error))
        {
            // 偏差 7：两门逐字串折叠回传（宿主把 error 文本回显进会话，不进模型回合）
            input.kind = GoalInput::Kind::Error;
            input.error = error;
            return input;
        }
        input.kind = GoalInput::Kind::Set;
        input.text = argument;
        return input;
    }

    input.kind = GoalInput::Kind::None;
    input.text = text;
    return input;
}

bool GoalManager::setGoal(const QString &condition, qint64 tokensAtStart, QString *error)
{
    // lcc L286-307
    const QString trimmed = condition.trimmed();
    if (trimmed.isEmpty())
    {
        if (error)
            *error = GoalRules::emptyConditionError();
        return false;
    }
    if (trimmed.size() > AgentConst::kGoalMaxConditionLength)
    {
        if (error)
            *error = GoalRules::tooLongConditionError();
        return false;
    }
    // 旧 goal 先记终局事件（active=False, met/failed=False, reason="replaced by a new goal"）
    if (m_active)
        record(false, false, false, GoalRules::replacedReason());

    // 偏差 8：新 goal 确立（含 replace）使在途旧评估器的回填失效——代际递增
    ++m_generation;

    GoalState state;
    state.condition = trimmed;
    state.iterations = 0;
    state.setAtMs = QDateTime::currentMSecsSinceEpoch();
    state.tokensAtStart = (tokensAtStart < 0) ? m_totalTokens : tokensAtStart;
    m_active = state;
    m_consecutiveBlocks = 0;
    record(true, false, false, GoalRules::goalSetReason());
    LOG_INFO << QStringLiteral("goal set: ") << trimmed.left(120);
    return true;
}

QString GoalManager::clear(const QString &reason)
{
    // lcc L309-317；默认 reason = "cleared"（GoalRules::clearedReason 单源）
    if (!m_active)
        return GoalRules::noGoalSetText();

    const QString condition = m_active->condition;
    record(false, false, false, reason.isEmpty() ? GoalRules::clearedReason() : reason);
    m_active.reset();
    m_consecutiveBlocks = 0;
    ++m_generation; // 偏差 8：clear 成功同样使在途旧回填失效
    LOG_INFO << QStringLiteral("goal cleared: ") << condition.left(120);
    return GoalRules::goalClearedText(condition);
}

QString GoalManager::status(qint64 currentTokens) const
{
    // lcc L319-344
    const qint64 tokens = (currentTokens < 0) ? m_totalTokens : currentTokens;

    if (!m_active)
    {
        if (m_lastStatus.isEmpty())
            return GoalRules::noGoalSetText();
        const QString condition = m_lastStatus.value(QStringLiteral("condition")).toString();
        const QString reason = m_lastStatus.value(QStringLiteral("reason")).toString();
        if (m_lastStatus.value(QStringLiteral("met")).toBool())
            return QStringLiteral("Goal achieved: %1\nReason: %2").arg(condition, reason);
        if (m_lastStatus.value(QStringLiteral("failed")).toBool())
            return QStringLiteral("Goal failed: %1\nReason: %2").arg(condition, reason);
        return GoalRules::noGoalSetText();
    }

    // 偏差 6：整秒口径（毫秒差 /1000 钳非负）；spent 钳非负防宿主喂了更小的同源读数
    const qint64 elapsed =
        qMax(qint64(0), (QDateTime::currentMSecsSinceEpoch() - m_active->setAtMs) / 1000);
    const qint64 spent = qMax(qint64(0), tokens - m_active->tokensAtStart);

    QStringList lines;
    lines << QStringLiteral("Goal active: %1").arg(m_active->condition)
          << QStringLiteral("Elapsed: %1s").arg(elapsed)
          << QStringLiteral("Evaluations: %1").arg(m_active->iterations)
          << QStringLiteral("Tokens: %1").arg(spent);
    if (!m_active->lastReason.isEmpty())
        lines << QStringLiteral("Last reason: %1").arg(m_active->lastReason);
    return lines.join(QLatin1Char('\n'));
}

void GoalManager::beginQuery()
{
    // lcc L346-349：只清 consecutiveBlocks——iterations/tokens/active 一律不动
    m_consecutiveBlocks = 0;
}

GoalManager::StopGateResult GoalManager::stopGate(const QVector<QJsonObject> &messages) const
{
    // lcc evaluateAfterTurn L353-363 的前置两段（决策①②）；评估器调用与否由宿主凭 outcome 定
    StopGateResult result;
    result.generation = m_generation; // 偏差 8：装配即快照，回填时须原样送回
    if (!m_active)
        return result; // Allow（默认 decision 即 allow，lcc 此处不发日志——allow 不记纪律）

    if (m_backgroundRunning && m_backgroundRunning())
    {
        result.outcome = StopGateResult::Outcome::Defer;
        result.decision.action = GoalRules::GoalAction::Defer;
        result.decision.reason = GoalRules::deferReason();
        // lcc stop_hook 对 defer 走 "{action}: reason[:160]" 的 log_info 分支；
        // 记账（iterations/blocks）一律不动 = §10 陷阱「defer 不消耗评估器调用」
        logDecision(result.decision);
        return result;
    }

    result.outcome = StopGateResult::Outcome::NeedEval;
    result.evaluatorSystemPrompt = GoalRules::evaluatorSystemPrompt();
    result.evaluatorUserPrompt = GoalRules::evaluatorUserPrompt(
        m_active->condition,
        GoalRules::transcriptText(messages, AgentConst::kGoalTranscriptCharLimit));
    return result;
}

GoalRules::StopDecision GoalManager::onEvaluatorText(const QString &rawText, quint64 generation)
{
    // 偏差 8 代际守卫：setGoal（含 replace）/clear 使在途评估迟到——旧代际回填一律静默放行，
    // 零记账、不动 last_reason、不发 record 事件（先于解析：坏旧代际文本也不经 error 路径落账）
    if (generation != m_generation)
        return GoalRules::StopDecision();

    // 六门解析失败 → error 决策（决策③ 的解析侧，偏差 2）；同代际透传
    GoalRules::GoalEvaluation evaluation;
    QString parseError;
    if (!GoalRules::parseEvaluatorJson(rawText, evaluation, parseError))
        return onEvaluatorError(GoalRules::goalErrorReason(parseError), generation);

    // 同代际但无 state（achieved/failed 内部清账不升代际）：第二层兜底，迟到回填不记账（偏差 8）
    if (!m_active)
        return GoalRules::StopDecision();

    // lcc L381-397：成功评测先计轮次再落 reason
    m_active->iterations += 1;
    m_active->lastReason = evaluation.reason;

    GoalRules::StopDecision decision;
    if (evaluation.ok)
    {
        record(false, true, false, evaluation.reason);
        m_active.reset();
        m_consecutiveBlocks = 0;
        decision.action = GoalRules::GoalAction::Achieved;
        decision.reason = evaluation.reason;
    }
    else if (evaluation.impossible)
    {
        record(false, false, true, evaluation.reason);
        m_active.reset();
        m_consecutiveBlocks = 0;
        decision.action = GoalRules::GoalAction::Failed;
        decision.reason = evaluation.reason;
    }
    else
    {
        m_consecutiveBlocks += 1;
        record(true, false, false, evaluation.reason);
        if (m_consecutiveBlocks > m_blockCap)
        {
            // lcc L394：limit 不清 goal、计数继续累积（§10 陷阱）
            decision.action = GoalRules::GoalAction::Limit;
            decision.reason = GoalRules::limitReason(m_blockCap);
        }
        else
        {
            decision.action = GoalRules::GoalAction::Block;
            decision.reason = evaluation.reason;
        }
    }
    logDecision(decision);
    return decision;
}

GoalRules::StopDecision GoalManager::onEvaluatorError(const QString &errorText, quint64 generation)
{
    // 决策③（lcc L370-378）：goal 保持 active、iterations/blocks 一律不动，主循环照常收尾
    if (generation != m_generation) // 偏差 8 代际守卫（同 onEvaluatorText，先于任何记账）
        return GoalRules::StopDecision();
    if (!m_active) // 同代际无 state 的第二层兜底
        return GoalRules::StopDecision();

    m_active->lastReason = errorText;
    record(true, false, false, errorText);

    GoalRules::StopDecision decision;
    decision.action = GoalRules::GoalAction::Error;
    decision.reason = errorText;
    logDecision(decision);
    return decision;
}

void GoalManager::record(bool active, bool met, bool failed, const QString &reason)
{
    // lcc _record L421-434：无 state 时 condition=""/iterations=0/duration=0
    QJsonObject event;
    QString condition;
    int iterations = 0;
    qint64 duration = 0;
    if (m_active)
    {
        condition = m_active->condition;
        iterations = m_active->iterations;
        duration = qMax(qint64(0),
                        (QDateTime::currentMSecsSinceEpoch() - m_active->setAtMs) / 1000);
    }
    event.insert(QStringLiteral("type"), QStringLiteral("goal_status"));
    event.insert(QStringLiteral("condition"), condition);
    event.insert(QStringLiteral("active"), active);
    event.insert(QStringLiteral("met"), met);
    event.insert(QStringLiteral("failed"), failed);
    event.insert(QStringLiteral("reason"), reason);
    event.insert(QStringLiteral("iterations"), iterations);
    event.insert(QStringLiteral("duration"), duration);

    m_events.append(event);
    m_lastStatus = event;
}

void GoalManager::logDecision(const GoalRules::StopDecision &decision) const
{
    // lcc stop_hook L399-417 日志纪律（§10：除 allow 外全部决策必须留痕）
    switch (decision.action)
    {
    case GoalRules::GoalAction::Allow:
        return;
    case GoalRules::GoalAction::Block:
        LOG_WARN << QStringLiteral("goal block %1/%2: ")
                        .arg(m_consecutiveBlocks)
                        .arg(m_blockCap)
                 << decision.reason.left(120);
        return;
    default:
        LOG_INFO << QStringLiteral("goal %1: ")
                        .arg(QString::fromLatin1(GoalRules::actionName(decision.action)))
                 << decision.reason.left(160);
        return;
    }
}
