// AgentLoopGoal.cpp — s17: host wiring TU for the goal-loop engine (GoalManager, P1 引擎层).
//
// lcc s17 goal loop reference: the terminal gate inside the agent loop's no-tool finish path
// (_run_turn → stop 收尾的 goal 判定) + parse_input 分流 + observe_usage 记账 + stop-hook 续跑。
// lite 宿主转译形与登记偏差（与 port 文件裁决一致）：
//   ① lcc 在终局点同步阻塞跑评估器；lite 零线程/零嵌套事件循环红线下改「挂起终局 + 异步回填」
//      ——仿 AgentLoopWorkflow.cpp 的 task/workflow 形：NeedEval 时把终局消息暂存
//      m_goalPendingFinalMsg，评估器回填经 finishTurnWithoutTools / continueTurnAfterStopHook
//      再入原终局/续跑链（两函数为 s17 从 messageFinished lambda 的逐字搬移，行为不变）。
//   ② 单飞义务：同一时刻至多一次在途评估（m_goalEvalInFlight 闩锁 + 回调代际快照）。
//      迟到/并发终局在途时到达 gate → fail-safe Allow 落原终局 + LOG_WARN（port 裁决缺漏处
//      的宿主侧补登记）。
//   ③ 评估器往返 token 不进消耗账：AsyncRequest 无 usage 通道（既有传输形态），lcc 亦只对
//      主响应 observe_usage，口径一致、不另造估算。
//   ④ blockCap 构造期快照 settings（stopHookBlockCapValue()），中途改设置对新会话生效。
//   ⑤ max_turns 撞上限出口的钩子链差量（s17 比对审计登记）：lcc loop.py:218-220 该出口仍
//      trigger_hooks("Stop", skip_goal=True)——skip_goal 只跳 goal、summary 播报照跑，并
//      log_warn("goal","max_turns=N 用尽，交还控制权（goal 保持 active）")；lite 该出口
//      （AgentLoopRequest.cpp:262-272）整条 Stop 钩子链不跑、无 goal 交还日志。goal 层
//      效果一致：不进评估器、不动 blocks/iterations 账。
//   ⑥ 评估器模型三级回退的次级语义替换（s17 比对审计登记）：lcc 次级=静态
//      ANTHROPIC_DEFAULT_HAIKU_MODEL（组装根启动期固化）；lite 次级=m_recoveryCurrentModel
//      （s15 回合内 fallback 动态粘滞），且首级每次评估现读 settings.ini——生效时机与
//      可变性均不同。
// 权限门/工具分发零改动——goal 不是模型工具（ToolNames.h 零触碰），分流只在 run() 入口，
// 终局门只在 messageFinished 的无 tool_calls 分支；max_turns 与流错误两终局不经本门
// （goal 层效果与 lcc skip_goal 一致：不进评估器不动账；出口钩子链差量见 ⑤ 登记）。

#include "AgentLoop.h"

#include "AgentConstants.h"
#include "GoalRules.h"
#include "LiteLog.h"
#include "QOpenAi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

// 终局门（承 lcc loop 收尾 goal 判定，置于 triggerStopHooks 之前——goal 先于 summary 序）。
// 返回 true = 本终局已挂起（评估器在途），调用方立即 return；false = 落原终局路径。
// 无 goal 会话第一行短路返回——逐字节等价红线（AgentLoopRequest.cpp 消费点同注）。
bool AgentLoop::gateGoalTerminal(const QJsonObject &finalMsg)
{
    if (!m_goal.active())
        return false;

    // 单飞卫兵（防御性）：挂起窗口内主循环无在途请求，正常不可达；若到达即状态异常，
    // 按 port 缺漏处补裁——fail-safe Allow，放行原终局，不动账
    if (m_goalEvalInFlight)
    {
        // 不重调 stopGate——引擎对 Defer 会另发 logDecision，日志口径会翻倍
        LOG_WARN << QStringLiteral("[goal] terminal gate reached while evaluator in flight — fail-safe allow");
        return false;
    }

    const GoalManager::StopGateResult gate = m_goal.stopGate(m_messages);
    switch (gate.outcome)
    {
    case GoalManager::StopGateResult::Outcome::NeedEval:
        startGoalEvaluation(finalMsg, gate);
        return true;
    case GoalManager::StopGateResult::Outcome::Defer:
    case GoalManager::StopGateResult::Outcome::Allow:
        break; // 引擎内部已 logDecision（Defer=LOG_INFO），宿主不另计账
    }
    return false;
}

