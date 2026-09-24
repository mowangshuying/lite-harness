#include <QApplication>
#include <QCoreApplication>
// #include <FluFrameLessWidget.h>
#include "LiteHarness.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 全局设定组织/应用名：默认构造的 QSettings 即命中与旧显式双参构造
    // QSettings("LiteHarness","LiteHarness") 完全相同的注册表键，无迁移风险；
    // FluentUI 不设置这两个名称，不会互相覆盖
    QCoreApplication::setOrganizationName(QStringLiteral("LiteHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("LiteHarness"));

    LiteHarness liteharness;
    liteharness.show();
    
    return a.exec();
}