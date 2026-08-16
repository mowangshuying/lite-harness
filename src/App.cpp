#include <QApplication>
// #include <FluFrameLessWidget.h>
#include "LiteHarness.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    
    LiteHarness liteharness;
    liteharness.show();
    
    return a.exec();
}