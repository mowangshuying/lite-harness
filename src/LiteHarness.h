#pragma once

#include <FLuFrameLessWidget.h>
#include <FluStackedLayout.h>
#include <FluVNavigationView.h>
#include <QHash>

class NewChatPage;
class ChatSessionPage;

class LiteHarness : public FluFrameLessWidget
{
    Q_OBJECT
public:
    LiteHarness(QWidget *parent = nullptr);

    void __initUI();
    void __initNavView();

    void __connect();

/// slots;
    void onThemeChanged();
private:
    void __createSession(const QString &text);
    // 启动恢复：从 index.json 读取历史会话，按创建时间升序重建会话页/导航项/堆叠布局，
    // 不切换当前页（保持停留在 NewChatPage）
    void __restoreSessions();

    // 会话重命名/删除（右键导航子项触发的上下文菜单动作；key 为导航/堆叠页键 Session_*）
    void __showSessionMenu(QWidget *childItem, const QPoint &globalPos);
    void __renameSession(const QString &key);
    void __deleteSession(const QString &key);

    // 递归给会话子项及其全部后代控件装右键过滤器并登记（行区域被子控件占满，
    // Qt 仅投递给最深接收者，装在主体上收不到事件）
    void __hookContextMenu(QWidget *childItem);
    // 沿 parentWidget 链向上解析首个登记在册的会话子项本体；未命中返回 nullptr
    FluVNavigationIconTextItem *__resolveSessionItem(QWidget *w) const;

protected:
    // 拦截 Sessions 分组子项的右键：按下即弹菜单并吞掉，释放一并吞掉，
    // 规避 FluVNavigationIconTextItem::mouseReleaseEvent 不辨按键即发 itemClicked 的翻页 bug
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    FluStackedLayout *m_sLayout;
    FluVNavigationView *m_navView;
    NewChatPage *m_newChatPage = nullptr;
    int m_sessionCount = 0;
    QHash<QString, ChatSessionPage *> m_sessions;
    // 导航/堆叠 key（Session_*）→ 会话数据短 ID：删除/重命名据 key 定位 index.json 条目
    QHash<QString, QString> m_keyToDataId;
    // 导航子项控件（仅本体）→ 其 key：__renameSession 反查 / __deleteSession 清理专用
    // （右键过滤不再直查此表，改用 m_ctxWatchedToKey，防止登记歧义）
    QHash<QWidget *, QString> m_childWidgetToKey;
    // 已装右键过滤器的全部控件（会话子项本体 + 其所有后代）→ key：
    // 命中即沿父链解析归属会话；子项删除/重建时同步清理，防悬空指针误命中
    QHash<QWidget *, QString> m_ctxWatchedToKey;
};
