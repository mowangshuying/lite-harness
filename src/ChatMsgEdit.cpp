#include "ChatMsgEdit.h"
#include <QVBoxLayout>
#include <QTextEdit>
#include <FluLabel.h>
#include "SendMsgButton.h"

ChatMsgEdit::ChatMsgEdit(QWidget *parent) : FluWidget(parent)
{
    setMaximumWidth(800);
    setMaximumHeight(130);
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(4, 4, 4, 4);
    vMainLayout->setSpacing(0);
    setLayout(vMainLayout);

    auto textEdit = new QTextEdit(this);
    textEdit->setObjectName("textEdit");
    vMainLayout->addWidget(textEdit);

    auto toolSetsLayout = new QHBoxLayout();
    vMainLayout->addLayout(toolSetsLayout);
    toolSetsLayout->setContentsMargins(0, 0, 0, 0);
    toolSetsLayout->setSpacing(15);
    toolSetsLayout->setAlignment(Qt::AlignRight);

    auto modelLabel = new FluLabel(this);
    modelLabel->setText("deepseek-v4-flash");
    modelLabel->setLabelStyle(FluLabelStyle::BodyTextBlockStyle);
    toolSetsLayout->addWidget(modelLabel);

    auto sendMsgButton = new SendMsgButton(this);
    toolSetsLayout->addWidget(sendMsgButton);

    onThemeChanged();
}

ChatMsgEdit::~ChatMsgEdit()
{
}

void ChatMsgEdit::onThemeChanged()
{
    FluStyleSheetUtils::setQssByFileName("ChatMsgEdit.qss", this, FluThemeUtils::getUtils()->getTheme());
}
