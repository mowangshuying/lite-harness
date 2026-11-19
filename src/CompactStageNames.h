#pragma once

/*
    CompactStageNames.h —— 本地四段压缩的 UI 伪工具名（只依赖 Qt6::Core）

    为什么单独一个纯头（而不是塞进 ToolNames.h）：
    ToolNames.h 是**发给模型的工具名**唯一事实源，tst_toolnames / tst_toolschema 钉住
    「常量清单 ↔ 静态 schema 27 项」恰好重合。本地四段压缩不是模型可调用的工具，
    入表会让那两条重合断言立刻红。故沿用 s09 "memory" 的 UI 伪键先例：伪名不入
    ToolNames.h，但**也不散写字面量**——发射侧（CompactManager）与呈现侧
    （ToolBlock 标题表）共用本头，改一处即两侧同步，杜绝「卡片发了但标题回退成
    『已执行』」这类静默失配。

    四个名字与 lcc s08 本地管线的四段一一对应（顺序即管线顺序）：
      compact_budget —— tool_result_budget：末批超大工具结果落盘换预览
      compact_snip   —— snip_compact：中段历史归档到 snip_archive.jsonl
      compact_micro  —— micro_compact：已消化的旧工具结果换 Format B 标记
      compact_fit    —— fit_tool_results：工具结果换 1000 字符预览（Format A）

    标签类别：ToolTagKindNames 未识别一律落 "other"（中性灰），与 "compact" /
    "memory" 同款——不新增 kind，故三主题 QSS 零改动。

    命名域：全小写 + 下划线 + `compact_` 前缀，与真工具名零交集（tst_toolnames
    逐条对表钉住）；前缀同时让用户在等宽标签上一眼认出「这是压缩家族」。
*/

#include <QString>

namespace CompactStageNames
{
    inline const QString BUDGET = QStringLiteral("compact_budget");
    inline const QString SNIP = QStringLiteral("compact_snip");
    inline const QString MICRO = QStringLiteral("compact_micro");
    inline const QString FIT = QStringLiteral("compact_fit");
} // namespace CompactStageNames
