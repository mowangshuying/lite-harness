#pragma once

#include <QPushButton>

class SendMsgButton : public QPushButton
{
    Q_OBJECT
public:
    SendMsgButton(QWidget *parent = nullptr);
    ~SendMsgButton();

protected:
    /// 按当前主题重着色圆形 SVG 图标（组件特有行为；QSS 样板由
    /// ThemeAware::bind 吸收，本方法经其 extraRefresh 联动）
    void updateIcon();
};
