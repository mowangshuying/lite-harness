#include <QApplication>
#include <FluFrameLessWidget.h>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    FluFrameLessWidget w;
    w.show();
    return a.exec();
}