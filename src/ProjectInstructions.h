#pragma once

// 项目指令文件（AGENTS.md）判据单源（header-only、仅 QtCore，纪律同 MemoryRules.h / ToolSchema.h）：
// 两级候选顺序、命中即止择一、缓存签名、注入块第 0 段的拼装与截断。
//
// 为什么单独成头：这四条判据若留在 AgentLoopInstructions.cpp，该 TU 要链 QOpenAi.h
// （Qt6::Network），按 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest，
// 于是整套判据零直测。而它们恰是「跑得通但喂错料」的高危面：
//   · 候选顺序写反（全局压项目）：用户改了仓库里的 AGENTS.md，模型照旧读全局那份，
//     表现为「规则写了不生效」，无任何报错——本仓唯一的判据就是顺序，只能钉死；
//   · 命中即止写成「两个都读」：两段指令互相打架，token 双份，且没人知道哪条赢；
//   · 空正文仍拼段头：注入块里出现「Project instructions ... :」后跟空行，模型读到
//     一个没有内容的指令段，比完全不注入更容易诱发臆测；
//   · 截断静默吞尾：16000 字符后的规则（如发布纪律、禁翻区纪律）凭空消失且无标记，
//     模型会自信地宣称「已遵循项目全部约定」。
//
// 本头零 IO、零 QCoreApplication 依赖：存在性判定经 std::function 注入（生产侧传
// QFileInfo::isFile+isReadable，测试侧传桩表），路径派生经 LitePaths 的纯口 *For。

#include <QString>
#include <QStringList>
#include <functional>

#include "LitePaths.h"

namespace ProjectInstructions {

// 候选顺序单源：工程根在前（随仓库走、团队共享），存储根在后（本机全局兜底）。
// 互斥回退而非叠加——两级都读会引入「谁覆盖谁」这个无法在本层消解的冲突。
// 空串入参跳过：LitePaths::joined 对空根是 Q_ASSERT 硬炸，release 下会拼出 "/AGENTS.md"
// 这种「当前驱动器根」形态（该形态的成因见 LitePaths.h 头部注释）。
inline QStringList candidatesFor(const QString &workDir, const QString &configRoot)
{
    QStringList out;
    if (!workDir.trimmed().isEmpty())
        out.append(LitePaths::agentsFileFor(workDir));
    if (!configRoot.trimmed().isEmpty())
        out.append(LitePaths::agentsFileFor(configRoot));
    return out;
}

// 命中即止：返回第一个 exists 判真的候选，其后候选不再探测；全不命中返回空串。
inline QString selectExisting(const QStringList &candidates,
                              const std::function<bool(const QString &)> &exists)
{
    for (const QString &candidate : candidates) {
        if (exists(candidate))
            return candidate;
    }
    return QString();
}

// 惰性缓存签名单源：路径 + mtime(ms) + 字节数。三段任一变化即换签名，
// 于是「会话中途编辑 AGENTS.md」在下一回合自然取新值，不需要显式失效钩子。
inline QString signatureFor(const QString &path, qint64 modifiedMs, qint64 sizeBytes)
{
    return path + QLatin1Char('|') + QString::number(modifiedMs) + QLatin1Char('|')
        + QString::number(sizeBytes);
}

// 注入块第 0 段：段标题英文（与既有 "Skills available:" / "Memory catalog:" 同风格）
// + 来源路径 + 正文。返回空串 = 不产生该段（空段省略纪律，见 AgentLoopPrompt.cpp）。
// 正文为空白仍返回空串，但**不回退到下一级候选**：回退属选择层语义，命中文件后
// 正文空是该文件自身的状态，静默改读全局那份等于把用户项目的显式清空当没看见。
// 截断：超预算时只下发前 budgetChars 个字符，并在段标题里如实标注「前 N / 共 M」，
// 让模型知道看到的是残篇；budgetChars <= 0 表示不设预算、全量下发。
// 拼接纪律：path 与正文一律 '+' 追加，数字也用 QString::number 后 '+'——**整条路径不经过
// arg()**。两个理由：①'+' 不解释 '%'，path 或正文里的 "%N" 形态串不会被展开（Windows 目录名
// 允许 '%'，如 "D:/100%/repo"）；②链式 .arg(a, b) 传两个整数会命中
// `arg(qlonglong a, int fieldWidth, ...)` 重载——第二个整数被当成**字段宽度**而不是 %2 的值，
// 产出的段标题既错又静默（本批实测踩过：期望 "first 9 of 20 characters shown"，实际拿到
// 把 20 当宽度补空格的结果）。同 AgentLoopPrompt.cpp makeSystemPrompt 的 head/tail 拆分纪律。
inline QString formatSection(const QString &path, const QString &text, qsizetype budgetChars)
{
    if (path.isEmpty() || text.trimmed().isEmpty())
        return QString();
    if (budgetChars > 0 && text.size() > budgetChars) {
        return QStringLiteral("Project instructions (AGENTS.md from ") + path
            + QStringLiteral(", first ") + QString::number(budgetChars)
            + QStringLiteral(" of ") + QString::number(text.size())
            + QStringLiteral(" characters shown):\n")
            + text.left(budgetChars);
    }
    return QStringLiteral("Project instructions (AGENTS.md from ") + path
        + QStringLiteral("):\n") + text;
}

} // namespace ProjectInstructions
