#pragma once

// s15 错误恢复与异步拒批的逐字串 / 文本判据单源（header-only 静态函数集，无状态、无实例，
// 被 include 即可编译，纪律同 LineEnding.h）：lcc s15（af26862）在主循环外挂恢复层 recovery.py，
// 对瞬态错误（限流 / 过载 / 超时）做退避重试与换模决策，并在多处以外挂常量作为跨模块契约串。
// 本头收拢两类内容：
//   ① 契约逐字串——发往模型或落进历史的文本（续写指令、异步审批拒绝），改一个字即破坏
//      匹配或语义，必须单源；
//   ② 文本判据——lcc 的判据是「异常类型名 + 消息」鸭子式双源（Python 异常层级），lite 无
//      异常类型名可查，错误来源全部是文本（HTTP 状态码、服务端错误体、QNetworkReply 的
//      errorString），故折叠为统一小写化 contains 判据，语义等价。
// 消费方：QOpenAi（传输层重试/换模，本轮接线）与 AgentLoop（恢复主循环接线，后续 lane）；
// tests 可直接 include（纯 QtCore，无 GUI 依赖，与 tst_lineending 同款可测性）。
// 禁翻区纪律：回交模型 / 落历史的串一律 QStringLiteral、禁包 tr()（AGENTS.md「禁翻区」
// C 类口径）——这类串是上游关键词判定与模型理解口径的承重文本，本地化即失配。

#include <QString>

namespace RecoveryRules {

// max_tokens 溢出续写指令（lcc s15 recovery.py CONTINUATION_PROMPT 逐字）。
// 主循环检测到 finish_reason=length 时以该文本追加历史尾部，让模型从上次响应继续；
// 两拍恢复：第一拍升 max_tokens 重发，第二拍带本文本请求续写。消费点在 AgentLoop（后续 lane）。
inline QString continuationPrompt()
{
    return QStringLiteral("Continue from the previous response. "
                          "Do not repeat completed work.");
}

// 异步回合（无交互审批渠道）fail-closed 固定拒绝文案（lcc s15 permission.py
// ASYNC_APPROVAL_DENIED 逐字）。唤醒/后台/cron 等异步回合命中「需确认」规则时不开审批卡，
// 直接把本句作为 tool_result 交还模型；消费点在权限门（后续 lane）。
inline QString asyncApprovalDenied()
{
    return QStringLiteral("Permission denied: interactive approval is unavailable "
                          "during an asynchronous turn");
}

// 限流族判据（lcc s15 recovery.py withRetry 鸭子判据：类型名含 "ratelimit"，
// 消息含 "429" / "rate limit" / "rate_limit"）。lite 折叠为对小写化文本的并集 contains：
// 传输层错误文本（HTTP 429、"HTTP 429 错误（重试 N 次后仍失败）: …" 等）天然含这些关键词。
// 消费点在 AgentLoop 恢复接线（后续 lane）。
inline bool isRateLimitText(const QString &text)
{
    const QString lowered = text.toLower();
    return lowered.contains(QStringLiteral("ratelimit"))
        || lowered.contains(QStringLiteral("429"))
        || lowered.contains(QStringLiteral("rate limit"))
        || lowered.contains(QStringLiteral("rate_limit"));
}

// 过载族判据（lcc s15 recovery.py withRetry：类型名含 "overloaded"、消息含 "529" /
// "overloaded"）。连续命中达 AgentConst::kMaxConsecutive529 即触发 fallback 换模——
// 消费点在 QOpenAi 传输层（本轮接线）与 AgentLoop 恢复接线（后续 lane）。
inline bool isOverloadedText(const QString &text)
{
    const QString lowered = text.toLower();
    return lowered.contains(QStringLiteral("overloaded"))
        || lowered.contains(QStringLiteral("529"));
}

} // namespace RecoveryRules
