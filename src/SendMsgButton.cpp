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
    // （图标按形态取字形）作为 extraRefresh 挂入；首刷全靠 bind 的 apply 无条件调它一次
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
    // 形态经动态属性喂 QSS 属性选择器（ToolTagKind/focused 同款惯例）：停止态实心红底
    // （三主题各一支，见 stylesheet/<theme>/SendMsgButton.qss 的 stopMode 段），传达"可中止运行中回合"；
    // 首帧不在此设属性（默认发送态无需选择器命中，免多余 polish）
    setProperty("stopMode", stop);
    style()->unpolish(this);
    style()->polish(this);
    updateIcon();
}

void SendMsgButton::updateIcon()
{
    // 两形态字形由 m_stopMode 单源切换；画笔恒白，不随主题前景色走——
    // FluTheme 重载在 light 下给 QColor(8,8,8) 深字，压停止态实心深红底 CR 仅 3.56（白字 5.63）
    setIcon(FluIconUtils::getFluentIcon(m_stopMode ? FluAwesomeType::Stop : FluAwesomeType::Send, Qt::white));
}
