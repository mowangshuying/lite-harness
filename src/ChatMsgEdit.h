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

    // 本会话回合繁忙态（异步化 P2 引入，设计文档 §3.5b）：宿主页面接 AgentLoop::runningChanged
    // 注入，覆盖含召回异步飞行期的整回合输入禁用。P4 起为唯一禁用来源（原跨会话
    // 全局等待态网关随同步链清退一并删除）
    void setTurnBusy(bool busy);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void sendMessage(const QString &text);
    // 运行中（发送钮已切停止形态）点击发出：宿主页面接 ChatSessionPage::stop 中止回合
    void stopRequested();
    // 用户在下拉框改选模型时发出（程序化 setCurrentModel 不回环）
    void modelChanged(const QString &model);

private:
    // 依 m_turnBusy 刷新形态：输入框 enabled + 发送钮停止/发送形态切换（钮恒可点）
    void applyBusyState();

    QTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
    FluComboBox *m_modelComboBox = nullptr;
    bool m_turnBusy = false; // 本会话回合态（runningChanged 驱动）
};