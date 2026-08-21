#include "ChatMsgEdit.h"
#include <QVBoxLayout>
#include <QTextEdit>
#include <FluLabel.h>
#include "SendMsgButton.h"
#include <FluScrollDelegate.h>

ChatMsgEdit::ChatMsgEdit(QWidget *parent) : FluWidget(parent)
{
    setMaximumWidth(800);
    setMaximumHeight(130);
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(4, 4, 4, 4);
    vMainLayout->setSpacing(0);
    setLayout(vMainLayout);

    m_textEdit = new QTextEdit(this);
    auto delegate = new FluScrollDelegate(m_textEdit);
    m_textEdit->setObjectName("textEdit");
    vMainLayout->addWidget(m_textEdit);

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

    onThemeChanged();
}

ChatMsgEdit::~ChatMsgEdit()
{
}

void ChatMsgEdit::onThemeChanged()
{
    FluStyleSheetUtils::setQssByFileName("ChatMsgEdit.qss", this, FluThemeUtils::getUtils()->getTheme());
}
