#pragma once

#include <FluWidget.h>

class QTextEdit;
class SendMsgButton;

class ChatMsgEdit : public FluWidget
{
    Q_OBJECT
public:
    ChatMsgEdit(QWidget* parent = nullptr);
    virtual ~ChatMsgEdit() override;

    void onThemeChanged() override;

signals:
    void sendMessage(const QString &text);

private:
    QString m_qssFile;
    QTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
};