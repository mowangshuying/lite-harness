#include "ChatMsgEdit.h"
#include <QVBoxLayout>
#include <QTextEdit>
#include <QApplication>
#include <QClipboard>
#include <QKeySequence>
#include <QMimeData>
#include <functional>
#include <FluLabel.h>
#include "SendMsgButton.h"
#include <FluScrollDelegate.h>
#include <FluAction.h>
#include <FluPMenu.h>

ChatMsgEdit::ChatMsgEdit(QWidget *parent) : FluWidget(parent)
{
    setMaximumWidth(800);
    setMaximumHeight(100);
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(4, 4, 4, 4);
    vMainLayout->setSpacing(0);
    setLayout(vMainLayout);

    m_textEdit = new QTextEdit(this);
    auto delegate = new FluScrollDelegate(m_textEdit);
    m_textEdit->setObjectName("textEdit");
    m_textEdit->setContextMenuPolicy(Qt::CustomContextMenu);
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

    auto modelLabel = new FluLabel(this);
    modelLabel->setText("deepseek-v4-flash");
    modelLabel->setLabelStyle(FluLabelStyle::BodyTextBlockStyle);
    toolSetsLayout->addWidget(modelLabel);

    m_sendMsgButton = new SendMsgButton(this);
    toolSetsLayout->addWidget(m_sendMsgButton);

    connect(m_sendMsgButton, &QPushButton::clicked, this, [this]() {
        QString text = m_textEdit->toPlainText().trimmed();
        if (!text.isEmpty()) {
            emit sendMessage(text);
            m_textEdit->clear();
        }
    });

    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, &ChatMsgEdit::onThemeChanged);

    onThemeChanged();
}

ChatMsgEdit::~ChatMsgEdit()
{
}

void ChatMsgEdit::onThemeChanged()
{
    FluStyleSheetUtils::setQssByFileName("ChatMsgEdit.qss", this, FluThemeUtils::getUtils()->getTheme());
}
