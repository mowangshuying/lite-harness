#pragma once

#include <QtGlobal> // qsizetype

#include "UsageTokens.h"

// ============================================================================
// StreamRate —— token 输出速度（token/s）计算单源（纯头：无 IO、无 GUI、无 tr()）。
//
// 本仓一切数字都遵守「计算单源」纪律（结构篇 §8）：token 计量在 AgentConst / CompactTokens.h /
// UsageTokens.h，行尾在 LineEnding.h。输出速度同样是一条算术，一旦散进 UI 就会长出第二个口径——
// 故按 UsageTokens.h 先例抽纯头，tests/tst_streamrate.cpp 直测真实编译产物（结构篇 §9.5）。
//
// 窗口定义（三时刻，全部由宿主观测，本头不读时钟）：
//   t0 = 流建立（createStream 返回）
//   t1 = 首个增量到达（thinkingDelta 或 textDelta）——生成真正开始的时刻
//   t2 = usage 末帧到达（契约 C-2：恒先于 messageFinished）
//   firstTokenMs  = t1 - t0   首字延迟（含流内重试退避——退避发生在 t1 之前，计入本值）
//   generationMs  = t2 - t1   生成窗口
//   rate = tokens / (generationMs / 1000)
//
// 为什么用「首个增量 → usage」而不是「请求发出 → usage」：后者把首字延迟（TTFT）与 429/5xx 退避
// 一起摊进分母，算出来的不是「模型吐字有多快」，而是「这一轮用户等了多久」——两个语义对体感
// 的影响相反（慢首字 + 快输出会被读成模型慢，快首字 + 慢输出会被读成网络慢）。故生成窗口从 t1
// 起算，TTFT 单独记录、单独显示，不混进速率。
//
// 有效性判据与 UsageTokens 同纪律——「没拿到数据」不等于「0」：
//   · usage 快照无效（prompt<=0）→ 窗口作废（与 adoptUsageAnchor 落锚门同一条）；
//   · 本轮没有任何增量（纯 tool_calls delta：SSE 的 delta 里只有 tool_calls 字段，不触发
//     textDelta/thinkingDelta）→ t1 未知 → 窗口作废。宁可不显示，也不把 TTFT 冒充生成窗口。
//
// 聚合口径：平均速率 = Σtokens / ΣgenerationMs，**不是**各窗口速率的算术平均——后者会让
// 1 token 的窗口与 2000 token 的窗口等权，一条长回答被九条短回答稀释。这条差异在下面测试里钉死。
//
// 口径边界（UI tooltip 同步声明）：本计量只覆盖主循环的 ChatStream 请求。队友回合与压缩摘要 /
// 记忆链 / workflow runner 不计时——后者走 AsyncRequest（无 usage 通道），前者虽有 usage 回读但
// 增量不经宿主信号面（AgentLoopTeam 只接 usageReceived/messageFinished），t1 无从观测。
// 这与 UsageTokens.h 的「部分消耗」边界不完全同构：消耗账含队友，速度账只含主循环。
// ============================================================================
namespace StreamRate {

// 一次请求的观测窗口。三个时刻量都是宿主观测所得，本头只负责判定与折算。
struct Window {
    qsizetype tokens = 0;        // 服务端回读的 completion_tokens
    qsizetype generationMs = 0;  // 生成窗口：t1 → t2
    qsizetype firstTokenMs = 0;  // 首字延迟：t0 → t1（含重试退避）
    bool valid = false;          // tokens>0 && generationMs>0；无效窗口不进任何分母
};

// 单窗口速率（token/s）。无效窗口返回 0.0——不返回 NaN/inf，UI 才能安全显示「—」。
inline double tokensPerSecond(const Window &w) {
    if (!w.valid || w.tokens <= 0 || w.generationMs <= 0)
        return 0.0;
    return double(w.tokens) * 1000.0 / double(w.generationMs);
}

// 定窗：判据全在这里，调用方只报观测时刻（firstDeltaMs<0 表示本轮无增量）。
// snapshot 走 UsageTokens::fromUsage，故「无效读数不进账」这条承重墙与消耗账同源。
inline Window makeWindow(const UsageTokens::Snapshot &snapshot, qsizetype firstDeltaMs, qsizetype elapsedMs) {
    Window w;
    if (!snapshot.valid || firstDeltaMs < 0)
        return w; // 无有效用量 / 无增量起点：窗口作废
    w.tokens = snapshot.completion;
    w.firstTokenMs = firstDeltaMs;
    w.generationMs = elapsedMs - firstDeltaMs;
    w.valid = w.tokens > 0 && w.generationMs > 0;
    return w;
}

// 会话级速度账：只增不清零（与 UsageTokens::Totals 同纪律），无效窗口直接忽略、不污染分母。
struct Meter {
    Window last;                  // 最后一个**有效**窗口（UI 显示「本轮」）
    qsizetype tokens = 0;         // Σtokens（仅有效窗口）
    qsizetype generationMs = 0;   // ΣgenerationMs（仅有效窗口）
    qsizetype firstTokenMsSum = 0; // ΣfirstTokenMs（仅有效窗口）
    int calls = 0;                // 有效窗口数（分母口径，与 UsageTokens::Totals::calls 同语义）

    void add(const Window &w) {
        if (!w.valid)
            return;
        last = w;
        tokens += w.tokens;
        generationMs += w.generationMs;
        firstTokenMsSum += w.firstTokenMs;
        ++calls;
    }

    bool isEmpty() const { return calls <= 0; }

    double lastRate() const { return tokensPerSecond(last); }
    // Σ/Σ 口径（见头注释）：空账返回 0.0，不除零
    double averageRate() const {
        if (calls <= 0 || generationMs <= 0)
            return 0.0;
        return double(tokens) * 1000.0 / double(generationMs);
    }
    double averageFirstTokenMs() const {
        if (calls <= 0)
            return 0.0;
        return double(firstTokenMsSum) / double(calls);
    }
};

} // namespace StreamRate
