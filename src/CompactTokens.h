#pragma once

// 会话 token 计量单源头（header-only 纯函数集，无状态、无实例，被 include 即可编译）：
// 五级压缩管线（CompactManager）、上下文占用锚点外推（AgentLoop）、队友预算裁剪
// （TeammateRuntime）共用同一口径——逐条 QJsonDocument::Compact 序列化 → UTF-16 解码 →
// AgentConst::estimateTokens 求和。
//
// 为何单独成头（而非留在 CompactManager.cpp）：CompactManager.cpp 有 5 处依赖
// QOpenAi::AsyncRequest（摘要 LLM 段），而测试目标硬纪律是「仅允许 QtCore-only 的引擎
// 进清单」（CMakeLists.txt:128），整 TU 无法进 ctest。仿 LineEnding.h 先例把纯计量抽成头，
// 测试直接 include 即测真实编译产物，零 CMakeLists 改动（结构篇 §9.5「抽纯头」路线）。
//
// **承重不变量**（tst_compacttokens 逐条钉死，改本头前必读）：
//   ① conversationTokens 是 messageTokens 的**逐条独立求和**——项与项之间零耦合
//      （无分隔符、无长度前缀、无跨条累计量）。
//   ② 由 ① 得恒等式：替换其中一条后 新总量 == 原总量 - 旧条项 + 新条项。
//      microCompact / fitToolResults 的增量维护据此成立，是数学恒等而非近似。
//   ③ 若把口径改成「整段一次序列化」（引入分隔符或跨条累计），①② 同时失效，
//      增量维护会让压缩触发点静默漂移 → 属行为回归，必须连同两个消费点一起改。
//
// 与 CompactManager::estimateChars（UTF-16 码元口径、历史数据参照）同族近似：代理对按
// 2 个非 CJK 码元参与 4:1 折算，与 Python len(json.dumps(...)) 在码点/码元上有 ±1 量级差
// （登记偏差，同源）。

#include "AgentConstants.h" // estimateTokens 字符→token 折算单源（kCharsPerTokenBudget 基准）

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace CompactTokens {

// 单条消息的 token 估算项：只依赖入参自身，不看邻居、不看它在会话中的位置。
inline qsizetype messageTokens(const QJsonObject &message)
{
    return AgentConst::estimateTokens(
        QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

// 整段会话的 token 估算：逐条 messageTokens 求和。
// （原 CompactManager::estimateTokens 函数体逐字搬入，调用方经该类静态方法转发，口径零变更。）
inline qsizetype conversationTokens(const QVector<QJsonObject> &conversation)
{
    qsizetype total = 0;
    for (const QJsonObject &message : conversation)
        total += messageTokens(message);
    return total;
}

} // namespace CompactTokens
