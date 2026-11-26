// 粘贴文本归一判据守卫（PasteText.h）。
//
// 守什么：行尾归一（CRLF/落单 CR/U+2028/U+2029 → 单 LF）、C0 控制符与不可见字符的剥离
// 名单、空白族折叠、三类刻意保留项（U+3000 全角空格、ZWNJ/ZWJ、TAB）、幂等性、
// 代理对不被拆。
//
// 为什么值得钉：这些字符在屏幕上**看不见**，却各自有可见后果——U+000C 换页与 U+2028
// 行分隔符会被 QTextDocument 画成断行（用户只见「莫名多出一行」），NBSP 与半角空格视觉
// 无差却搜不到、粘进命令会静默改变行为，ZWJ 一剥 emoji 家庭序列就拆成几个独立 emoji。
// 判据错了不报错，只在输入框里表现成排版怪象，只能靠断言钉住。
//
// 不守什么（诚实边界）：
//   ① 「剪贴板的 text/html 不再进文档」——那是 PlainPasteTextEdit 的覆写口径，长在
//      Qt6::Widgets 的 QTextEdit 上，测试目标只链 Qt6::Core（CMakeLists.txt 的测试目标
//      纪律），进不了 ctest；由冒烟（真机 Ctrl+V 一段带样式文本 + 截屏）取证。
//   ② 富文本渲染结果、QSS 字色覆盖与否——同属 GUI 侧。
//   ③ 「内容整形」按用户裁决**刻意不做**：不折叠连续空行、不清行尾空白、不设体量上限，
//      故本套件同时钉住几条否定式断言，防止下一个人「顺手加上」。

#include "TestHarness.h"
#include "PasteText.h"

#include <initializer_list>

namespace {

// 按码元构造输入串：控制符与不可见字符一律走码位，不写字面转义——
// 尤其 NUL，写成 "a\0b" 时 QStringLiteral 与 fromUtf8 两种构造结果不同，测的会是构造 API
// 而不是判据本身。
QString fromCodes(std::initializer_list<uint> codes)
{
    QString out;
    for (const uint code : codes)
        out.append(QChar(static_cast<ushort>(code)));
    return out;
}

int countOf(const QString &text, QChar ch)
{
    int n = 0;
    for (const QChar &c : text)
        if (c == ch)
            ++n;
    return n;
}

bool containsAny(const QString &text, std::initializer_list<uint> codes)
{
    for (const uint code : codes)
        if (text.contains(QChar(static_cast<ushort>(code))))
            return true;
    return false;
}

} // namespace

