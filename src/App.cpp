#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <FluLogUtils.h>
// #include <FluFrameLessWidget.h>
#include "I18n.h"
#include "LiteHarness.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

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
    // （= project VERSION 拼 s 前缀，唯一真源，如 "s13.1"），供设置页等展示点取用
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