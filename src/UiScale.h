#pragma once

// ============================================================================
// UiScale —— 显示尺度三件事的判据单源（纯头：无 IO、无 GUI、无 tr()）。
//
// 本仓一切会显示给用户看的数字都遵守「计算单源」纪律（结构篇 §8）：token 计量在
// CompactTokens.h / UsageTokens.h，输出速度在 StreamRate.h，行尾在 LineEnding.h。
// 显示尺度同样是判据而非样式：倍率越界、策略名拼错、几何串被手改截半，全都不会抛错，
// 只会静默把 UI 变成另一个样子——三条都是「跑得通但结果不对」的路径，只能断言钉死。
//
// 刻意不碰 QFont / QWidget：QFont 属 QtGui、QWidget 属 Widgets，而测试目标
// （CMakeLists.txt：target_link_libraries(lite-harness-tests PRIVATE Qt6::Core)）只链
// Core；Qt6 更把 QRegularExpression 挪进了 Core5Compat（测试目标不可用），故本头的
// QSS 扫描全部手写字符判定。接线留在消费方：
//   取整策略 + 应用字体 → App.cpp（必须在 QGuiApplication 构造前，见该处注释）
//   QSS 字号缩放       → ThemeAware::bind（全仓 QSS 加载的唯一收口点）
//   代码设定字号的组件 → 自己接：WorkDirPathBar.cpp 的 secondaryPixelSize()（P2-A3）
//                        凡在 C++ 里 setFont/setPixelSize 的字号**不经上面那个收口点**，
//                        必须自行调 scaledSize(base, runtimeFontScalePercent())；且必须读
//                        启动快照而非现取 fontScalePercent()，否则与①②两头脱钩成半生效。
//                        新增此类组件时的检查项：本行是否登记了它。
//   save/restoreGeometry → LiteHarness.cpp（QWidget 契约）
// 与 StreamRate.h 同口径：本头证判据，接线靠编译期等价 + 冒烟运行。
//
// 键语义（README 设置表 / AGENTS 键清单同步）：
//   uiFontScale    整数百分比，[80,200]，默认 100（= 不缩放）。重启生效——应用字体
//                  只在启动时取一次，与 language 键同语义，故设置页沿用「重启」提示。
//   dpiRounding    策略名串，大小写不敏感；空/未知一律 PassThrough（= Qt6 内建默认，
//                  即「不改行为」）。必须在 QGuiApplication 构造前落定：
//                  qguiapplication.cpp:4009 实证构造后调用只 qWarning 且不生效。
//   windowGeometry 主窗口 saveGeometry() 的 base64 形态。空/缺失 = 无存档，消费方
//                  保持默认尺寸（零回归），绝不猜一个尺寸。
// ============================================================================

#include <QtCore/qnamespace.h> // Qt::HighDpiScaleFactorRoundingPolicy
#include <QByteArray>
#include <QString>
#include <QVariant>

