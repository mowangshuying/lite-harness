#include "ChatMsgEdit.h"
#include <QVBoxLayout>
#include <QTextEdit>
#include <QTextCursor> // setText 的光标归位（moveCursor(QTextCursor::End)）
#include <QApplication>
#include <QClipboard>
#include <QKeySequence>
#include <QKeyEvent>
#include <QMimeData>
#include <QStyle>
#include <QInputMethodEvent>
#include <functional>
#include "SendMsgButton.h"
#include <FluScrollDelegate.h>
#include <FluAction.h>
#include <FluPMenu.h>
#include <FluComboBox.h>
#include "AgentConstants.h" // 模型清单单源（config.json 可配置，取值见 AgentConst::modelOptions()）
#include "LayoutConstants.h"
#include "ThemeAware.h"

// 可选模型清单见 AgentConst::modelOptions()（AgentConstants.h）：config.json 的
// modelOptions 键（逗号分隔）优先，未配置/写空回退内置两项；生效清单首项为回落默认项
// （AgentLoop::model() 不在清单内时下拉显示并选中它），可用 defaultModel 键单独指定

// 模型下拉框占位宽：随清单里最长的模型名自适应。原固定 130 是按内置 qwen3.8-flash 调的，
// 配置化之后清单可长可短，写 deepseek-r1-distill-llama-70b 这类长名会被截断成看不懂的片段。
// 下限 130 保持既有观感（内置两项最长 13 字符，按此口径算得 128 落在下限，未配置时宽度不变），
// 上限 220 防超长名把右对齐工具行挤爆。系数 8 为默认字体 14px 下 latin 均宽的保守值，
// 24 给下拉箭头与内边距。定义成文件内静态函数：清单在运行期可被 reloadModelOptions 换掉，
// 宽度必须跟着重算，不能只算一次。
static int modelComboWidth(const QStringList &options)
{
    int longest = 0;
    for (const QString &option : options)
        longest = qMax(longest, option.size());
    return qMin(220, qMax(130, 24 + longest * 8));
}

// IME 组合态判定（P0-1）见 eventFilter：中文输入法在候选词未上屏时，Enter 的语义是
// 「提交当前候选」而非「发送消息」，此刻必须放行按键，否则半截拼音会被当消息整条发出。
//
// 信号口径取「本控件收到的 QEvent::InputMethod 里 preeditString 是否非空」，理由：
// ① Qt 6.9 的 Qt::InputMethodQuery 枚举没有组合阶段查询位（无 IMQueryStage/HasPreedit），
//    想「主动问一句现在在不在组合中」在 API 层就没有入口；
// ② QInputMethod::isAnimating() 在 Windows TSF 组合期不保证为真，不可依赖；
// ③ IME 的预编辑串上下行本身就是组合起止的唯一权威事件流——非空即组合中，
//    提交/取消时必然带空 preedit 下来，旗标随之复位，无需定时器兜底。
// 代价是旗标只能由事件驱动更新（被动记录），因此 FocusOut 必须显式复位：
// 否则「组合中途点走别处」会把旗标卡在 true，回来按 Enter 永远发不出去。

