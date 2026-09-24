#pragma once

#include "BasePage.h"

class ChatMsgEdit;
class QLabel;

class NewChatPage : public BasePage
{
    Q_OBJECT
public:
    NewChatPage(QWidget *parent = nullptr);

    void onThemeChanged() override;

    // 输入区当前选中的模型（宿主创建会话时读取，注入新会话继承）
    QString currentModel() const;

    // 输入区当前选中的工作目录（宿主创建会话时读取，注入新会话）；
    // 初值取设置页默认目录（存在且为目录时），否则进程当前目录
    QString currentWorkDir() const;

signals:
    void newChatRequested(const QString &text);

protected:
    // 维护输入区栏宽（min(800, 可用宽) 并水平居中，纯布局 stretch 无法表达该语义）
    void resizeEvent(QResizeEvent *event) override;
    // 路径 label 尺寸变化时按新宽度重新中间省略
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateWorkDirDisplay(); // ToolTip 始终为全路径，可见文本按当前宽度中间省略

    ChatMsgEdit *m_chatMsgEdit = nullptr;
    QWidget *m_inputDock = nullptr;     // 工作目录路径条 + 输入框的同栏容器（英雄页视觉中心）
    QLabel *m_workDirLabel = nullptr;   // 工作目录路径显示（objectName workDirPath，配色见 QSS）
    QString m_workDir;                  // 当前选中的工作目录绝对路径（省略前原值）
};
