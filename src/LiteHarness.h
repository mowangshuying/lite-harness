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

protected:
    FluStackedLayout *m_sLayout;
    FluVNavigationView *m_navView;
    NewChatPage *m_newChatPage = nullptr;
    int m_sessionCount = 0;
    QHash<QString, ChatSessionPage *> m_sessions;
};
