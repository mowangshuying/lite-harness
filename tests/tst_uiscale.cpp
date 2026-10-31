// tst_uiscale.cpp —— 显示尺度纯判据直测（src/UiScale.h，header-only 零 CMake 改动）
// 被测：字号倍率校验与折算 / 高 DPI 策略名解析与回退 / 窗口几何 base64 往返。
//
// 本套件为什么必须存在（不是凑覆盖率）：这三条判据全都没有失败信号。
//   · 倍率写成 0 或 999：不报错，只是文字突然看不见或卡片被撑破；
//   · 策略名手改拼错（"roundPreferFlour"）：不报错，静默回落另一档缩放；
//   · 几何串被手改截半：不报错，restoreGeometry 返回 false，窗口回默认尺寸——
//     用户读成「软件忘了我的窗口」，而真相是配置文件里那个值坏了。
// 三条都是「跑得通但结果不对」的路径，只能靠断言钉死，冒烟运行看不全。
//
// 覆盖面边界（勿高估）：本套件测的是**判据与折算**。它不证明 App.cpp 真的在
// QGuiApplication 构造前调了 setHighDpiScaleFactorRoundingPolicy（那是接线，
// Qt 侧实证构造后调用只 qWarning 不生效，见 qguiapplication.cpp:4009），也不证明
// LiteHarness 真的接上了 saveGeometry/restoreGeometry——后者按 QWidget 契约走，
// 验证手段是编译期等价 + 冒烟运行。与 StreamRate.h / UsageTokens.h 同口径。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "UiScale.h"

#include <QtCore/qnamespace.h>
#include <QByteArray>
#include <QString>
#include <QVariant>

namespace {

using TestHarness::check; // 本套件断言密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// 策略名解析的小助手：把「盘上串 → 枚举 → 展示名」走完整条链，
// 断言时同时看到两端，错值回落到哪一档一目了然。
QString policyRoundTrip(const QString &name)
{
    return UiScale::dpiPolicyName(UiScale::dpiPolicyFromName(name));
}

} // namespace

