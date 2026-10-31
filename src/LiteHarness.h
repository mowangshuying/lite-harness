#pragma once

#include <FLuFrameLessWidget.h>
#include <FluStackedLayout.h>
#include <FluVNavigationView.h>
#include "SessionRegistry.h"

class NewChatPage;
class NavItem;
class FluVNavigationSettingsItem;
class FluVNavigationIconTextItem;

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
    // 启动恢复：先把旧 <config>/index.json 一次性搬进 config.json 键 app/sessions，再从该键读取
    // 历史会话，按创建时间升序重建会话页/导航项/堆叠布局，
    // 不切换当前页（保持停留在 NewChatPage）
    void restoreSessions();

    // 会话重命名/删除（右键导航子项触发的上下文菜单动作；key 为导航/堆叠页键 Session_*）
    void showSessionMenu(QWidget *childItem, const QPoint &globalPos);
    void renameSession(const QString &key);
    void deleteSession(const QString &key);

    // 会话页 → 导航子项的运行态联动（评审 B3 遗留接线）：navRunningChanged 自实现起
    // 只 emit 无连接，回合运行与否在导航上完全不可见。此处补订阅并落到图标位。
    void hookNavBadge(const QString &key, ChatSessionPage *page);
    // 运行态徽标：只改子项图标字形，绝不写 label 文本——label 是搜索的匹配面
    // （FluVNavigationView::getItemByText 按全文精确比对）兼重命名的旧标题来源，
    // 一旦掺入徽标字符即同时破坏两者。
    void applyNavBadge(const QString &key, bool running);
    // 主题切换后重放全部徽标：FluVNavigationIconTextItem::onThemeChanged 会按
    // m_awesomeType 无条件重设图标，把运行态字形冲回 Message，故须在其之后覆写。
    void refreshNavBadges();
    // 搜索候选喂入通路已删（s17.1 按用户裁决关闭导航搜索项）。原注释留档备查：
    // FluVNavigationView::updateSearchKeys 在 FluentUI 内部无任何调用点，候选恒空即搜索
    // 框恒不命中，故当年恢复搜索项时必须自建本方法喂入（a78722d 的 P0-5）。撤搜索项时
    // 若只藏本体而留这条喂入，就是每次增删改会话白跑一遍全等候选重建。

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

    // 窗口尺寸/位置恢复（P1-11）：构造末尾、show() 之前调用；无存档或存档非法即保持默认。
    // 落盘在 closeEvent 的 isAccepted() 分支，两侧不对称分工见 cpp 注释。
    void restoreWindowGeometry();

    // i18n 第八轮：QTranslator 重装触发 Qt 向全 widget 树广播 LanguageChange，
    // 常驻导航三项文本在此重取 tr()（重启后启动即装好 translator，本钩子主要兜
    // 未来"免重启切换"演进；当前链路重启动作在 SettingsPage 发起）
    void changeEvent(QEvent *event) override;

private:
    void retranslateUi();

    // 导航列/浮动滚动条底色对齐：把覆盖块追加进 FluentUI 控件「自身样式表」尾部。
    // 窗口级 QSS 复合选择器压不过控件自身表（近表优先，9e7fcf2 像素实证），
    // 详见 LiteHarness.cpp 定义处与 stylesheet/<theme>/LiteHarnessNavAlign.qss 注释。
    void applyNavAlignOverrides();

protected:
    // 构造体内必然先于任何使用完成赋值，显式置空仅为防御：与下方页面指针成员统一初值纪律，
    // 避免万一早退/异常路径留下未定义指针
    FluStackedLayout *m_sLayout = nullptr;
    FluVNavigationView *m_navView = nullptr;
    NewChatPage *m_newChatPage = nullptr;
    // 导航三项常驻引用（i18n 第八轮）：LanguageChange 时经 getLabel()->setText 重译
    FluVNavigationIconTextItem *m_newChatItem = nullptr;
    NavItem *m_sessionsItem = nullptr;
    FluVNavigationSettingsItem *m_settingsItem = nullptr;
    // 会话映射数据（key↔页面/数据ID、导航子项、右键受控控件）全部收进注册表；
    // 主窗口不再直持任何会话表，所有权与删除时序不变（注册表只持观察指针）
    SessionRegistry m_registry;
};
