#include "NewChatPage.h"
#include <FluUtils.h>
#include <QLabel>
#include <QVBoxLayout>
#include <FluTextEdit.h>
#include "ChatMsgEdit.h"
#include <QPixmap>

NewChatPage::NewChatPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    setLayout(vMainLayout);
    vMainLayout->setAlignment(Qt::AlignCenter);
    vMainLayout->setContentsMargins(35, 35, 35, 35);
    vMainLayout->setSpacing(15);

    auto label = new QLabel(this);
    label->setFixedSize(30, 30);

    QPixmap pixmap(":/res/LiteHarness.ico");
    pixmap = pixmap.scaled(30, 30);
    label->setPixmap(pixmap);
    vMainLayout->addWidget(label,0, Qt::AlignHCenter);

    auto chatMsgEdit = new ChatMsgEdit(this);
    vMainLayout->addWidget(chatMsgEdit);


    onThemeChanged();
}

void NewChatPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("NewChatPage.qss", this, FluThemeUtils::getUtils()->getTheme());
}