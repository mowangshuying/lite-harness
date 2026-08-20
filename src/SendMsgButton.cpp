#include "SendMsgButton.h"
#include <FluUtils.h>
#include <QFile>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>

SendMsgButton::SendMsgButton(QWidget *parent) : QPushButton(parent)
{
    setFixedSize(30, 30);
    setIconSize(QSize(24, 24));
    setIcon(FluIconUtils::getFluentIcon(FluAwesomeType::Send, FluThemeUtils::getUtils()->getTheme()));
    onThemeChanged();
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, &SendMsgButton::onThemeChanged);
}

SendMsgButton::~SendMsgButton()
{
}

void SendMsgButton::onThemeChanged()
{
    setIcon(FluIconUtils::getFluentIcon(FluAwesomeType::Send, FluThemeUtils::getUtils()->getTheme()));
    FluStyleSheetUtils::setQssByFileName("SendMsgButton.qss", this, FluThemeUtils::getUtils()->getTheme());
}
