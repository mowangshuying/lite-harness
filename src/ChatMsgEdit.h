#pragma once

#include <FluWidget.h>

class QTextEdit;
class SendMsgButton;
class FluComboBox;

class ChatMsgEdit : public FluWidget
{
    Q_OBJECT
public:
    ChatMsgEdit(QWidget* parent = nullptr);
    virtual ~ChatMsgEdit() override;

    void onThemeChanged() override;

    // 程序化设置当前选中模型（不回环 emit modelChanged；不在选项列表内回落默认项）
    void setCurrentModel(const QString &model);
    // 当前显示的模型文本
    QString currentModel() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void sendMessage(const QString &text);
    // 用户在下拉框改选模型时发出（程序化 setCurrentModel 不回环）
    void modelChanged(const QString &model);

private:
    QString m_qssFile;
    QTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
    FluComboBox *m_modelComboBox = nullptr;
};