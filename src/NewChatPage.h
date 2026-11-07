#pragma once

#include "BasePage.h"

#include <QDate>
#include <QHash>

#include "ActivityLedger.h" // 热力图数据形态：QHash<QDate, DayTotal>

class ChatMsgEdit;
class WorkDirPathBar;
class ContributionHeatmap;
class QLabel;
class QEvent;
class QShowEvent;
class QHBoxLayout;

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

    // 会话活动日账（合并跨会话后）push 入口——宿主 LiteHarness 在启动扫描/回合增量/删除
    // 会话三处调用。本页面从不直读磁盘、从不订阅 AgentLoop（承 SessionSidebar setter-push 纪律）。
    void setActivityData(const QHash<QDate, ActivityLedger::DayTotal> &dayMap);

signals:
    void newChatRequested(const QString &text);

protected:
    // 维护输入区栏宽（maximumWidth(min(800, 可用宽)) + 行容器手动居中边距；
    // 只钳上限不钳下限，配合 Ignored 行容器断开窗口收缩棘轮）
    void resizeEvent(QResizeEvent *event) override;

    // i18n 第八轮：常驻页面，LanguageChange 时重译欢迎语（工作目录条文本由
    // WorkDirPathBar 自身的 changeEvent 处理，Qt 按控件逐个投递、父级无需转发）
    void changeEvent(QEvent *event) override;

    // 每次进入发起页重读 config.json 的默认工作目录，使设置页保存即时生效、无需重启
    void showEvent(QShowEvent *event) override;

private:
    // 读 config.json 默认工作目录并写入路径条（空/非法回退进程当前目录）；
    // 构造与 showEvent 共用
    void applyStoredWorkDir();

    ChatMsgEdit *m_chatMsgEdit = nullptr;
    // 输入栏的居中行容器布局：栏宽靠 maximumWidth 钳制，两侧留白由 resizeEvent
    // 手动写入边距（水平 Ignored 行容器，断开窗口收缩棘轮，与会话页同款机制）
    QHBoxLayout *m_dockRowLayout = nullptr;
    QWidget *m_inputDock = nullptr;       // 工作目录路径条 + 输入框的同栏容器（英雄页视觉中心）
    WorkDirPathBar *m_workDirBar = nullptr; // 只读路径展示 + 浏览入口（省略/配色细节见组件注释）
    QLabel *m_welcomeLabel = nullptr;     // 英雄区问候语（重译面）
    QLabel *m_subtitleLabel = nullptr;    // 英雄区副标题（问候语下方一行次要灰字，参考图两行构图）
    bool m_workDirTouched = false;        // 用户经浏览钮临时改选后不再被外部配置覆盖本页面会话
    // 会话活动热力图（跨会话合并后由宿主 push 数据）；本页面只创建/布局，从不读写磁盘
    ContributionHeatmap *m_heatmap = nullptr;
};
