#pragma once

// 文本文件行尾归一单源（header-only 静态函数集，无状态、无实例，被 include 即可编译）：
// read_file 以 QTextStream 逐行读、再以 '\n' join 交还模型，模型看到的永远是 LF 归一化文本；
// 而落盘文件在本仓（core.autocrlf=true）多为 CRLF。两侧口径不一致会同时产出两个缺陷：
//   ① edit_file 拿原始字节直接匹配 → 模型从 read_file 输出抄来的多行 old_string 在 CRLF
//      文件上必然失配（报 text not found），多行编辑等于不可用；
//   ② 模型给的 LF new_string 原样插入 CRLF 文件 → 混入裸 LF，产出混合行尾、污染 git diff。
// 故统一纪律：**匹配域归一到 LF，写回域按文件主导行尾还原**。使用方：AgentLoopFileTools.cpp
// 的 runEditFileIn（两级匹配替换）与 runWriteFileIn（覆盖已存在文件时保真行尾）。
//
// 孤立 CR（老 Mac 行尾）不在本仓范围：toLf 只折 CRLF，不动单独 '\r'，避免误伤正文里的 CR 字面量。

#include <QString>

namespace LineEnding {

enum class Style
{
    Lf,   // 换行以裸 '\n' 为主（含无换行/空文本：写回不引入任何转换）
    Crlf  // 换行以 "\r\n" 为主
};

// 主导行尾判定：CRLF 数 >= 裸 LF 数且非零则判 CRLF。平局偏 CRLF 是 Windows 优先裁决
// （本仓 core.autocrlf=true，混合文件多为「CRLF 主体 + 少量误入的裸 LF」，正是待归一的噪声）。
inline Style dominant(const QString &text)
{
    qsizetype crlf = 0;
    qsizetype bareLf = 0;
    for (qsizetype i = 0; i < text.size(); ++i)
    {
        if (text.at(i) != QLatin1Char('\n'))
            continue;
        if (i > 0 && text.at(i - 1) == QLatin1Char('\r'))
            ++crlf;
        else
            ++bareLf;
    }
    return (crlf > 0 && crlf >= bareLf) ? Style::Crlf : Style::Lf;
}

// 归一到 LF（匹配域）：仅折 CRLF，孤立 CR 保持原样
inline QString toLf(const QString &text)
{
    QString out = text;
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return out;
}

// 按主导行尾渲染（写回域）：先归一再展开，保证幂等——已是 CRLF 的文本不会被二次展开成 "\r\r\n"
inline QString apply(const QString &text, Style style)
{
    QString out = toLf(text);
    if (style == Style::Crlf)
        out.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    return out;
}

// 命中次数统计（不重叠计数，与 replaceOnce 的两级匹配同口径：在哪个域命中就在哪个域计数）。
// 用途：edit_file 的歧义拒绝——old_string 在文件里出现多次时，「替换第一处」改的是哪一处
// 取决于文件里恰好先出现哪个，模型无从判断，静默改错位置比失败更危险。
// firstIndex 由调用方传入（即已求出的首次命中位置），避免重复扫一遍前缀。
inline int countOccurrences(const QString &haystack, const QString &needle, int firstIndex)
{
    if (needle.isEmpty())
        return 1; // 空 needle 不参与计数：indexOf("") 恒 0 会死循环，调用方已先行拒绝空 old_string
    int count = 1;
    int from = firstIndex + needle.size();
    while (from <= haystack.size())
    {
        const int next = haystack.indexOf(needle, from);
        if (next < 0)
            break;
        ++count;
        from = next + needle.size();
    }
    return count;
}

// 单次替换（对齐 lcc str.replace(old, new, 1) 的「只替换第一处」语义），两级匹配：
//   ① 先按文件主导行尾渲染 old/new 后在**原文**匹配——命中则只动匹配区间，其余字节逐字保留，
//      diff 无噪声（这是绝大多数情形：文件行尾本身统一）；
//   ② 失配再退到 LF 归一化全文匹配（混合行尾文件才走到这），写回时整文件按主导行尾归一，
//      顺带把误入的裸 LF 收敛掉，属可接受副作用。
// 出参（均 nullptr 容错，与 SessionStore 的 error 出参同纪律）：
//   matched    —— 是否命中；未命中时返回值无意义，调用方须折叠成可判定错误文本（B1 约定）
//   matchCount —— 命中次数（0 = 未命中）。调用方据此拒绝歧义编辑：>1 时不得静默替换第一处，
//                 须回可判定错误要求补上下文使其唯一（本仓对 lcc 语义的有意偏离，见 runEditFileIn）
inline QString replaceOnce(const QString &text, const QString &oldString, const QString &newString,
                           bool *matched, int *matchCount)
{
    const Style style = dominant(text);

    const QString oldRendered = apply(oldString, style);
    const int rawIndex = text.indexOf(oldRendered);
    if (rawIndex >= 0)
    {
        if (matched)
            *matched = true;
        if (matchCount)
            *matchCount = countOccurrences(text, oldRendered, rawIndex);
        QString edited = text;
        edited.replace(rawIndex, oldRendered.size(), apply(newString, style));
        return edited;
    }

    QString normalized = toLf(text);
    const QString oldLf = toLf(oldString);
    const int index = normalized.indexOf(oldLf);
    if (index < 0)
    {
        if (matched)
            *matched = false;
        if (matchCount)
            *matchCount = 0;
        return QString();
    }
    if (matched)
        *matched = true;
    if (matchCount)
        *matchCount = countOccurrences(normalized, oldLf, index);
    normalized.replace(index, oldLf.size(), toLf(newString));
    return apply(normalized, style);
}

} // namespace LineEnding
