#pragma once

#include <FLuFrameLessWidget.h>
#include <FluStackedLayout.h>
#include <FluVNavigationView.h>
#include "SessionRegistry.h"

class NewChatPage;

class LiteHarness : public FluFrameLessWidget
{
    Q_OBJECT
public:
    LiteHarness(QWidget *parent = nullptr);

    // 原名 __initUI/__connect 等双下划线标识符为 C++ 标准保留（任意标识符均不得含 __），重命名为小驼峰；
    // setupConnections 而非 connect 以避免与 QObject::connect 静态成员同名歧义
    void initUi();
    void initNavView();

    void setupConnections();

/// slots;
    void onThemeChanged();
private:
    void createSession(const QString &text);
    // 启动恢复：从 index.json 读取历史会话，按创建时间升序重建会话页/导航项/堆叠布局，
    // 不切换当前页（保持停留在 NewChatPage）
    void restoreSessions();

    // 会话重命名/删除（右键导航子项触发的上下文菜单动作；key 为导航/堆叠页键 Session_*）
    void showSessionMenu(QWidget *childItem, const QPoint &globalPos);
    void renameSession(const QString &key);
    void deleteSession(const QString &key);

    // 递归给会话子项及其全部后代控件装右键过滤器并登记（行区域被子控件占满，
    // Qt 仅投递给最深接收者，装在主体上收不到事件）
    void hookContextMenu(QWidget *childItem);
    // 沿 parentWidget 链向上解析首个登记在册的会话子项本体；未命中返回 nullptr
    FluVNavigationIconTextItem *resolveSessionItem(QWidget *w) const;

protected:
    // 拦截 Sessions 分组子项的右键：按下即弹菜单并吞掉，释放一并吞掉，
    // 规避 FluVNavigationIconTextItem::mouseReleaseEvent 不辨按键即发 itemClicked 的翻页 bug
    bool eventFilter(QObject *watched, QEvent *event) override;

    // 退出守卫：仍有会话回合在运行时先确认（详见 cpp 定义处注释）
    void closeEvent(QCloseEvent *event) override;

protected:
    // 构造体内必然先于任何使用完成赋值，显式置空仅为防御：与下方页面指针成员统一初值纪律，
    // 避免万一早退/异常路径留下未定义指针
    FluStackedLayout *m_sLayout = nullptr;
    FluVNavigationView *m_navView = nullptr;
    NewChatPage *m_newChatPage = nullptr;
    // 会话映射数据（key↔页面/数据ID、导航子项、右键受控控件）全部收进注册表；
    // 主窗口不再直持任何会话表，所有权与删除时序不变（注册表只持观察指针）
    SessionRegistry m_registry;
};
