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

protected:
    FluStackedLayout *m_sLayout;
    FluVNavigationView *m_navView;
    NewChatPage *m_newChatPage = nullptr;
    int m_sessionCount = 0;
    QHash<QString, ChatSessionPage *> m_sessions;
};