namespace UiScale {

// ---- 键名单源（读写两侧共用，杜绝两处拼法漂移）----
inline const QString kFontScaleKey = QStringLiteral("uiFontScale");
inline const QString kDpiRoundingKey = QStringLiteral("dpiRounding");
inline const QString kWindowGeometryKey = QStringLiteral("windowGeometry");

// ---- ① 字号倍率 ----
constexpr int kFontScaleDefault = 100; // 未设置/非法：不缩放
constexpr int kFontScaleMin = 80;      // 下界：再小就低于正文可读底线
constexpr int kFontScaleMax = 200;     // 上界：2 倍字号会把固定宽卡片撑破

// 倍率校验（与 AgentConst::maxRetriesValue 同纪律：缺失、非整数、越界一律回退默认）。
// 刻意用 toLongLong 而非 toInt：config.json 是手改面，写成 1e9 之类时 toInt 溢出成
// 负数再被下界拦掉，语义上仍是「回退默认」，但日志里看不出用户到底写了什么。
inline int fontScalePercent(const QVariant &stored)
{
    bool ok = false;
    const qlonglong value = stored.toLongLong(&ok);
    if (!ok || value < kFontScaleMin || value > kFontScaleMax)
        return kFontScaleDefault;
    return static_cast<int>(value);
}

// 折算因子：点字号用这条（9pt × 1.2 = 10.8pt）——点制数量子粗，整数量子化会把
// 「放大 20%」变成「+2pt ≈ +22%」，倍率与观感不成比例。
// 启动快照（进程级唯一值，App.cpp 在 QApplication 构造后写一次，消费方只读这里）：
// 字号有**两个生效面**——应用字体（气泡正文/输入框，QSS 不写死字号）只在启动时设一次，
// QSS 字号（卡片次要文字）则在每次主题切换时由 ThemeAware::bind 重放。若后者现取配置文件，
// 用户手改配置后切一次主题就会出现「卡片字变了、正文没变」的半生效，比完全没改更难解释。
// 快照把两面钉在同一个值上，代价是本键与 language 同语义：**重启生效**（设置页据此提示）。
// 函数内 static + inline 取引用，跨 TU 唯一实例（同 TestHarness / AppSettings 计数口径）。
inline int &runtimeFontScale()
{
    static int percent = kFontScaleDefault;
    return percent;
}

// 写侧只此一处：入参先过一遍校验，于是「越界的启动值」与「没配」在快照里同为 100，
// 消费方无需再各自兜底（各自兜底就会各自漂移）。
inline void setRuntimeFontScale(int percent)
{
    runtimeFontScale() = fontScalePercent(QVariant(percent));
}

inline int runtimeFontScalePercent()
{
    return runtimeFontScale();
}

inline qreal scaleFactor(int percent)
{
    return qreal(percent) / 100.0;
}

// 整数量子（QSS 里的 px 字号）：四舍五入而非截断——12px×1.05=12.6，截断回 12 等于
// 「调了没反应」，用户会连按几次放大。下限 1：绝不出 0px 隐形文字。
// 非正基数原样交回：这里的 0 是「未设」而不是「零像素」，本函数不替调用方猜该变几。
inline int scaledSize(int baseSize, int percent)
{
    if (baseSize <= 0 || percent <= 0)
        return baseSize;
    const int scaled = (baseSize * percent + 50) / 100;
    return scaled > 0 ? scaled : 1;
}

// ---- ② 高 DPI 取整策略 ----
// 名称表与 Qt 内建查找表逐项对齐（qtbase/src/gui/kernel/qhighdpiscaling.cpp 的
// scaleFactorRoundingPolicyLookup：Round / Ceil / Floor / RoundPreferFloor /
// PassThrough）。比的是 toLower 后的串，故 "roundpreferfloor" 与 "RoundPreferFloor"
// 同解——配置文件是手改面，大小写不该成为失败模式（键名侧则相反：json 后端大小写敏感，
// 见 AppSettings::keyCaseSensitivity()；这里放宽的是**值**，别把两条口径混为一谈）。
// 空串与未知值都回退 PassThrough：那是 Qt6 自己的默认值（qguiapplication.cpp:142-143
// 实证），所以「没写过这个键」与「写错了值」看到同一套缩放，不会被静默切到另一档。
inline Qt::HighDpiScaleFactorRoundingPolicy dpiPolicyFromName(const QString &name)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("round"))
        return Qt::HighDpiScaleFactorRoundingPolicy::Round;
    if (key == QLatin1String("ceil"))
        return Qt::HighDpiScaleFactorRoundingPolicy::Ceil;
    if (key == QLatin1String("floor"))
        return Qt::HighDpiScaleFactorRoundingPolicy::Floor;
    if (key == QLatin1String("roundpreferfloor"))
        return Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor;
    return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
}

// 展示名（启动日志用）：把生效的枚举还原成 Qt 文档里的写法，于是「盘上写的串」与
// 「实际生效的策略」能并排落在日志里，而不是把错值静默吃掉。
inline QString dpiPolicyName(Qt::HighDpiScaleFactorRoundingPolicy policy)
{
    using Policy = Qt::HighDpiScaleFactorRoundingPolicy;
    switch (policy)
    {
    case Policy::Round:            return QStringLiteral("Round");
    case Policy::Ceil:             return QStringLiteral("Ceil");
    case Policy::Floor:            return QStringLiteral("Floor");
    case Policy::RoundPreferFloor: return QStringLiteral("RoundPreferFloor");
    case Policy::PassThrough:      return QStringLiteral("PassThrough");
    case Policy::Unset:            return QStringLiteral("Unset");
    }
    return QStringLiteral("PassThrough"); // 枚举日后新增时的兜底，与默认档一致
}

// ---- ③ 窗口几何 ----
// saveGeometry() 交出的是不透明二进制（版本号 + 屏幕序号 + 矩形），直接写进配置文件会产出
// 乱码值（ini 后端还会连带打乱下一行的解析），故整串 base64 化。
inline QString encodeGeometry(const QByteArray &geometry)
{
    return QString::fromLatin1(geometry.toBase64());
}

// 解出空 QByteArray 的两种含义（串缺失 / 串全非法）在此合并成一条：调用方「拿到空就
// 不恢复」即可，不必再区分——两种情况下该做的动作完全相同。
// 真正的格式校验不在这里做：QWidget::restoreGeometry 自带版本与屏幕合法性检查
// （qwidget.cpp 实证：屏幕序号越界回落 0、宽屏比超 1.5 直接 return false），本头只
// 保证「交出去的字节串要么来自一次真实 saveGeometry，要么为空」。
inline QByteArray decodeGeometry(const QString &stored)
{
    const QString trimmed = stored.trimmed();
    if (trimmed.isEmpty())
        return QByteArray();
    return QByteArray::fromBase64(trimmed.toLatin1());
}

