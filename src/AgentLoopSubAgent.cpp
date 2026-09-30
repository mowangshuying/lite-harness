// task 子代理：启动 SubAgent 异步链（独立上下文黑盒）与统一收口（cancel → kill → 合成 tool_result）。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "SubAgent.h"
#include "ToolNames.h"

#include <QJsonArray>

void AgentLoop::startSubAgentTask(const QJsonObject &toolCall, const QJsonObject &args)
{
    // 串行队列下同一时刻至多一个子代理；防御回填：异常残留时不能直接 return——
    // 该 task 调用将永不收口，队列停摆且 m_running 永真（会话卡死）。
    // 回填错误结果续跑队列，与 Unknown tool / 沙箱拒绝同一套约定。
    if (m_activeSub)
    {
        onToolFinished(toolCall, ToolNames::TASK,
                       AgentLoopDetail::toolSummary(ToolNames::TASK, args),
                       QStringLiteral("Error: another subagent is already active"));
        return;
    }

    SubAgent *sub = new SubAgent(this, this, m_workDir, m_model,
                                 args.value(QStringLiteral("prompt")).toString());
    // 子代理权限询问透明转发：复用宿主同一个 3 参 permissionRequired 信号，UI 零改动
    connect(sub, &SubAgent::permissionRequired, this, &AgentLoop::permissionRequired);
    // 子代理内部活动转发：进度信号直连宿主同名信号（UI task 卡 live 进度行；
    // 取消路径由 SubAgent 卫兵/SignalBlocker 双保险静默，sub 先于宿主析构无悬空）
    connect(sub, &SubAgent::progressEmitted, this, &AgentLoop::subagentProgress);

    m_activeSub = sub;
    m_pendingTaskCall = toolCall;

    // 完成回调：子代理黑盒收口，仅把最终汇总文本作为 tool_result 交还父循环
    //（"task" 的 toolOutputReady 供 UI 展示；stop()/错误链已先行收口时 onToolFinished
    // 的 !m_running 兜底自然静默）
    sub->start([this, toolCall, args](const QString &result) {
        m_activeSub = nullptr;
        m_pendingTaskCall = QJsonObject();
        onToolFinished(toolCall, ToolNames::TASK,
                       AgentLoopDetail::toolSummary(ToolNames::TASK, args), result);
    });
}

void AgentLoop::cancelSubAgent()
{
    if (!m_activeSub)
        return;

    SubAgent *sub = m_activeSub.data();
    m_activeSub = nullptr;
    sub->cancel();      // 级联：kill 流与进程、丢弃其待裁决询问、抑制完成回调
    sub->deleteLater();

    // 为父级 task 调用合成 "(cancelled)" tool_result，直写历史保持 tool_use/tool_result
    // 配对（不经 onToolFinished：停发展示信号、不续跑队列——s03 stop() 待决权限的同款收口）
    if (!m_pendingTaskCall.isEmpty())
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            m_pendingTaskCall.value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("(cancelled)");
        m_messages.append(toolResult);
        m_pendingTaskCall = QJsonObject();
    }
    // 半途批其余成员一并收口（实证缺陷：曾裸清空两队列）：子代理卡队时，队列里
    // 「已完成未回填」与「未开始」两类调用同样悬空——裸清空令 stop() 后段半途批兜底
    // 空转、坏配对落盘，续谈必遭上游 400（每条 tool_call 必须有对应 tool 消息）。
    // 同款纪律直写历史：flush 已完成结果 → 剩余调用逐一合成 "(cancelled)" → 清队列。
    // 三路调用方（stop/错误链/析构）自此都拿到配对完整历史。
    for (const auto &value : m_toolResultsReady)
        m_messages.append(value.toObject());
    m_toolResultsReady = QJsonArray();
    for (const auto &value : m_pendingToolCalls)
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            value.toObject().value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("(cancelled)");
        m_messages.append(toolResult);
    }
    m_pendingToolCalls = QJsonArray();
}
