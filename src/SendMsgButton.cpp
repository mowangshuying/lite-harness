#include "SendMsgButton.h"
#include "ThemeAware.h"
#include <FluUtils.h>
#include <QFile>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

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

void SendMsgButton::setStopMode(bool stop)
{
    if (m_stopMode == stop)
        return;
    m_stopMode = stop;
    // 形态经动态属性喂 QSS 属性选择器（ToolTagKind/focused 同款惯例）：停止态钮环
    // 强调蓝淡染（与侧栏 running 灯同源色，琥珀专属留给审批），传达"可中止运行中回合"；
    // 首帧不在此设属性（默认发送态无需选择器命中，免多余 polish）
    setProperty("stopMode", stop);
    style()->unpolish(this);
    style()->polish(this);
    updateIcon();
}

void SendMsgButton::updateIcon()
{
    // 两形态字形单源：发送箭头 / 停止方块（Segoe 媒体停止 E71A），随主题前景色重着色；
    // 主题切换经 bind 的 extraRefresh 再入本函数，字形不滞后
    setIcon(FluIconUtils::getFluentIcon(m_stopMode ? FluAwesomeType::Stop : FluAwesomeType::Send,
                                        FluThemeUtils::getUtils()->getTheme()));
}
