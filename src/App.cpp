#include <QApplication>
#include <QCoreApplication>
// #include <FluFrameLessWidget.h>
#include "I18n.h"
#include "LiteHarness.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 版本号进运行时：LITE_VERSION 宏由 CMake target_compile_definitions 注入
    // （= project VERSION，唯一真源），供设置页等展示点取用
    QCoreApplication::setApplicationVersion(QStringLiteral(LITE_VERSION));

    // i18n 第八轮：translator 须在任何窗口构造前装载，控件构造期 tr() 即取目标
    // 语言文案（FluentUI 控件无运行中重译能力，语言切换以重启生效）
    I18n::applyLanguage(I18n::language());

    LiteHarness liteharness;
    liteharness.show();

    const int rc = a.exec();
    // rc==931：语言切换自重启（gallery 惯例）。新进程已由 I18n::requestRestart
    // startDetached 拉起，此处只透传返回值，严禁再次 startDetached（双启缺陷）。
    return rc;
}