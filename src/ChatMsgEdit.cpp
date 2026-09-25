#include "ChatMsgEdit.h"
#include <QVBoxLayout>
#include <QTextEdit>
#include <QApplication>
#include <QClipboard>
#include <QKeySequence>
#include <QKeyEvent>
#include <QMimeData>
#include <QStyle>
#include <functional>
#include "SendMsgButton.h"
#include <FluScrollDelegate.h>
#include <FluAction.h>
#include <FluPMenu.h>
#include <FluComboBox.h>
#include "AgentConstants.h" // 模型清单单源（原文件内 static 列表迁入头文件，值不变）
#include "LayoutConstants.h"
#include "ThemeAware.h"

// 可选模型清单见 AgentConst::kModelOptions（AgentConstants.h）：字面量列表，
// 不做注册表/配置等多余抽象；首项为回落默认项
// （AgentLoop::model() 不在列表内时下拉显示并选中它）

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

        addEditAction(FluAwesomeType::Undo, tr("Undo"), QKeySequence::Undo,
                      m_textEdit->document()->isUndoAvailable(), [this]() { m_textEdit->undo(); });
        addEditAction(FluAwesomeType::Redo, tr("Redo"), QKeySequence::Redo,
                      m_textEdit->document()->isRedoAvailable(), [this]() { m_textEdit->redo(); });
        menu->addSeparator();
        addEditAction(FluAwesomeType::Cut, tr("Cut"), QKeySequence::Cut,
                      m_textEdit->textCursor().hasSelection(), [this]() { m_textEdit->cut(); });
        addEditAction(FluAwesomeType::Copy, tr("Copy"), QKeySequence::Copy,
                      m_textEdit->textCursor().hasSelection(), [this]() { m_textEdit->copy(); });
        addEditAction(FluAwesomeType::Paste, tr("Paste"), QKeySequence::Paste,
                      QApplication::clipboard()->mimeData()->hasText(), [this]() { m_textEdit->paste(); });
        addEditAction(FluAwesomeType::SelectAll, tr("Select All"), QKeySequence::SelectAll,
                      !m_textEdit->toPlainText().isEmpty(), [this]() { m_textEdit->selectAll(); });

        menu->exec(m_textEdit->mapToGlobal(pos));
        menu->deleteLater();
    });

    auto toolSetsLayout = new QHBoxLayout();
    vMainLayout->addLayout(toolSetsLayout);
    toolSetsLayout->setContentsMargins(0, 0, 0, 0);
    toolSetsLayout->setSpacing(15);
    toolSetsLayout->setAlignment(Qt::AlignRight);

    // 模型下拉框：FluComboBox 自带三主题 QSS 与 hover/pressed 态，弹层为 FluIndicatorRoundMenu
    // （当前项左侧画主题色选中标识竖条，WinUI ComboBox 观感）；高度 30 与 SendMsgButton 同高
    m_modelComboBox = new FluComboBox(this);
    m_modelComboBox->addItems(AgentConst::kModelOptions);
    m_modelComboBox->setFixedWidth(130); // 紧凑宽度：容纳 "qwen3.8-flash" + chevron，不撑爆右对齐工具行
    toolSetsLayout->addWidget(m_modelComboBox);

    m_sendMsgButton = new SendMsgButton(this);
    toolSetsLayout->addWidget(m_sendMsgButton);

    connect(m_sendMsgButton, &QPushButton::clicked, this, [this]() {
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
    // 不在选项内回落默认项（首项 qwen3.8-flash）；blockSignals 抑制 currentTextChanged，
    // 程序化设置不对外回环 emit modelChanged
    int index = m_modelComboBox->findText(model);
    if (index < 0)
        index = 0;
    m_modelComboBox->blockSignals(true);
    m_modelComboBox->setCurrentIndex(index);
    m_modelComboBox->blockSignals(false);
}

QString ChatMsgEdit::currentModel() const
{
    return m_modelComboBox->currentText();
}

void ChatMsgEdit::setTurnBusy(bool busy)
{
    // 本会话回合态唯一禁用来源（异步化 P4 起；宿主页面接 runningChanged 直连注入）；
    // 同值早退防噪声刷新。禁用面沿用旧 Gate 单源口径：输入框+发送钮；Enter 发送走
    // textEdit 键事件，控件禁用后天然关闭；模型下拉不禁（改选只写成员不发起请求）
    if (m_turnBusy == busy)
        return;
    m_turnBusy = busy;
    applyBusyState();
}

void ChatMsgEdit::applyBusyState()
{
    m_textEdit->setEnabled(!m_turnBusy);
    m_sendMsgButton->setEnabled(!m_turnBusy);
}

bool ChatMsgEdit::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_textEdit)
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        {
            // 焦点态经 focused 属性驱动 QSS 边框反馈（FluentUI 标准模式：setProperty + polish）
            setProperty("focused", event->type() == QEvent::FocusIn);
            style()->unpolish(this);
            style()->polish(this);
        }
        else if (event->type() == QEvent::KeyPress)
        {
            auto keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            {
                if (keyEvent->modifiers() & Qt::ShiftModifier)
                {
                    // Shift+Enter: 插入换行，让默认处理继续
                    return false;
                }
                // Enter: 触发发送
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
