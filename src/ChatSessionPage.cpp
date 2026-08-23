#include "ChatSessionPage.h"
#include <FluUtils.h>
#include <FluThemeUtils.h>
#include <FluVScrollView.h>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include "ChatMsgEdit.h"
#include "AgentLoop.h"

ChatSessionPage::ChatSessionPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(35, 35, 35, 35);
    vMainLayout->setSpacing(15);
    setLayout(vMainLayout);

    m_scrollView = new FluVScrollView(this);
    m_scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    m_scrollView->getMainLayout()->setContentsMargins(15, 15, 15, 15);
    m_scrollView->getMainLayout()->setSpacing(15);
    vMainLayout->addWidget(m_scrollView, 1);

    auto hLayout = new QHBoxLayout();
    m_inputEdit = new ChatMsgEdit(this);
    // vMainLayout->addWidget(m_inputEdit, 0, Qt::AlignHCenter);
    hLayout->addWidget(m_inputEdit, 1);

    // Agent Loop：真实模型回复 + 工具调用循环（流式打字机渲染）
    m_agentLoop = new AgentLoop(this);
    connect(m_agentLoop, &AgentLoop::finished, this, [this](const QString &reply) {
        // 流式气泡已存在：收尾渲染后复用该气泡，不另起新气泡
        if (m_currentBubble)
        {
            m_currentBubble->finishStreaming();
            m_currentBubble = nullptr;
        }
        else
        {
            addMessage(MessageBubbleWidget::Role::Assistant, reply);
        }
    });
    connect(m_agentLoop, &AgentLoop::error, this, [this](const QString &err) {
        if (m_currentBubble)
        {
            m_currentBubble->finishStreaming();
            m_currentBubble = nullptr;
        }
        addMessage(MessageBubbleWidget::Role::Assistant, QString("*Error:* %1").arg(err));
    });
    connect(m_agentLoop, &AgentLoop::thinkingDelta, this, [this](const QString &delta) {
        if (m_currentBubble)
        {
            m_currentBubble->appendThinkingText(delta);
            scrollToBottom();
        }
    });
    connect(m_agentLoop, &AgentLoop::textDelta, this, [this](const QString &delta) {
        if (m_currentBubble)
        {
            m_currentBubble->appendText(delta);
            scrollToBottom();
        }
    });

    connect(m_inputEdit, &ChatMsgEdit::sendMessage, this, [this](const QString &text) {
        addMessage(MessageBubbleWidget::Role::User, text);
        startAssistantStream(text); // 创建流式气泡并启动代理循环
    });

    vMainLayout->addLayout(hLayout);

    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, &ChatSessionPage::onThemeChanged);
    onThemeChanged();
}

void ChatSessionPage::addMessage(MessageBubbleWidget::Role role, const QString &content)
{
    auto bubble = new MessageBubbleWidget(role, this);
    bubble->setContent(content);
    m_scrollView->getMainLayout()->addWidget(bubble);
    scrollToBottom();
}

void ChatSessionPage::startAssistantStream(const QString &userText)
{
    // 兜底：上一轮未收到 finished 时收尾清场
    if (m_currentBubble)
    {
        m_currentBubble->finishStreaming();
        m_currentBubble = nullptr;
    }

    m_currentBubble = new MessageBubbleWidget(MessageBubbleWidget::Role::Assistant, this);
    m_currentBubble->startStreaming();
    m_scrollView->getMainLayout()->addWidget(m_currentBubble);
    scrollToBottom();
    m_agentLoop->run(userText);
}

void ChatSessionPage::startConversation(const QString &text)
{
    addMessage(MessageBubbleWidget::Role::User, text);
    startAssistantStream(text);
}

void ChatSessionPage::scrollToBottom()
{
    auto scrollBar = m_scrollView->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void ChatSessionPage::clearMessages()
{
    m_currentBubble = nullptr;
    auto mainLayout = m_scrollView->getMainLayout();
    while (auto item = mainLayout->takeAt(0))
    {
        if (auto widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

void ChatSessionPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);
    //m_scrollView->resize(event->size().width() - 100, m_scrollView->height());

    auto mainLayout = m_scrollView->getMainLayout();
    for (int i = 0; i < mainLayout->count(); ++i)
    {
        auto bubble = qobject_cast<MessageBubbleWidget *>(mainLayout->itemAt(i)->widget());
        if (bubble)
            bubble->refreshSize();
    }

    QTimer::singleShot(0, this, [this]() {
        auto mainLayout = m_scrollView->getMainLayout();
        for (int i = 0; i < mainLayout->count(); ++i)
        {
            auto bubble = qobject_cast<MessageBubbleWidget *>(mainLayout->itemAt(i)->widget());
            if (bubble)
                bubble->refreshSize();
        }
    });
}

void ChatSessionPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("ChatSessionPage.qss", this, FluThemeUtils::getUtils()->getTheme());

    // Re-polish existing bubbles so their role-based QSS picks up the new theme
    auto mainLayout = m_scrollView->getMainLayout();
    for (int i = 0; i < mainLayout->count(); ++i)
    {
        auto widget = mainLayout->itemAt(i)->widget();
        if (widget)
        {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }
}