int tst_pastetext()
{
    using TestHarness::check;
    using PasteText::normalize;

    const QChar CR = QChar(0x000D);
    const QChar LF = QChar(0x000A);
    const QChar TAB = QChar(0x0009);

    // ① 行尾归一：CRLF 归**一个** LF（判据必须前瞻，否则每段 Windows 文本粘贴完都翻倍）
    check(normalize(QStringLiteral("a\r\nb")) == QStringLiteral("a\nb"),
          "CRLF -> 单 LF（不产生空行）");
    check(normalize(QStringLiteral("a\rb")) == QStringLiteral("a\nb"),
          "落单 CR -> LF（老式 Mac 文本 / PDF 导出）");
    check(countOf(normalize(QStringLiteral("a\r\n\r\nb")), LF) == 2,
          "连续 CRLF 段落分隔 -> 恰好两个 LF");
    check(countOf(normalize(fromCodes({0x61, 0x000D, 0x000D, 0x000A, 0x62})), LF) == 2,
          "CR + CRLF 混排 -> 两个 LF（前瞻只吃配对的那个 LF）");
    check(normalize(QStringLiteral("a\n\r\nb")) == QStringLiteral("a\n\nb"),
          "LF 后跟 CRLF -> 两个 LF（前一个 LF 不被误吃）");
    check(!normalize(QStringLiteral("x\r\ny\rz")).contains(CR),
          "归一结果里不残留 CR");
    check(normalize(QStringLiteral("plain text")) == QStringLiteral("plain text"),
          "纯 ASCII 逐字不变");
    check(normalize(QString()).isEmpty(), "空串 -> 空串（不猜内容）");
    check(normalize(QString(LF)) == QString(LF), "单个 LF 原样保留");

    // ② U+2028 / U+2029：网页与 JSON 转义里混得进来，QTextDocument 把它们画成断行
    check(normalize(fromCodes({0x61, 0x2028, 0x62})) == QStringLiteral("a\nb"),
          "U+2028 LINE SEPARATOR -> LF");
    check(normalize(fromCodes({0x61, 0x2029, 0x62})) == QStringLiteral("a\nb"),
          "U+2029 PARAGRAPH SEPARATOR -> LF");
    check(countOf(normalize(fromCodes({0x61, 0x2028, 0x2029, 0x62})), LF) == 2,
          "连续两个行/段分隔 -> 两个 LF（各自独立成断行，符合原排版）");
    check(!containsAny(normalize(fromCodes({0x61, 0x2028, 0x2029, 0x62})), {0x2028, 0x2029}),
          "归一结果里不残留 U+2028/U+2029");

    // ③ C0 控制符：只留 TAB 与 LF，其余一律剥（含 VT/FF 这两个会画成断行的）
    check(normalize(fromCodes({0x61, 0x0000, 0x62})) == QStringLiteral("ab"),
          "NUL 剥离");
    check(normalize(fromCodes({0x61, 0x0001, 0x02, 0x08, 0x62})) == QStringLiteral("ab"),
          "C0 其余控制符剥离");
    check(normalize(fromCodes({0x61, 0x000B, 0x62})) == QStringLiteral("ab"),
          "U+000B 垂直制表剥离（不转 LF——它原本是隐形字符，转成断行等于凭空加一行）");
    check(normalize(fromCodes({0x61, 0x000C, 0x62})) == QStringLiteral("ab"),
          "U+000C 换页剥离（同上：换页在文档里画成断行）");
    check(normalize(fromCodes({0x61, 0x000E, 0x1F, 0x62})) == QStringLiteral("ab"),
          "U+000E–U+001F 区间剥离");
    check(normalize(fromCodes({0x61, 0x007F, 0x62})) == QStringLiteral("ab"),
          "U+007F DEL 剥离");
    check(normalize(fromCodes({0x61, 0x0009, 0x62})) == fromCodes({0x61, 0x0009, 0x62}),
          "TAB 保留（粘代码时缩进保真，属可见内容）");
    check(normalize(QStringLiteral("\tdef(a);")).startsWith(TAB),
          "行首 TAB 不被吃掉");

    // ④ 隐形字符剥离族：不占宽却真实存在，复制一轮后「同一个词搜不到」
    check(normalize(fromCodes({0x61, 0x00AD, 0x62})) == QStringLiteral("ab"),
          "U+00AD 软连字符剥离");
    check(normalize(fromCodes({0x61, 0x200B, 0x62})) == QStringLiteral("ab"),
          "U+200B 零宽空格剥离");
    check(normalize(fromCodes({0x61, 0x200E, 0x62})) == QStringLiteral("ab"),
          "U+200E LRM 剥离");
    check(normalize(fromCodes({0x61, 0x200F, 0x62})) == QStringLiteral("ab"),
          "U+200F RLM 剥离");
    check(normalize(fromCodes({0x61, 0x2060, 0x62})) == QStringLiteral("ab"),
          "U+2060 WORD JOINER 剥离");
    check(normalize(fromCodes({0xFEFF, 0x61, 0x62})) == QStringLiteral("ab"),
          "U+FEFF BOM 剥离（含串首位置：从文件复制首行时最常见）");

    // ⑤ 空白族折叠为 ASCII 空格（NBSP 系与半角空格视觉无差、语义不同）
    check(normalize(fromCodes({0x61, 0x00A0, 0x62})) == QStringLiteral("a b"),
          "U+00A0 NBSP -> 空格");
    check(normalize(fromCodes({0x61, 0x2000, 0x62})) == QStringLiteral("a b"),
          "U+2000 EN QUAD -> 空格");
    check(normalize(fromCodes({0x61, 0x2007, 0x62})) == QStringLiteral("a b"),
          "U+2007 FIGURE SPACE -> 空格");
    check(normalize(fromCodes({0x61, 0x200A, 0x62})) == QStringLiteral("a b"),
          "U+200A HAIR SPACE -> 空格");
    check(normalize(fromCodes({0x61, 0x202F, 0x62})) == QStringLiteral("a b"),
          "U+202F NARROW NBSP -> 空格（法语/俄语排版数字前常见）");
    check(normalize(fromCodes({0x61, 0x205F, 0x62})) == QStringLiteral("a b"),
          "U+205F MEDIUM MATHEMATICAL SPACE -> 空格");
    check(normalize(fromCodes({0x61, 0x1680, 0x62})) == QStringLiteral("a b"),
          "U+1680 OGHAM SPACE MARK -> 空格");

    // ⑥ 三类刻意保留项（否定式断言：防下一个人「顺手统一」而静默改用户内容）
    check(normalize(fromCodes({0x4F60, 0x3000, 0x597D})) == fromCodes({0x4F60, 0x3000, 0x597D}),
          "U+3000 全角空格保留（CJK 合法排版字符，转半角等于改内容）");
    check(normalize(fromCodes({0x61, 0x200D, 0x62})) == fromCodes({0x61, 0x200D, 0x62}),
          "U+200D ZWJ 保留（emoji 家庭序列靠它粘连）");
    check(normalize(fromCodes({0x61, 0x200C, 0x62})) == fromCodes({0x61, 0x200C, 0x62}),
          "U+200C ZWNJ 保留（阿拉伯文/印度文连字）");
    check(containsAny(normalize(fromCodes({0x61, 0x200B, 0x200D, 0x00A0, 0x200C})), {0x200D, 0x200C}),
          "同一串里 ZWSP/NBSP 被处理、ZWJ/ZWNJ 存活（两类判据互不误伤）");

    // ⑦ 代理对（扩展平面：emoji 与 CJK 兼容表意文字）逐码元透传，不得参与任何剥离分支
    check(normalize(fromCodes({0xD83D, 0xDE00})) == fromCodes({0xD83D, 0xDE00}),
          "emoji 😀（D83D DE00）两个码元原样保留");
    check(normalize(fromCodes({0xD83D, 0x200B, 0xDE00})) == fromCodes({0xD83D, 0xDE00}),
          "夹在代理对中间的 ZWSP 被剥，代理对仍相邻且不被拆");
    check(normalize(fromCodes({0xD83D, 0xDE00})).size() == 2,
          "代理对长度不变（不被折叠成 1 个码元）");
    check(normalize(fromCodes({0xD800})) == fromCodes({0xD800}),
          "落单高代理不报错、原样透传（剪贴板串可能被上游截断）");

    // ⑧ 幂等：normalize(normalize(x)) == normalize(x)——输入框可被反复粘贴/撤销重放
    const QString nasty = fromCodes({0x61, 0x000D, 0x000A, 0x62, 0x2028, 0x63, 0x00A0, 0x64,
                                    0x200B, 0x65, 0xFEFF, 0x3000, 0x0009, 0x66, 0xD83D, 0xDE00});
    const QString once = normalize(nasty);
    check(normalize(once) == once, "幂等：二次归一不再改变结果");
    check(once == fromCodes({0x61, 0x000A, 0x62, 0x000A, 0x63, 0x0020, 0x64, 0x65,
                             0x3000, 0x0009, 0x66, 0xD83D, 0xDE00}),
          "混合脏串逐码元终值（CR/CRLF 合成一个 LF、2028 转 LF、NBSP 转空格、"
          "ZWSP 与 BOM 剥掉、全角空格与 TAB 与 emoji 存活）");
    check(!containsAny(once, {0x000D, 0x2028, 0x2029, 0x00A0, 0x200B, 0xFEFF}),
          "混合脏串里六类问题字符全部消失");
    check(countOf(once, LF) == 2, "混合脏串只剩两个断行（CRLF 归一个、U+2028 归一个）");

    // ⑨ 不越界：本头只做不可见字符归一，不做 trim / 不做空行折叠 / 不设体量上限
    check(normalize(QStringLiteral("  a  ")) == QStringLiteral("  a  "),
          "行首行尾空白不裁（trim 归发送口 ChatMsgEdit::toPlainText().trimmed()）");
    check(countOf(normalize(QStringLiteral("a\n\n\n\n\nb")), LF) == 5,
          "连续空行不折叠（用户裁决：改内容比排版难看更糟）");
    const QString big = QStringLiteral("x").repeated(200000);
    check(normalize(big) == big, "不设体量上限（用户裁决）；200000 码元逐字往返");
    check(normalize(big).size() == 200000, "体量不变：归一只删隐形字符，不截断");

    // ⑩ 中文正文（本仓源语言）逐字不变——归一判据不得对 CJK 有任何副作用
    const QString cjk = QStringLiteral("上下文压缩：把超长历史压到阈值以下，输入框粘贴也走这条判据。");
    check(normalize(cjk) == cjk, "中文正文逐字不变");

    return 0;
}