ChatMsgEdit::ChatMsgEdit(QWidget *parent) : FluWidget(parent)
{
    setMaximumWidth(LayoutConst::kColumnMaxWidth);
    setMaximumHeight(100);
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(4, 4, 4, 4);
    vMainLayout->setSpacing(0);
    setLayout(vMainLayout);

    m_textEdit = new QTextEdit(this);
    new FluScrollDelegate(m_textEdit); // 仅需 parent 联动滚轮行为，无需持有引用
    m_textEdit->setObjectName("textEdit");
    m_textEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    m_textEdit->installEventFilter(this);
    vMainLayout->addWidget(m_textEdit);

    connect(m_textEdit, &QTextEdit::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto menu = new FluPMenu(this);

        auto addEditAction = [menu](FluAwesomeType icon, const QString &text, const QKeySequence &shortcut,
                                    bool enabled, const std::function<void()> &handler) {
            auto action = new FluAction(icon, text, menu);
            action->setShortcut(shortcut);
            action->setEnabled(enabled);
            menu->addAction(action);
            QObject::connect(action, &QAction::triggered, menu, handler);
        };

        // i18n 第八轮：右键菜单源文本统一为中文（每次弹出重建，无需 LanguageChange 处理）
        addEditAction(FluAwesomeType::Undo, tr("撤销"), QKeySequence::Undo,
                      m_textEdit->document()->isUndoAvailable(), [this]() { m_textEdit->undo(); });
        addEditAction(FluAwesomeType::Redo, tr("重做"), QKeySequence::Redo,
                      m_textEdit->document()->isRedoAvailable(), [this]() { m_textEdit->redo(); });
        menu->addSeparator();
        addEditAction(FluAwesomeType::Cut, tr("剪切"), QKeySequence::Cut,
                      m_textEdit->textCursor().hasSelection(), [this]() { m_textEdit->cut(); });
        addEditAction(FluAwesomeType::Copy, tr("复制"), QKeySequence::Copy,
                      m_textEdit->textCursor().hasSelection(), [this]() { m_textEdit->copy(); });
        addEditAction(FluAwesomeType::Paste, tr("粘贴"), QKeySequence::Paste,
                      QApplication::clipboard()->mimeData()->hasText(), [this]() { m_textEdit->paste(); });
        addEditAction(FluAwesomeType::SelectAll, tr("全选"), QKeySequence::SelectAll,
                      !m_textEdit->toPlainText().isEmpty(), [this]() { m_textEdit->selectAll(); });

        menu->exec(m_textEdit->mapToGlobal(pos));
        menu->deleteLater();
    });

    auto toolSetsLayout = new QHBoxLayout();
    vMainLayout->addLayout(toolSetsLayout);
    toolSetsLayout->setContentsMargins(0, 0, 0, 0);
    // 与消息列同栏间隙单源（P2-D3）：15 此前在 ChatSessionPage 两处与本行各抄一遍
    toolSetsLayout->setSpacing(LayoutConst::kColGap);
    toolSetsLayout->setAlignment(Qt::AlignRight);

    // 模型下拉框：FluComboBox 自带三主题 QSS 与 hover/pressed 态，弹层为 FluIndicatorRoundMenu
    // （当前项左侧画主题色选中标识竖条，WinUI ComboBox 观感）；高度 30 与 SendMsgButton 同高
    m_modelComboBox = new FluComboBox(this);
    m_modelComboBox->addItems(AgentConst::modelOptions()); // 清单来自 config.json（未配置回退内置两项）
    m_modelComboBox->setFixedWidth(modelComboWidth(AgentConst::modelOptions())); // 占位宽随清单最长名自适应，理由见 modelComboWidth
    // 配置驱动：弹层展开前重读清单（事件过滤器早于控件处理），改完 config.json
    // 不必重启即可在下拉里选中新加的模型；清单未变时 reloadModelOptions 整体早退
    m_modelComboBox->installEventFilter(this);
    toolSetsLayout->addWidget(m_modelComboBox);

    m_sendMsgButton = new SendMsgButton(this);
    toolSetsLayout->addWidget(m_sendMsgButton);

    connect(m_sendMsgButton, &QPushButton::clicked, this, [this]() {
        // 停止形态单源判定走 m_turnBusy（与钮视觉形态同一驱动）：运行中点击只发中止，
        // 绝不把输入框残留文本当消息发出；空闲态维持原发送逻辑
        if (m_turnBusy)
        {
            emit stopRequested();
            return;
        }
        QString text = m_textEdit->toPlainText().trimmed();
        if (!text.isEmpty()) {
            emit sendMessage(text);
            m_textEdit->clear();
        }
    });

    // 用户改选 → 对外广播（弹层触发 setCurrentIndex → currentTextChanged；
    // 程序化路径在 setCurrentModel 内抑制信号，不回环）
    connect(m_modelComboBox, &FluComboBox::currentTextChanged, this,
            [this](const QString &model) { emit modelChanged(model); });

    // FluComboBox 构造未自连 themeChanged（仅其弹层自连），主题切换时需单独重刷控件本体 QSS；
    // 作为 bind 的 extraRefresh 挂入（原对 m_modelComboBox 的独立 connect 一并收敛，
    // bind 连接以本控件为 context，combo 随父销毁，生命周期等价）
    ThemeAware::bind("ChatMsgEdit.qss", this, [this] { m_modelComboBox->onThemeChanged(); });
}

