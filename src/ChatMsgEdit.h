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

    // 程序化设置当前选中模型（不回环 emit modelChanged；不在选项列表内回落默认项）
    void setCurrentModel(const QString &model);
    // 当前显示的模型文本
    QString currentModel() const;

    // 本会话回合繁忙态（异步化 P2，设计文档 §3.5b）：宿主页面接 AgentLoop::runningChanged
    // 注入，覆盖含召回异步飞行期的整回合输入禁用。与 BlockingGate 全局等待态为**两个
    // 独立来源**，内部 OR 合成后驱动禁用（任一置位即禁），互不覆盖时序；Gate 随 P4
    // 同步链清退后本来源自然独扛
    void setTurnBusy(bool busy);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void sendMessage(const QString &text);
    // 用户在下拉框改选模型时发出（程序化 setCurrentModel 不回环）
    void modelChanged(const QString &model);

private:
    // 两禁用来源（Gate 全局 / 本会话回合）OR 合成 → 输入框+发送钮 enabled
    void applyBusyState();

    QTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
    FluComboBox *m_modelComboBox = nullptr;
    bool m_gateBusy = false; // BlockingGate 等待态（跨会话全局，P4 删）
    bool m_turnBusy = false; // 本会话回合态（runningChanged 驱动）
};