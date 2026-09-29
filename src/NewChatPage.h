#pragma once

#include "BasePage.h"

class ChatMsgEdit;
class WorkDirPathBar;
class QLabel;
class QEvent;
class QShowEvent;

class NewChatPage : public BasePage
{
    Q_OBJECT
public:
    NewChatPage(QWidget *parent = nullptr);

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

    // i18n 第八轮：常驻页面，LanguageChange 时重译欢迎语（工作目录条文本由
    // WorkDirPathBar 自身的 changeEvent 处理，Qt 按控件逐个投递、父级无需转发）
    void changeEvent(QEvent *event) override;

    // 每次进入发起页重读 settings.ini 的默认工作目录，使设置页保存即时生效、无需重启
    void showEvent(QShowEvent *event) override;

private:
    // 读 settings.ini 默认工作目录并写入路径条（空/非法回退进程当前目录）；
    // 构造与 showEvent 共用
    void applyStoredWorkDir();

    ChatMsgEdit *m_chatMsgEdit = nullptr;
    QWidget *m_inputDock = nullptr;       // 工作目录路径条 + 输入框的同栏容器（英雄页视觉中心）
    WorkDirPathBar *m_workDirBar = nullptr; // 只读路径展示 + 浏览入口（省略/配色细节见组件注释）
    QLabel *m_welcomeLabel = nullptr;     // 英雄区问候语（重译面）
    bool m_workDirTouched = false;        // 用户经浏览钮临时改选后不再被外部配置覆盖本页面会话
};