// ---- ④ QSS 字号缩放（ThemeAware::bind 的唯一改写点）----
// 为什么改 QSS 文本而不是 setStyleSheet 之外另设一层：本仓三套主题共 87 条 font-size
// 声明（含 `font: 12px "Consolas"` 简写形态），把它们全部抽成变量要动 261 处 ×3 主题
// 且每处 bind 都得传参；而 ThemeAware::bind 已经是「全仓 QSS 主题化」的收口点
//（21 个调用点），在唯一收口点上做一次文本缩放，比在每处铺参数更单源。
//
// 改写规则（保守到宁可漏改也不改坏）：
//   · 只认 `font-size` 与 `font:` 两种属性名，且属性名后必须紧跟冒号（允许空白）；
//   · 只认「声明位」上的属性：跳过空白后前一个字符须是 '{' ';' '}' 或位于串首——
//     于是注释里的 `/* font-size: 14px */`（前一个非空白字符是 '/'）不会被当成声明；
//   · 值区只取**第一个**「数字串 + 可选空白 + px」，简写形态（font: bold 12px "Consolas"）
//     的字号正是第一个 px 字面量；font-weight:600 / line-height:1.5 这类无 px 的数字
//     会被跳过（见 findPixelLiteral），故不会把权重当字号放大；
//   · 值区终点是 ';' 或 '}'，绝不跨声明；
//   · 倍率为 100 时原样返回（同一对象，零开销、零风险）。
// 刻意不改 FluentUI 自带 QSS：那是第三方样式表（3rdparty/ 惯例不改），本头只作用于
// stylesheet/<theme>/ 下我方文件——于是导航项/按钮字号恒按 FluentUI 原值，属已知边界。
inline int findPixelLiteral(const QString &text, int from, int end)
{
    int i = from;
    while (i < end)
    {
        if (!text.at(i).isDigit())
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < end && text.at(j).isDigit())
            ++j;
        int k = j;
        while (k < end && (text.at(k) == QLatin1Char(' ') || text.at(k) == QLatin1Char('\t')))
            ++k;
        if (k + 1 < end && text.at(k) == QLatin1Char('p') && text.at(k + 1) == QLatin1Char('x'))
            return i; // 命中「数字 + px」，返回数字串起始
        i = (j > i) ? j : i + 1; // 非 px 数字（权重/行高等）整段跳过
    }
    return -1;
}

inline QString scaleQssFontSizes(const QString &qss, int percent)
{
    if (percent == kFontScaleDefault || percent <= 0 || qss.isEmpty())
        return qss;

    const QString lower = qss.toLower(); // 索引与原文一一对应，仅用于属性名匹配
    QString out;
    out.reserve(qss.size() + 16);
    int cursor = 0; // 已拷进 out 的原文位置
    int pos = 0;    // 属性名扫描位

    while (pos < qss.size())
    {
        const int sizeAt = lower.indexOf(QLatin1String("font-size"), pos);
        const int shortAt = lower.indexOf(QLatin1String("font:"), pos);
        int at = -1;
        bool isSizeProp = false;
        if (sizeAt >= 0 && (shortAt < 0 || sizeAt <= shortAt))
        {
            at = sizeAt;
            isSizeProp = true;
        }
        else if (shortAt >= 0)
        {
            at = shortAt;
        }
        if (at < 0)
            break;

        // 属性名本体长度（**不含冒号**）："font-size"=9、"font"=4。
        // 匹配串用的是 "font:"（含冒号，长度 5），若这里也写 5 就正好跳过冒号、
        // 把 nameEnd 落到冒号后的空白上，下方「冒号必须紧跟属性名」的判据必然失败——
        // 简写形态于是整片静默不缩放。两条断言（font: bold 12px / font: 1.5 12px）钉住这点。
        const int nameEnd = at + (isSizeProp ? 9 : 4);
        int colon = nameEnd;
        while (colon < qss.size() && (qss.at(colon) == QLatin1Char(' ') || qss.at(colon) == QLatin1Char('\t')))
            ++colon;
        const bool nameOk = colon < qss.size() && qss.at(colon) == QLatin1Char(':');

        int prev = at - 1;
        while (prev >= 0 && (qss.at(prev) == QLatin1Char(' ') || qss.at(prev) == QLatin1Char('\t')
                             || qss.at(prev) == QLatin1Char('\n') || qss.at(prev) == QLatin1Char('\r')))
            --prev;
        const bool posOk = prev < 0 || qss.at(prev) == QLatin1Char('{') || qss.at(prev) == QLatin1Char(';')
                           || qss.at(prev) == QLatin1Char('}');

        int end = qss.size();
        const int semi = qss.indexOf(QLatin1Char(';'), nameEnd);
        const int brace = qss.indexOf(QLatin1Char('}'), nameEnd);
        if (semi >= 0)
            end = semi;
        if (brace >= 0 && brace < end)
            end = brace;

        const int digits = (nameOk && posOk) ? findPixelLiteral(qss, colon + 1, end) : -1;
        if (digits < 0)
        {
            pos = at + 1; // 这处不是可改写的字号声明，继续往后找
            continue;
        }

        int digitsEnd = digits;
        while (digitsEnd < qss.size() && qss.at(digitsEnd).isDigit())
            ++digitsEnd;
        const int base = qss.mid(digits, digitsEnd - digits).toInt();
        out += qss.mid(cursor, digits - cursor);
        out += QString::number(scaledSize(base, percent));
        cursor = digitsEnd;
        pos = digitsEnd;
    }

    out += qss.mid(cursor);
    return out;
}

} // namespace UiScale
