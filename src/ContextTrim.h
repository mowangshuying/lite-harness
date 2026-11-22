#pragma once

// 上下文成组裁剪内核单源（header-only 纯函数，仿 CompactTokens.h 先例）：
// 从最旧开始按「可安全成组删除」单元丢历史，直到估算达标或无可删单元。
// 队友（TeammateRuntime::compactConversationTo，保任务卡 [0]）与子代理
// （SubAgent，保 system[0]+任务 prompt[1]）共用本内核——配对不变量只写一份。
//
// 抽头动机：内核原先长在 TeammateRuntime.cpp（QObject + QOpenAi 依赖链进不了
// ctest），配对逻辑（assistant 带 tool_calls 必须连同其后连续 tool 结果整组删，
// 孤立删 tool 会破坏 OpenAI 形态配对）是安全关键却零直测；子代理复用若复制一份
// 即开出第二处漂移源。抽成纯头后两侧一行转发，判据由 tst_contexttrim 直测。
//
// 计量口径：删除量按 CompactTokens::conversationTokens 逐条独立求和（不变量①），
// currentTokens 由调用方传入其当前口径的估算值（队友=锚定/兜底含 overhead，
// 子代理=本地全量+tools schema）——锚定调用方的减法为有界近似（锚真值 vs 本地
// 估算混合口径），与抽头前逐值等价；本地估算调用方则是数学恒等。

#include "CompactTokens.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ContextTrim {

// 常规压缩目标：预算的 80%（与主循环 CompactManager 压缩目标 0.8T' 同口径簇）。
// 反应式压缩目标：预算的 50%（溢出后要留足余量给新回合输出）。
// 比例只作用于换算后整数，系数取整为 qsizetype 防丢尾（原 TeammateRuntime.cpp
// 匿名 ns 常量逐字搬入，两消费点共用防漂移）。
constexpr qsizetype kCompactTargetRatioNumerator = 4;
constexpr qsizetype kCompactTargetRatioDenominator = 5;
constexpr qsizetype kReactiveTargetRatioNumerator = 1;
constexpr qsizetype kReactiveTargetRatioDenominator = 2;

// 成组裁剪：messages 前 keepHead 条恒保留；currentTokens 为调用方口径的当前总量，
// 每删一组按 CompactTokens 口径减项。返回是否动过（调用方据此作废 token 锚）。
// 单元判定：assistant 带 tool_calls → 连同其后连续 tool 结果整组；其余单条独立；
// 扫描位上的孤立 tool（畸形历史兜底）跳过不删。只删不重组，保留消息相对顺序不变。
inline bool trimConversation(QVector<QJsonObject> &messages, qsizetype keepHead,
                             qsizetype currentTokens, qsizetype targetTokens)
{
    bool changed = false;
    qsizetype current = currentTokens;
    qsizetype i = keepHead;
    while (current > targetTokens && i < messages.size()) {
        const QJsonObject &head = messages.at(i);
        const QString role = head.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("tool")) {
            // 属于更早 assistant 的工具结果：不能独立删，继续向后找单元头
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        if (role == QLatin1String("assistant") && head.contains(QLatin1String("tool_calls"))) {
            while (j < messages.size()
                   && messages.at(j).value(QStringLiteral("role")).toString()
                          == QLatin1String("tool")) {
                ++j;
            }
        }
        // 逐条取 messageTokens 求和，替代 conversationTokens(messages.mid(i, j - i))：
        // mid() 每组都分配一个临时 QVector（k 组 = k 次分配 + k 次整段搬运），而
        // CompactTokens.h 不变量① 明写 conversationTokens 就是 messageTokens 的逐条独立
        // 求和（项间零耦合：无分隔符、无长度前缀、无跨条累积），故两者**逐值恒等**，
        // 不是近似。断言见 tests/tst_contexttrim.cpp + tst_compacttokens.cpp。
        qsizetype groupTokens = 0;
        for (qsizetype k = i; k < j; ++k)
            groupTokens += CompactTokens::messageTokens(messages.at(k));
        current -= groupTokens;
        messages.remove(i, j - i);
        changed = true;
    }
    return changed;
}

} // namespace ContextTrim