#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <FluLogUtils.h>
// #include <FluFrameLessWidget.h>
#include "AppSettings.h"
#include "I18n.h"
#include "LiteHarness.h"
#include "UiScale.h"

int main(int argc, char *argv[])
{
    // 高 DPI 取整策略（P1-11）：**必须在 QApplication 构造前**落定——Qt 实证构造后
    // 调用只 qWarning 且对已算好的缩放毫无作用（qguiapplication.cpp:4009），
    // 故这里不能走 AppSettings::value()（其 reader() 的 Q_ASSERT 要求实例已存在），
    // 改走同头的 bootstrapValue（exe 目录从 argv[0] 反推，同源不同路）。
    // 缺省档 = PassThrough，正是 Qt6 自己的默认值（qguiapplication.cpp:142-143），
    // 所以「没配过 ini」与「配了个错值」都与改动前完全一致：本行的价值是把
    // 「依赖 Qt 默认」变成「本仓显式声明」，并给 125%/150% 分数缩放留一个不改代码
    // 就能切档的出口（ini 写 Round / RoundPreferFloor / Ceil / Floor）。
    // 第三参 fallback 给空 QVariant：toString() 得空串，dpiPolicyFromName 的空串分支
    // 正是 PassThrough——与「键从未写过」同一路，缺省语义不因引导通道而漂移。
    QApplication::setHighDpiScaleFactorRoundingPolicy(UiScale::dpiPolicyFromName(
        AppSettings::bootstrapValue(argc > 0 ? argv[0] : nullptr, UiScale::kDpiRoundingKey,
                                    QVariant())
            .toString()));

    QApplication a(argc, argv);

    // 字号倍率（P1-11）：settings.ini 键 uiFontScale（整数百分比，[80,200]，缺省 100）。
    // 走 QApplication::setFont 而非逐控件设字体——本仓 QSS 里正文面（#msgBrowser 气泡
    // 与输入框 #textEdit）**刻意不写 font-size**，字号由应用字体单源决定，故这一行
    // 就覆盖到用户真正要读的内容；反之 QSS 里显式写了 font-size 的次要文字（卡片
    // 标题/徽标/状态条等 87 处）**不随本键缩放**，属已知边界：那些值与固定行高
    // （kHeaderHeight=32 等）成对设计，单独放大字号会把卡片撑破。
    // 生效时机 = 启动（改后需重启，与 language 键同语义，设置页据此给重启提示）。
    const int fontScalePercent = UiScale::fontScalePercent(AppSettings::value(UiScale::kFontScaleKey));
    UiScale::setRuntimeFontScale(fontScalePercent); // 快照：ThemeAware 重放 QSS 时读这里，两面同值
    if (fontScalePercent != UiScale::kFontScaleDefault)
    {
        QFont appFont = a.font();
        // 点制优先（Windows 默认应用字体是点制）：点数量子粗，必须按实数折算，
        // 否则 9pt × 110% 会被截回 9pt，用户按了没反应。像素形态才走整数量子。
        if (appFont.pointSizeF() > 0.0)
            appFont.setPointSizeF(appFont.pointSizeF() * UiScale::scaleFactor(fontScalePercent));
        else
            appFont.setPixelSize(UiScale::scaledSize(appFont.pixelSize(), fontScalePercent));
        a.setFont(appFont);
    }

    // 日志单源：FluentUI Utils 的 FluLogUtils 接管 qInstallMessageHandler，
    // 全仓既有 qDebug/qWarning（101 处，24 文件）自动落文件，调用点不迁 LOG_* 宏
    // （宏自带时间/文件/函数/行号前缀，与 handler 的落盘格式重复，二选一即可）。
    // 主程序已是 GUI 子系统（无 stderr 宿主），不接管日志则调试输出全部蒸发。
    // __init 内部路径写死 "../log/<时间戳>.log"（相对 CWD，即 exe 目录的上一级），
    // 而 QFile::open 不建目录：目录缺失 → open 失败 → 它连 handler 都不装、静默全丢。
    // 故先按同一路径 mkpath 兜底；不改第三方源码（仓内惯例）。
    QDir().mkpath(QStringLiteral("../log"));
    FluLogUtils::__init();

    // 版本号进运行时：LITE_VERSION 宏由 CMake target_compile_definitions 注入
    // （= project VERSION 拼 s 前缀，唯一真源，如 "s17"），供设置页等展示点取用
    QCoreApplication::setApplicationVersion(QStringLiteral(LITE_VERSION));

    // i18n 第八轮：translator 须在任何窗口构造前装载，控件构造期 tr() 即取目标
    // 语言文案（FluentUI 控件无运行中重译能力，语言切换以重启生效）
    I18n::applyLanguage(I18n::language());

    LiteHarness liteharness;
    liteharness.show();

    const int rc = a.exec();
    // rc==931：语言切换自重启（gallery 惯例，走 qApp->exit(931) 而非 std::exit，
    // 故 exec() 正常返回、本行必达）。新进程已由 I18n::requestRestart
    // startDetached 拉起，此处只透传返回值，严禁再次 startDetached（双启缺陷）。
    // __deinit 必须在此：QFile 关闭才 flush，缺它则末段日志留在缓冲里丢。
    FluLogUtils::__deinit();
    return rc;
}