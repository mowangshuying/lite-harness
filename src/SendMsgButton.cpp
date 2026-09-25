#include "SendMsgButton.h"
#include "ThemeAware.h"
#include <FluUtils.h>
#include <QFile>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>

SendMsgButton::SendMsgButton(QWidget *parent) : QPushButton(parent)
{
    setFixedSize(30, 30);
    setIconSize(QSize(24, 24));
    // QSS 加载 + themeChanged 订阅收敛到 ThemeAware::bind；本组件特有行为
    // （SVG 图标按主题重着色）作为 extraRefresh 挂入，首刷与联动同一路径
    ThemeAware::bind("SendMsgButton.qss", this, [this] { updateIcon(); });
}

SendMsgButton::~SendMsgButton()
{
}

void SendMsgButton::updateIcon()
{
    setIcon(FluIconUtils::getFluentIcon(FluAwesomeType::Send, FluThemeUtils::getUtils()->getTheme()));
}
