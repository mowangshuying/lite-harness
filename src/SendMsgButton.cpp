#include "SendMsgButton.h"
#include "LayoutConstants.h"
#include "ThemeAware.h"
#include <FluUtils.h>
#include <QFile>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

SendMsgButton::SendMsgButton(QWidget *parent) : QPushButton(parent)
{
    setFixedSize(LayoutConst::kSendBtnSize, LayoutConst::kSendBtnSize);
    setIconSize(QSize(24, 24));
    // 无障碍与可发现性（P1-B）：本钮是纯图标件（字形随 setStopMode 变），读屏器此前
    // 拿不到任何名称；tooltip 原先只在 ChatMsgEdit::applyBusyState 里设，而首次
    // setTurnBusy(false) 因「同值早退」不会走到那里 —— 回合开始前那段最需要发现入口
    // 的时间里，钮上连 tooltip 都没有。名称与提示从此由本类按形态单源维护。
    setToolTip(tr("发送"));
    setAccessibleName(tr("发送"));
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
    // 形态名与提示随形态更新：无障碍名称与 tooltip 同一处维护，避免两处各改一半
    // （ChatMsgEdit::applyBusyState 曾单独设 tooltip，首次同值早退时漏设）
    setToolTip(stop ? tr("停止") : tr("发送"));
    setAccessibleName(stop ? tr("停止") : tr("发送"));
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
