// LineEnding.h 行尾口径单测：直接 include 生产头，测的是真实编译产物而非复制的算法。
//
// 覆盖的回归点全部是实际踩过的坑：
//   · CRLF 文件 + 从 read_file 输出抄来的 LF 多行 old_string 必须命中（曾必然 text not found）
//   · 写回不得混入裸 LF（曾产出混合行尾、污染 git diff）
//   · LF 文件不得被转成 CRLF
//   · 带 BOM 文件的编码守卫不得误拒（QString::fromUtf8 会吃掉前导 BOM，守卫必须按去 BOM 后的
//     正文字节比较——这条正是本测试第一跑抓出来的真 bug）
//   · 多处命中必须报出次数，供 edit_file 拒绝歧义编辑

#include "TestHarness.h"

#include "LineEnding.h"

#include <QByteArray>
#include <QString>

namespace {

using LineEnding::Style;

// 把行尾可视化，失败时便于肉眼定位（<CR>/<LF> 显式打印）
QString vis(const QString &s)
{
    QString out = s;
    out.replace(QLatin1String("\r"), QLatin1String("<CR>"));
    out.replace(QLatin1String("\n"), QLatin1String("<LF>\n"));
    return out;
}

void testDominant()
{
    TestHarness::check(LineEnding::dominant(QStringLiteral("a\r\nb\r\n")) == Style::Crlf,
                       "dominant: 全 CRLF -> Crlf");
    TestHarness::check(LineEnding::dominant(QStringLiteral("a\nb\n")) == Style::Lf, "dominant: 全 LF -> Lf");
    TestHarness::check(LineEnding::dominant(QString()) == Style::Lf, "dominant: 空文本 -> Lf");
    TestHarness::check(LineEnding::dominant(QStringLiteral("abc")) == Style::Lf, "dominant: 无换行 -> Lf");
    TestHarness::check(LineEnding::dominant(QStringLiteral("a\r\nb\nc\r\n")) == Style::Crlf,
                       "dominant: CRLF 多数 -> Crlf");
    TestHarness::check(LineEnding::dominant(QStringLiteral("a\nb\r\n")) == Style::Crlf,
                       "dominant: 平局偏 Crlf（Windows 优先裁决）");
    TestHarness::check(LineEnding::dominant(QStringLiteral("a\nb\nc\r\n")) == Style::Lf,
                       "dominant: LF 多数 -> Lf");
}

void testToLfAndApply()
{
    TestHarness::check(LineEnding::toLf(QStringLiteral("a\r\nb")) == QStringLiteral("a\nb"), "toLf: CRLF 折叠");
    TestHarness::check(LineEnding::toLf(QStringLiteral("a\rb")) == QStringLiteral("a\rb"),
                       "toLf: 孤立 CR 不动（不误伤正文 CR 字面量）");
    TestHarness::check(LineEnding::apply(QStringLiteral("a\nb"), Style::Crlf) == QStringLiteral("a\r\nb"),
                       "apply: LF -> CRLF");
    TestHarness::check(LineEnding::apply(QStringLiteral("a\r\nb"), Style::Crlf) == QStringLiteral("a\r\nb"),
                       "apply: 幂等，不产生 CR CR LF");
    TestHarness::check(LineEnding::apply(QStringLiteral("a\r\nb"), Style::Lf) == QStringLiteral("a\nb"),
                       "apply: Lf 风格归一");
}

void testReplaceOnceCoreRegression()
{
    // 核心缺陷回归：CRLF 文件 + LF 多行 old_string
    const QString text = QStringLiteral("alpha\r\nbeta\r\ngamma\r\n");
    bool matched = false;
    int count = 0;
    const QString out =
        LineEnding::replaceOnce(text, QStringLiteral("alpha\nbeta"), QStringLiteral("ALPHA\nBETA-2"), &matched, &count);
    TestHarness::check(matched, "replaceOnce: CRLF 文件 + LF 多行 old_string 命中");
    TestHarness::check(count == 1, "replaceOnce: 唯一命中计数为 1");
    TestHarness::check(out == QStringLiteral("ALPHA\r\nBETA-2\r\ngamma\r\n"), "replaceOnce: 结果按 CRLF 渲染");
    TestHarness::check(out.count(QLatin1String("\r\n")) == out.count(QLatin1Char('\n')),
                       "replaceOnce: 结果无裸 LF（不产出混合行尾）");
    if (out != QStringLiteral("ALPHA\r\nBETA-2\r\ngamma\r\n"))
        std::printf("  实际结果: %s", qPrintable(vis(out)));
}

void testReplaceOnceLfFileUnchanged()
{
    const QString text = QStringLiteral("alpha\nbeta\ngamma\n");
    bool matched = false;
    int count = 0;
    const QString out =
        LineEnding::replaceOnce(text, QStringLiteral("alpha\nbeta"), QStringLiteral("ALPHA\nBETA-2"), &matched, &count);
    TestHarness::check(matched && count == 1, "replaceOnce: LF 文件命中");
    TestHarness::check(out == QStringLiteral("ALPHA\nBETA-2\ngamma\n"), "replaceOnce: LF 文件不被转成 CRLF");
}

void testReplaceOnceSingleLineNoRegression()
{
    const QString text = QStringLiteral("alpha\r\nbeta\r\n");
    bool matched = false;
    int count = 0;
    const QString out = LineEnding::replaceOnce(text, QStringLiteral("beta"), QStringLiteral("BETA"), &matched, &count);
    TestHarness::check(matched && out == QStringLiteral("alpha\r\nBETA\r\n"), "replaceOnce: 单行替换不退化");
    TestHarness::check(count == 1, "replaceOnce: 单行唯一命中计数为 1");
}

void testReplaceOnceNotFound()
{
    bool matched = true;
    int count = -1;
    LineEnding::replaceOnce(QStringLiteral("a\r\nb\r\n"), QStringLiteral("zzz"), QStringLiteral("y"), &matched, &count);
    TestHarness::check(!matched, "replaceOnce: 未命中回 matched=false");
    TestHarness::check(count == 0, "replaceOnce: 未命中回 matchCount=0");
}

void testReplaceOnceOnlyFirst()
{
    // 「只替换第一处」语义保留（对齐 lcc str.replace(old, new, 1)）；歧义由调用方按 count 拒绝
    bool matched = false;
    int count = 0;
    const QString out = LineEnding::replaceOnce(QStringLiteral("x\nx\nx\n"), QStringLiteral("x"),
                                                QStringLiteral("y"), &matched, &count);
    TestHarness::check(matched, "replaceOnce: 重复串命中");
    TestHarness::check(out == QStringLiteral("y\nx\nx\n"), "replaceOnce: 只替换第一处");
    TestHarness::check(count == 3, "replaceOnce: 命中次数如实报 3（供 edit_file 拒绝歧义）");
}

void testReplaceOnceMixedEndings()
{
    // 混合行尾文件走 LF 归一化回退，写回按主导行尾归一（2 CRLF vs 1 裸 LF -> 主导 Crlf）
    const QString text = QStringLiteral("a\r\nb\nc\r\n");
    bool matched = false;
    int count = 0;
    const QString out = LineEnding::replaceOnce(text, QStringLiteral("b\nc"), QStringLiteral("B\nC"), &matched, &count);
    TestHarness::check(matched && count == 1, "replaceOnce: 混合行尾经归一化回退命中");
    TestHarness::check(out == QStringLiteral("a\r\nB\r\nC\r\n"), "replaceOnce: 混合行尾写回归一为主导行尾");
}

void testReplaceOnceNullOutParams()
{
    const QString out = LineEnding::replaceOnce(QStringLiteral("a\nb\n"), QStringLiteral("a"), QStringLiteral("z"),
                                                nullptr, nullptr);
    TestHarness::check(out == QStringLiteral("z\nb\n"), "replaceOnce: 出参均为 nullptr 不崩且正常替换");
}

void testCountOccurrences()
{
    TestHarness::check(LineEnding::countOccurrences(QStringLiteral("a\r\na\r\na"), QStringLiteral("a"), 0) == 3,
                       "countOccurrences: 不重叠计数 3");
    TestHarness::check(LineEnding::countOccurrences(QStringLiteral("aaa"), QStringLiteral("aa"), 0) == 1,
                       "countOccurrences: 不重叠语义（aaa 中 aa 只算 1 次）");
    TestHarness::check(LineEnding::countOccurrences(QStringLiteral("abc"), QStringLiteral(""), 0) == 1,
                       "countOccurrences: 空 needle 返回 1 而非死循环");
}

void testUtf8GuardPremises()
{
    // edit_file 编码守卫（往返字节等价）所依赖的三条前提，任一变化都会让守卫误判
    const QByteArray cn = QString::fromUtf8("// 中文注释：版本号单源").toUtf8();
    TestHarness::check(QString::fromUtf8(cn).toUtf8() == cn, "utf8 守卫: 合法中文往返等价（不误拒）");

    const QByteArray bom = QByteArray("\xef\xbb\xbf", 3) + QByteArray("int x = 1;\r\n");
    TestHarness::check(QString::fromUtf8(bom).toUtf8() != bom,
                       "utf8 守卫前提: fromUtf8 吃 BOM 致整段往返不等价（故守卫必须按去 BOM 正文比较）");
    TestHarness::check(QString::fromUtf8(bom).at(0).unicode() == 'i',
                       "utf8 守卫前提: 解码首字符即正文（BOM 已消失）");
    const QByteArray body = bom.mid(3);
    TestHarness::check(QString::fromUtf8(body).toUtf8() == body,
                       "utf8 守卫: 去 BOM 后正文往返等价（带 BOM 文件不被误拒）");

    const QByteArray gbk = QByteArray("\xd6\xd0\xce\xc4", 4); // GBK "中文"，非法 UTF-8
    TestHarness::check(QString::fromUtf8(gbk).toUtf8() != gbk, "utf8 守卫: 非法 UTF-8 往返不等价（正确拒绝）");
}

} // namespace

int tst_lineending()
{
    const int before = TestHarness::failCount();
    testDominant();
    testToLfAndApply();
    testReplaceOnceCoreRegression();
    testReplaceOnceLfFileUnchanged();
    testReplaceOnceSingleLineNoRegression();
    testReplaceOnceNotFound();
    testReplaceOnceOnlyFirst();
    testReplaceOnceMixedEndings();
    testReplaceOnceNullOutParams();
    testCountOccurrences();
    testUtf8GuardPremises();
    return TestHarness::failCount() - before;
}