// 评估器往返（lcc 同步阻塞评估的异步转译，偏差①）。请求体仿 workflow runner 桥形：
// system/user 两条 + max_tokens；无 tools、无 stream_options（一次性文本 JSON 回答）。
// 模型三级回退：settings goalEvaluatorModelId → 回合内粘滞 fallback → 主模型。
// （次级用 m_recoveryCurrentModel 动态粘滞、非 lcc 静态 HAIKU 环境变量，首级每次评估
//   现读 ini——时机与可变性差异=偏差⑥登记。）
void AgentLoop::startGoalEvaluation(const QJsonObject &finalMsg, const GoalManager::StopGateResult &gate)
{
    const QString configured = AgentConst::goalEvaluatorModelIdValue();
    const QString model = !configured.isEmpty()
        ? configured
        : (m_recoveryCurrentModel.isEmpty() ? m_model : m_recoveryCurrentModel);

    QJsonObject input;
    input[QStringLiteral("model")] = model;
    QJsonArray messages;
    QJsonObject system;
    system[QStringLiteral("role")] = QStringLiteral("system");
    system[QStringLiteral("content")] = gate.evaluatorSystemPrompt;
    messages.append(system);
    QJsonObject user;
    user[QStringLiteral("role")] = QStringLiteral("user");
    user[QStringLiteral("content")] = gate.evaluatorUserPrompt;
    messages.append(user);
    input[QStringLiteral("messages")] = messages;
    input[QStringLiteral("max_tokens")] = AgentConst::kGoalEvaluatorMaxTokens;

    // 挂起态先置位再发起（once 回调可能经 queued 续延迟到，闩锁必须先于在途事实成立）
    m_goalPendingFinalMsg = finalMsg;
    m_goalEvalInFlight = true;

    const QPointer<AgentLoop> self(this);
    const quint64 generation = gate.generation; // 代际快照：setGoal/clear 之后回填一律作废

    QOpenAi::AsyncRequest *req = QOpenAi::AsyncRequest::sendText(
        input, this,
        [self, generation](const QString &content, const QString &error)
        {
            if (!self)
                return; // 宿主已析构——parent 锚理论上先杀请求，此卫兵为双保险
            if (!self->m_goalEvalInFlight)
            {
                // cancelActiveGoal() 已收口（stop/析构窗口）：迟到回填静默弃置，零 emit
                LOG_DEBUG << QStringLiteral("[goal] late evaluator backfill discarded (latch down)");
                return;
            }
            self->m_goalEvalInFlight = false;
            self->m_goalEvalRequest = nullptr;

            // 引擎代际守卫兜底：不匹配时静默返默认 Allow、零记账，宿主不再另判
            const GoalRules::StopDecision decision =
                error.isEmpty()
                    ? self->m_goal.onEvaluatorText(content, generation)
                    : self->m_goal.onEvaluatorError(error, generation);
            self->applyGoalDecision(decision, generation);
        });
    if (req)
        m_goalEvalRequest = QPointer<QOpenAi::AsyncRequest>(req);
    else
    {
        // sendText 失败形态（配置缺失等）折叠为 done(error) 交付、理论恒有对象；若真为空：
        // s17 Gate2 MINOR-D——不直接终局，改经 onEvaluatorError + applyGoalDecision 合流，
        // 使 lastReason/record 入账与 lcc GoalError 终局同构、回显同样走 goalNotice 并接
        // summary 钩子链（MINOR-B 形）。兑底串为 lite 宿主自拟英文短语（非 lcc 逐字，
        // "Error:" 前缀承 isToolFailure 判据惯例，差异已登记 port 文件）。
        // 闩锁复位义务保持；pendingFinalMsg 挂起时已置好、由合流路径消费
        m_goalEvalInFlight = false;
        m_goalEvalRequest = nullptr;
        LOG_WARN << QStringLiteral("[goal] evaluator request could not be created — error decision fallback");
        const GoalRules::StopDecision decision =
            m_goal.onEvaluatorError(QStringLiteral("Error: evaluator request failed to start"), gate.generation);
        applyGoalDecision(decision, gate.generation);
    }
}

