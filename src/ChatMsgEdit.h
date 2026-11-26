#pragma once

#include <FluWidget.h>

#include "ImageAttach.h" // s18.6 图片输入：待发图片载荷类型进信号签名，moc 需要完整类型

#include <QList>

class PlainPasteTextEdit; // 只吃纯文本的 QTextEdit 子类（判据见 PasteText.h / PlainPasteTextEdit.h）
class SendMsgButton;
class FluComboBox;
class QHBoxLayout;
class QPushButton;

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
    // s18.6 图片输入批：签名多了一个待发图片清单。空清单时下游行为与改造前逐字相同
    // （AgentLoop::run 的附件形参有默认值，history.json 里也仍是纯串 content）。
    void sendMessage(const QString &text, const QList<ImageAttach::Pending> &images);
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

    // ---- s18.6 待发图片 ----
    // 入队（剪贴板与拖拽两个入口共用）：逐张过闸门，拒收的给一条可见提示
    void addPendingImages(const QList<ImageAttach::Pending> &incoming);
    // 清空待发图片（发送成功、点「清除」、宿主 setTurnBusy 之外不自动动）
    void clearPendingImages();
    // 重建缩略图行（增删都整体重排——四张封顶，重建成本远低于维护索引一致性）
    void rebuildAttachRow();
    // 发送口单点：Enter（eventFilter）与发送钮两条路原本各写一遍取文本 + 清空，
    // 加了附件之后必须同源，否则会出现「钮能带图、Enter 丢图」这种只在键盘路径复现的缺陷
    void emitSend();
    // 图片条显隐决定本控件高度上限（原固定 100 是按「约 3 行 + 工具行」定的）
    void applyColumnHeight();

    PlainPasteTextEdit *m_textEdit = nullptr;
    SendMsgButton *m_sendMsgButton = nullptr;
    FluComboBox *m_modelComboBox = nullptr;
    // 待发图片条：输入框上方一行缩略图钮（点一个即移除该张）+ 末尾一张「清除全部」。
    // 空清单时整行隐藏——没有图就不该在输入区占一行高度
    QWidget *m_attachRow = nullptr;
    QHBoxLayout *m_attachLayout = nullptr;
    QHBoxLayout *m_attachStrip = nullptr; // 芯片容器（每次增删整体重建）
    class QLabel *m_attachHint = nullptr; // 拒收原因落点（超限/超数是可见降级，不静默丢图）
    QList<ImageAttach::Pending> m_pendingImages;
    bool m_turnBusy = false; // 本会话回合态（runningChanged 驱动）
    // IME 组合态旗标（P0-1）：true = 有未上屏的预编辑串，此时 Enter 必须放行给输入法。
    // 由 eventFilter 里的 QEvent::InputMethod 按 preeditString 是否非空写入，
    // FocusOut 复位；Enter 分支只读不写（见 .cpp 口径说明）
    bool m_imeComposing = false;
};