ChatMsgEdit::~ChatMsgEdit()
{
}

void ChatMsgEdit::setCurrentModel(const QString &model)
{
    // 不在清单内回落默认项（AgentConst::defaultModel()：config.json 的 defaultModel 键，
    // 未配置即生效清单首项）；blockSignals 抑制 currentTextChanged，
    // 程序化设置不对外回环 emit modelChanged
    int index = m_modelComboBox->findText(model);
    if (index < 0)
        index = m_modelComboBox->findText(AgentConst::defaultModel());
    if (index < 0)
        index = 0; // 默认项也不在清单内（配置写坏的极端情形）：取首项保底
    m_modelComboBox->blockSignals(true);
    m_modelComboBox->setCurrentIndex(index);
    m_modelComboBox->blockSignals(false);
}

QString ChatMsgEdit::currentModel() const
{
    return m_modelComboBox->currentText();
}

// 重读 config.json 的可选模型清单并原地刷新下拉项（配置化改造的核心收益：改完配置
// 不必重启即可选新模型）。清单未变则整体早退——不动弹层、不发信号，避免每次点击都
// 重建 item 列表与无谓的 currentTextChanged 广播；原选中项被配置移除时回落默认项，
// 并对外 emit modelChanged 同步后端（否则 UI 显示与实际请求模型分叉）。
void ChatMsgEdit::reloadModelOptions()
{
    const QStringList options = AgentConst::modelOptions();
    QStringList shown;
    shown.reserve(m_modelComboBox->count());
    for (int i = 0; i < m_modelComboBox->count(); ++i)
        shown.append(m_modelComboBox->itemText(i));
    if (shown == options)
        return;

    // 清单变了才重算占位宽：配置里新写长的模型名不再被固定宽截断
    m_modelComboBox->setFixedWidth(modelComboWidth(options));

    const QString keep = m_modelComboBox->currentText();
    m_modelComboBox->blockSignals(true);
    m_modelComboBox->clear();
    m_modelComboBox->addItems(options);
    int index = m_modelComboBox->findText(keep);
    if (index < 0)
        index = m_modelComboBox->findText(AgentConst::defaultModel());
    m_modelComboBox->setCurrentIndex(index < 0 ? 0 : index);
    m_modelComboBox->blockSignals(false);
    if (m_modelComboBox->currentText() != keep)
        emit modelChanged(m_modelComboBox->currentText());
}

void ChatMsgEdit::setTurnBusy(bool busy)
{
    // 本会话回合态唯一禁用来源（异步化 P4 起；宿主页面接 runningChanged 直连注入）；
    // 同值早退防噪声刷新。禁用面：输入框（Enter 发送在禁用控件上天然关闭）；发送钮不再
    // 禁用而是随 busy 切停止/发送形态（见 applyBusyState）；模型下拉不禁（改选只写成员不发起请求）
    if (m_turnBusy == busy)
        return;
    m_turnBusy = busy;
    applyBusyState();
}

void ChatMsgEdit::focusInput()
{
    // 运行中输入框处于禁用态（setTurnBusy），禁用控件拿焦点无意义且会打断
    // 「停止」钮的键盘操作，此处直接跳过
    if (m_turnBusy || !m_textEdit)
        return;
    m_textEdit->setFocus(Qt::OtherFocusReason);
}

void ChatMsgEdit::focusSendButton()
{
    // 与 focusInput 互补（见 .h 分工说明）：本方法专供「回合内」的焦点归位——
    // 权限卡裁决后两钮禁用、焦点悬空，而回合仍在跑、输入框禁用，
    // 此时钮是唯一可交互控件（停止形态），Enter 即中止。
    // 空闲态也允许调用（钮恒可点），不做 m_turnBusy 早退。
    if (m_sendMsgButton)
        m_sendMsgButton->setFocus(Qt::OtherFocusReason);
}

