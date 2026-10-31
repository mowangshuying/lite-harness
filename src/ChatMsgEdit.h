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

    // 键盘可达（评审 B2）：把焦点交还输入框——宿主 Ctrl+L / 回合终局（权限卡裁决后焦点悬空）
    // 调用；运行中（输入框被禁用）不改焦点，避免把焦点塞到不可交互控件上
    void focusInput();

    // 键盘可达（P1-7）：把焦点交给发送/停止钮。与 focusInput 的分工是「回合内 vs 回合外」——
    // 运行中输入框禁用、focusInput 会主动跳过，此时唯一可交互控件是本钮（停止形态），
    // 权限卡裁决完毕就该落到这里，键盘用户 Enter 即可中止，不必回头找鼠标。
    void focusSendButton();

    // P1-9 空态引导：宿主/发起页的示例提示回填输入框（整段替换，非追加——
    // 用户手打的半截草稿不该被一次点击吞掉，故只在输入框为空时由调用方决定填入）。
    // 落点选纯文本口径，与用户手输同形，不引入富文本残留
    void setText(const QString &text);

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

    // 重读 config.json 的可选模型清单并原地刷新下拉项（清单未变整体早退、不发信号）；
    // 由模型下拉弹层展开前的事件过滤器调用——改完配置无需重启即可选新模型
    void reloadModelOptions();

    QTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
    FluComboBox *m_modelComboBox = nullptr;
    bool m_turnBusy = false; // 本会话回合态（runningChanged 驱动）
    // IME 组合态旗标（P0-1）：true = 有未上屏的预编辑串，此时 Enter 必须放行给输入法。
    // 由 eventFilter 里的 QEvent::InputMethod 按 preeditString 是否非空写入，
    // FocusOut 复位；Enter 分支只读不写（见 .cpp 口径说明）
    bool m_imeComposing = false;
};
