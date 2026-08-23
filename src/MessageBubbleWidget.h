#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QTimer;

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

    // 流式渲染（打字机）：增量追加思考/正文，流结束一次性渲染 markdown
    void startStreaming(const QString &placeholder = QString());
    void appendThinkingText(const QString &delta);
    void appendText(const QString &delta);
    void finishStreaming();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSize();
    void scheduleStreamResize();

private:
    Role m_role = Assistant;
    QTextBrowser *m_content = nullptr;
    bool m_updatingSize = false;
    bool m_streaming = false;
    QString m_thinkingBuffer;   // 累积思考原文
    QString m_textBuffer;       // 累积正文原文
    QTimer *m_streamResizeTimer = nullptr;   // 流式期间测量节流
};