void ChatMsgEdit::setText(const QString &text)
{
    // P1-9 空态引导回填入口：示例提示 chip 点击后整段替换输入内容。
    // 用 setPlainText 而非 insert：chip 语义是「给我一个起点草稿」，不是往用户已打的
    // 半截稿子里插一段——追加会把两次点击的文案粘成一句不通的话。
    if (m_textEdit)
    {
        m_textEdit->setPlainText(text);
        // 光标落到文末：chip 文案常以「：」或占位结尾，用户接着打字不必先按 End
        m_textEdit->moveCursor(QTextCursor::End);
    }
}

void ChatMsgEdit::applyBusyState()
{
    m_textEdit->setEnabled(!m_turnBusy);
    // 「运行中缺停止入口」修复：发送钮不再随 busy 禁用，而是切成停止形态
    // （方块字形 + 强调蓝淡染），点击走 stopRequested 分支；空闲恢复发送形态。
    // 输入框禁用口径不变（Enter 发送在禁用控件上天然收不到键事件）；模型下拉不禁
    m_sendMsgButton->setStopMode(m_turnBusy);
    m_sendMsgButton->setToolTip(m_turnBusy ? tr("停止") : tr("发送"));
}

bool ChatMsgEdit::eventFilter(QObject *watched, QEvent *event)
{
    // 模型下拉：鼠标按下/键盘展开弹层前先重读配置清单（过滤器早于控件处理，
    // 刷新发生在弹层构建之前，用户展开即见最新配置；清单未变则早退无副作用）
    if (watched == m_modelComboBox &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress))
    {
        reloadModelOptions();
    }
    if (watched == m_textEdit)
    {
        if (event->type() == QEvent::InputMethod)
        {
            // IME 组合态旗标（P0-1）：预编辑串非空 = 候选未上屏 = 组合中。
            // 只记录不拦截——事件必须继续交给 QTextEdit，否则候选词根本没法上屏。
            m_imeComposing = !static_cast<QInputMethodEvent *>(event)->preeditString().isEmpty();
            return false;
        }
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        {
            // 焦点态经 focused 属性驱动 QSS 边框反馈（FluentUI 标准模式：setProperty + polish）
            setProperty("focused", event->type() == QEvent::FocusIn);
            style()->unpolish(this);
            style()->polish(this);
            // 失焦显式复位旗标：组合中途被点走时，IME 未必补发空 preedit 事件，
            // 卡住的 true 会让「回来按 Enter」永远发不出去（比误发更难排查）
            if (event->type() == QEvent::FocusOut)
                m_imeComposing = false;
        }
        else if (event->type() == QEvent::KeyPress)
        {
            auto keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            {
                // 输入法组合中：Enter 归输入法（提交候选），本控件一律不截获——
                // 截获会把未上屏的拼音当成品消息发出，中文用户每次敲回车都踩。
                // 旗标由上方 QEvent::InputMethod 维护（口径见该处注释）。
                if (m_imeComposing)
                    return false;

                const Qt::KeyboardModifiers pure = keyEvent->modifiers() & ~Qt::KeypadModifier;
                if (pure == Qt::ShiftModifier)
                {
                    // Shift+Enter: 插入换行，让默认处理继续
                    return false;
                }
                if (pure != Qt::NoModifier && pure != Qt::ControlModifier)
                {
                    // Alt/Meta 等组合的 Enter 语义未定义：不截获，交默认处理，
                    // 避免「任何修饰键组合都当发送」误触（原实现口径）
                    return false;
                }
                // Enter / Ctrl+Enter: 触发发送
                QString text = m_textEdit->toPlainText().trimmed();
                if (!text.isEmpty())
                {
                    m_textEdit->clear();
                    emit sendMessage(text);
                }
                return true; // 拦截事件，不插入换行
            }
        }
    }
    return FluWidget::eventFilter(watched, event);
}