// 评估决策分派（六门判据与账目全在引擎侧，本函数只做宿主终局/续跑再入）。
// generation 仅用于诊断日志——引擎 onEvaluator* 已按代际折叠陈旧决策。
void AgentLoop::applyGoalDecision(const GoalRules::StopDecision &decision, quint64 generation)
{
    switch (decision.action)
    {
    case GoalRules::GoalAction::Block:
    {
        // goal 保持 active：把 blockContinueText 以 role=user 注入同一个 messages[] 再跑一轮
        // （与 summary 钩子 force 续跑同链——s17 逐字搬移的 continueTurnAfterStopHook）
        const GoalManager::GoalState *state = m_goal.active();
        if (!state)
            break; // 引擎语义上不可达（Block 不 reset），落默认收口
        LOG_WARN << QStringLiteral("[goal] block #") << m_goal.consecutiveBlocks()
                 << QStringLiteral(" (gen ") << QString::number(generation)
                 << QStringLiteral(") — continuing turn: ") << decision.reason.left(120);
        continueTurnAfterStopHook(GoalRules::blockContinueText(state->condition, decision.reason));
        // s17 Gate2 MINOR-C：显式清挂起终局——本条已入账 m_messages、续跑链不读本暂存，
        // 把「回填必先经 startGoalEvaluation 覆盖」的隐式不变量写成显式置空
        m_goalPendingFinalMsg = QJsonObject();
        return; // 回合继续，下一次终局重新过门
    }
    case GoalRules::GoalAction::Achieved:
    case GoalRules::GoalAction::Failed:
    case GoalRules::GoalAction::Limit:
    case GoalRules::GoalAction::Error:
        // 停下必须可见：终局回显走 goalNotice（status() 引擎口径——achieved/failed 已由
        // 引擎折入 lastStatus 终态形；limit/error 保持 active，status 显条件与 Last reason）
        emit goalNotice(m_goal.status());
        break;
    case GoalRules::GoalAction::Allow:
        // 陈旧代际/防御兜底：静默落原终局（引擎零记账，回显也不发——新 goal 的账不能被旧回填打扰）
        LOG_DEBUG << QStringLiteral("[goal] stale/defensive allow at gen ") << QString::number(generation);
        break;
    case GoalRules::GoalAction::Defer:
        break; // 决策路径不产生 Defer（仅 stopGate），并入静默收口
    }

    // s17 Gate2 MINOR-B：终局四形（achieved/failed/limit/error）与防御合流形同样经 summary
    // 钩子链收口——对齐 lcc（goal 门先判、stop hook/summary 照跑），镜像 messageFinished
    // 消费点 force 分支形。未注册钩子时 triggerStopHooks 返空串，行为逐字不变
    const QString force = triggerStopHooks();
    if (!force.isEmpty())
    {
        continueTurnAfterStopHook(force);
        return;
    }

    finishTurnWithoutTools(m_goalPendingFinalMsg);
    m_goalPendingFinalMsg = QJsonObject();
}

// 第七路收口成员（stop()/析构共用）：清闩锁 → 弃挂起终局 → cancel 在途请求
// （cancel 后 done 永久静默）。全路径零 emit——析构窗口红线，仿 cancelActiveWorkflow。
// 被弃的挂起终局无配对问题：goal 门不产生 tool_use，assistant 消息已入账 m_messages，
// stop() 自身的收口序列照常终局本回合。
void AgentLoop::cancelActiveGoal()
{
    if (!m_goalEvalInFlight && m_goalEvalRequest.isNull())
        return;
    m_goalEvalInFlight = false;
    m_goalPendingFinalMsg = QJsonObject();
    if (!m_goalEvalRequest.isNull())
        m_goalEvalRequest->cancel();
    m_goalEvalRequest = nullptr;
}
