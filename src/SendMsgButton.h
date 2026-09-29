#pragma once

#include <QPushButton>

class SendMsgButton : public QPushButton
{
    Q_OBJECT
public:
    SendMsgButton(QWidget *parent = nullptr);
    ~SendMsgButton();

    /// 两形态单源切换：false=发送箭头（默认，NewChatPage 恒此态零变化），
    /// true=停止方块（会话页回合运行中由 ChatMsgEdit 注入）。形态经动态属性
    /// 喂 QSS 选择器做钮环染色，图标随属性在 updateIcon 换字形
    void setStopMode(bool stop);

protected:
    /// 按当前形态与主题重着色圆形图标（组件特有行为；QSS 样板由
    /// ThemeAware::bind 吸收，本方法经其 extraRefresh 联动，主题切换字形不滞后）
    void updateIcon();

    bool m_stopMode = false; // 停止形态标记（默认发送，构造期不触属性避免多余 polish）
};