int tst_uiscale()
{
    using namespace UiScale;

    // ---- ① 字号倍率：校验界 ----
    // 缺失/空串/非数字/越界一律回退 100（= 不缩放），绝不返回 0 或脏值
    check(fontScalePercent(QVariant()) == kFontScaleDefault, "fontScale: missing key -> default");
    check(fontScalePercent(QVariant(QString())) == kFontScaleDefault, "fontScale: empty string -> default");
    check(fontScalePercent(QVariant(QStringLiteral("abc"))) == kFontScaleDefault, "fontScale: non-numeric -> default");
    check(fontScalePercent(QVariant(0)) == kFontScaleDefault, "fontScale: 0 -> default (not 'invisible')");
    check(fontScalePercent(QVariant(-50)) == kFontScaleDefault, "fontScale: negative -> default");
    check(fontScalePercent(QVariant(kFontScaleMin - 1)) == kFontScaleDefault, "fontScale: below min -> default");
    check(fontScalePercent(QVariant(kFontScaleMax + 1)) == kFontScaleDefault, "fontScale: above max -> default");
    // 边界值本身必须被接受（闭区间）：下界/上界/默认三点
    check(fontScalePercent(QVariant(kFontScaleMin)) == kFontScaleMin, "fontScale: min bound accepted");
    check(fontScalePercent(QVariant(kFontScaleMax)) == kFontScaleMax, "fontScale: max bound accepted");
    check(fontScalePercent(QVariant(150)) == 150, "fontScale: in-range passthrough");
    // 字符串形态的合法数字也要能读回（配置文件是手改面，QSettings 交出来的常是 QString）
    check(fontScalePercent(QVariant(QStringLiteral("120"))) == 120, "fontScale: numeric string reads back");
    check(fontScalePercent(QVariant(QStringLiteral(" 120 "))) == 120, "fontScale: surrounding spaces tolerated");
    // 千分位不是我们的约定（与 contextCharLimit 的展示格式刻意不同）：带逗号的串判非法回退，
    // 这条断言把「回退」钉成事实，而不是某天有人顺手加个 remove(',') 把它变成静默接受
    check(fontScalePercent(QVariant(QStringLiteral("1,20"))) == kFontScaleDefault, "fontScale: thousands separator rejected");

    // ---- ① 字号倍率：折算 ----
    check(qAbs(scaleFactor(100) - 1.0) < 1e-12, "scaleFactor: 100% == 1.0");
    check(qAbs(scaleFactor(150) - 1.5) < 1e-12, "scaleFactor: 150% == 1.5");
    check(qAbs(scaleFactor(80) - 0.8) < 1e-12, "scaleFactor: 80% == 0.8");

    // 整数量子走四舍五入：截断会让 12px×1.05 变回 12px（「调了没反应」）
    check(scaledSize(12, 100) == 12, "scaledSize: 100% is identity");
    check(scaledSize(12, 105) == 13, "scaledSize: rounds up, does not truncate");
    check(scaledSize(12, 95) == 11, "scaledSize: 12*0.95=11.4 -> 11");
    check(scaledSize(14, 150) == 21, "scaledSize: 14*1.5=21");
    check(scaledSize(13, 80) == 10, "scaledSize: 13*0.8=10.4 -> 10");
    // 下限 1：极小基数 × 极小倍率不得塌成 0（0px = 文字隐形，且下游按 0 高做布局会连锁出错）
    check(scaledSize(1, 80) == 1, "scaledSize: never collapses to 0px");
    check(scaledSize(2, 80) == 2, "scaledSize: 2*0.8=1.6 -> 2");
    // 非正基数原样交回：本函数不猜「0 该变成几」，把判断留给调用方（0 在这里是「未设」不是「零像素」）
    check(scaledSize(0, 150) == 0, "scaledSize: base 0 passthrough (unset, not zero-pixel)");
    check(scaledSize(-1, 150) == -1, "scaledSize: negative base passthrough");

    // ---- ② 高 DPI 策略名 ----
    // 五个合法名（与 Qt 内建查找表逐项对齐），大小写混写与前后空格都要能命中
    check(policyRoundTrip(QStringLiteral("Round")) == "Round", "dpi: Round");
    check(policyRoundTrip(QStringLiteral("Ceil")) == "Ceil", "dpi: Ceil");
    check(policyRoundTrip(QStringLiteral("Floor")) == "Floor", "dpi: Floor");
    check(policyRoundTrip(QStringLiteral("RoundPreferFloor")) == "RoundPreferFloor", "dpi: RoundPreferFloor");
    check(policyRoundTrip(QStringLiteral("PassThrough")) == "PassThrough", "dpi: PassThrough");
    check(policyRoundTrip(QStringLiteral("roundpreferfloor")) == "RoundPreferFloor", "dpi: case-insensitive");
    check(policyRoundTrip(QStringLiteral("  Round  ")) == "Round", "dpi: trims spaces");
    // 未知/空/手改拼错 → PassThrough。这条是「不改观感」的承重墙：
    // PassThrough 正是 Qt6 内建默认（qguiapplication.cpp:142-143），所以写错值的人
    // 与没写过这个键的人看到同一套缩放，而不是被静默切到另一档。
    check(policyRoundTrip(QStringLiteral("")) == "PassThrough", "dpi: empty -> PassThrough");
    check(policyRoundTrip(QStringLiteral("roundPreferFlour")) == "PassThrough", "dpi: typo -> PassThrough");
    check(policyRoundTrip(QStringLiteral("2")) == "PassThrough", "dpi: numeric form is not accepted");
    // 枚举值直取（默认档一致性：Unset 有展示名，但解析器永不交出 Unset）
    check(dpiPolicyFromName(QStringLiteral("Round")) == Qt::HighDpiScaleFactorRoundingPolicy::Round,
          "dpi: enum identity for Round");
    check(dpiPolicyName(dpiPolicyFromName(QString())) == dpiPolicyName(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough),
          "dpi: unknown resolves to Qt6's own default");

    // ---- ④ QSS 字号缩放 ----
    // 100% 原样返回（同一内容，且必须一字不差——这是「不改配置即不改行为」的保证）
    {
        const QString src = QStringLiteral("#a { font-size: 14px; color: rgb(1, 2, 3); }");
        check(scaleQssFontSizes(src, 100) == src, "qss: 100% is identity");
    }
    // 标准形态
    check(scaleQssFontSizes(QStringLiteral("font-size: 12px"), 150) == QStringLiteral("font-size: 18px"),
          "qss: font-size scales");
    check(scaleQssFontSizes(QStringLiteral("font-size:12px;"), 200) == QStringLiteral("font-size:24px;"),
          "qss: no-space form scales");
    check(scaleQssFontSizes(QStringLiteral("font-size:  12 px"), 50 /*越界倍率仍按传入算*/) == QStringLiteral("font-size:  6 px"),
          "qss: spaces between number and px are matched");
    // 简写形态：`font: bold 12px "Consolas"` —— 第一个 px 字面量才是字号
    check(scaleQssFontSizes(QStringLiteral("font: bold 12px \"Segoe UI\";"), 150)
              == QStringLiteral("font: bold 18px \"Segoe UI\";"),
          "qss: font shorthand scales the size not the weight");
    // 权重不得被当成字号：font-weight 无 px 后缀，整条声明不该被动过
    check(scaleQssFontSizes(QStringLiteral("font-weight: 600;"), 150) == QStringLiteral("font-weight: 600;"),
          "qss: font-weight untouched");
    check(scaleQssFontSizes(QStringLiteral("font-size: 12px; font-weight: 600;"), 150)
              == QStringLiteral("font-size: 18px; font-weight: 600;"),
          "qss: only the size scales inside one block");
    // 注释里的同名串不得被当成声明（前一个非空白字符是 '/'）
    check(scaleQssFontSizes(QStringLiteral("/* font-size: 14px */"), 150) == QStringLiteral("/* font-size: 14px */"),
          "qss: comment is not rewritten");
    // 跨声明不得串台：终点是 ';'，第二条声明独立处理
    check(scaleQssFontSizes(QStringLiteral("a { font-size: 10px; color: rgb(0,0,0); } b { font-size: 20px; }"), 150)
              == QStringLiteral("a { font-size: 15px; color: rgb(0,0,0); } b { font-size: 30px; }"),
          "qss: two declarations both scale, value region stops at ';'");
    // 行高这类无 px 的数字被跳过，继续找同一条声明里真正的 px 字号
    check(scaleQssFontSizes(QStringLiteral("font: 1.5 12px serif;"), 150) == QStringLiteral("font: 1.5 18px serif;"),
          "qss: non-px leading skipped, px size still found");
    // 幂等性检查（重要：ThemeAware 每次主题切换都会重放 bind，若把已缩放的结果再缩放会累乘）
    {
        const QString one = scaleQssFontSizes(QStringLiteral("font-size: 12px"), 150);
        check(one == QStringLiteral("font-size: 18px"), "qss: first pass");
        // 缩放是在**原文**上做的（消费方每次都从文件重读），故重复调用同一输入恒等；
        // 这条断言钉的是「函数无状态」，不是「可以对结果再缩放」
        check(scaleQssFontSizes(one, 150) == QStringLiteral("font-size: 27px"), "qss: pure function, re-input re-scales");
    }
    // 真实形态：本仓 QSS 的多行块（含中文注释与 rgb）
    {
        const QString src = QStringLiteral(
            "/* 只读工作目录页眉：12px 次要灰字 */\n"
            "ChatSessionPage QLabel#workDirCaption {\n"
            "    color: rgb(104, 104, 104);\n"
            "}\n"
            "\n"
            "ChatSessionPage QLabel#emptyHintTitle {\n"
            "    color: rgb(72, 80, 90);\n"
            "    font-size: 14px;\n"
            "    font-weight: 600;\n"
            "}\n");
        const QString scaled = scaleQssFontSizes(src, 150);
        check(scaled.contains(QStringLiteral("font-size: 21px")), "qss: multiline block scales the size");
        check(scaled.contains(QStringLiteral("font-weight: 600")), "qss: multiline block keeps weight");
        check(scaled.contains(QStringLiteral("rgb(104, 104, 104)")), "qss: colors untouched");
        check(scaled.contains(QStringLiteral("12px 次要灰字")), "qss: comment text untouched");
    }

    // ---- ③ 窗口几何 ----
    const QByteArray sample = QByteArray::fromHex("00020001ffffffff000003c000000280");
    const QString encoded = encodeGeometry(sample);
    check(!encoded.isEmpty(), "geometry: encode yields non-empty");
    check(encoded == encoded.toLatin1(), "geometry: encoded form is pure latin1 (ini-safe)");
    check(decodeGeometry(encoded) == sample, "geometry: round-trip preserves bytes");
    // 空/缺失 → 空 QByteArray：调用方据此「不恢复」，保持默认尺寸（零回归）
    check(decodeGeometry(QString()).isEmpty(), "geometry: empty string -> empty bytes");
    check(decodeGeometry(QStringLiteral("   ")).isEmpty(), "geometry: whitespace -> empty bytes");
    // 手改截半/乱码：解出空或残串都不得当成「有效几何」——空串是唯一的失败信号
    check(decodeGeometry(QStringLiteral("!!!not base64!!!")).isEmpty()
              || decodeGeometry(QStringLiteral("!!!not base64!!!")).size() < sample.size(),
          "geometry: garbage does not decode to a full-size payload");
    // 截断的合法 base64：解出来必然短于原串，restoreGeometry 会因版本/长度校验拒收
    const QString truncated = encoded.left(encoded.size() / 2);
    check(decodeGeometry(truncated).size() < sample.size(), "geometry: truncated string yields short bytes");
    // 往返幂等：写→读→再写不得让串增长（几何键会被反复覆写，膨胀即磁盘垃圾）
    check(encodeGeometry(decodeGeometry(encoded)) == encoded, "geometry: encode∘decode is idempotent");

    return 0;
}
