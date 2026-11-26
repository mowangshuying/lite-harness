#pragma once

/*
    PasteText.h —— 粘贴文本归一判据单源（只依赖 Qt6::Core）

    为什么单独一个纯头（而不是把判据写在控件里）：
    输入框是 QTextEdit，剪贴板带 text/html 时 Qt 会优先吃 HTML 并把行内样式落成逐字符
    format，盖掉本仓 QSS 给 #textEdit 的字色——深色主题下白字看不见、网页标题变 24px、
    代码块带语法色底。修掉富文本只需 setAcceptRichText(false) 一条，但「粘贴进来的原文里
    还有一批不可见字符会破坏排版」这件事是**判据**，错了不报错、只在输入框里表现成
    「凭空多出的断行 / 看不见却占位的空格」，只能靠断言钉。故把字符级判据抽在本头，
    控件只做「只吃 text/plain + 调本头」两件事，判据由 tst_pastetext 直测
    （沿用 CompactStageNames.h / ScrollSettle.h 的 header-only 模式：零 IO、
    不占测试目标 TU 清单、测试直接 include 生产头）。

    与「内容整形」的边界（用户裁决）：本头**只处理不可见字符**，不动可见内容——
    不折叠连续空行、不清行尾空白、不压缩进空格、不设体量上限。粘贴结果与源文本
    逐字符同源（剥掉的是渲染成断行或占位的隐形字符），这样「粘贴后内容变了」永远
    不会是静默行为，用户不必怀疑自己的原文被动过。

    三类保留项刻意**不**处理，理由各异：
      U+3000 全角空格 —— CJK 排版的合法字符，转半角等于改内容（中文用户可见差异）
      U+200C ZWNJ / U+200D ZWJ —— emoji 家庭/职业序列（👨‍👩‍👧）靠 ZWJ 粘连，剥掉会拆成
                        多个独立 emoji；剥 ZWNJ 还会破坏阿拉伯文/印度文连字
      U+0009 TAB        —— 从编辑器粘代码时缩进保真比对齐观感重要；行首缩进属可见内容

    代理对（U+D800–U+DFFF 区间）逐码元透传，绝不参与任何剥离/替换分支：emoji 与 CJK
    扩展平面字符走的就是这条路，tst_pastetext 单独钉一条「高代理后紧跟低代理不被拆开」。
*/

#include <QString>
#include <QChar>

namespace PasteText
{
    // 归一粘贴文本：行尾统一成 LF、剥掉会破坏排版的不可见字符。
    // 判据幂等（normalize(normalize(x)) == normalize(x)），故重复调用无副作用。
    inline QString normalize(const QString &raw)
    {
        QString out;
        out.reserve(raw.size());
        for (int i = 0; i < raw.size(); ++i)
        {
            const ushort u = raw.at(i).unicode();

            // CR 单独成支：必须「先看下一个码元」，否则 CRLF 会归出两个 LF（多一个空行）。
            // 紧随 LF 就一并吃掉；落单的 CR（老式 Mac 文本、PDF 导出常见）归成 LF。
            if (u == 0x000D)
            {
                if (i + 1 < raw.size() && raw.at(i + 1).unicode() == 0x000A)
                    ++i;
                out.append(QChar(0x000A));
                continue;
            }

            // U+2028 LINE SEPARATOR / U+2029 PARAGRAPH SEPARATOR：网页与 JSON 转义里混得进来，
            // QTextDocument 把它当断行渲染（用户在剪贴板里看不见它，只见「莫名换行」）→ 归 LF
            if (u == 0x2028 || u == 0x2029)
            {
                out.append(QChar(0x000A));
                continue;
            }

            // C0 控制符：只留 TAB 与 LF。U+000B 垂直制表、U+000C 换页在文档里同样画成断行，
            // 与上面的 2028/2029 同属「剪贴板带进来、用户看不见、屏幕上却多一行」
            if (u < 0x0020)
            {
                if (u == 0x0009 || u == 0x000A)
                    out.append(QChar(u));
                continue;
            }

            // 剥离族：软连字符、零宽空格、双向标记、词连字符、BOM/ZWNBSP、DEL。
            // 共同点是在屏幕上不占宽（或占一个看不见的宽）却真实存在于字符串里，
            // 会让用户复制粘贴一轮后「同一个词搜不到」。
            if (u == 0x007F || u == 0x00AD || u == 0x200B || u == 0x200E ||
                u == 0x200F || u == 0x2060 || u == 0xFEFF)
                continue;

            // 空白族转 ASCII 空格（不含 U+3000，见文件头保留项）：NBSP 系字符在等宽对齐里
            // 与普通空格视觉上无差、语义上却不同，粘进代码/命令即成静默差异源
            if (u == 0x00A0 || u == 0x1680 || u == 0x202F || u == 0x205F ||
                (u >= 0x2000 && u <= 0x200A))
            {
                out.append(QChar(u' '));
                continue;
            }

            out.append(QChar(u));
        }
        return out;
    }
} // namespace PasteText
