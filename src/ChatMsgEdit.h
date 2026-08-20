#pragma once

#include <FluWidget.h>

class ChatMsgEdit : public FluWidget
{
    Q_OBJECT
public:
    ChatMsgEdit(QWidget* parent = nullptr);
    virtual ~ChatMsgEdit() override;

    void onThemeChanged() override;
private:
    QString m_qssFile;
};