#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class MessageBubbleWidget : public FluWidget
{
    Q_OBJECT
public:
    enum Role { User, Assistant };
    Q_ENUM(Role)

    explicit MessageBubbleWidget(Role role, QWidget *parent = nullptr);

    void setRole(Role role);
    Role role() const { return m_role; }

    void setContent(const QString &markdown);
    QString content() const { return m_content->toMarkdown(); }
    void refreshSize();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSize();

private:
    Role m_role = Assistant;
    QTextBrowser *m_content = nullptr;
    bool m_updatingSize = false;
};
