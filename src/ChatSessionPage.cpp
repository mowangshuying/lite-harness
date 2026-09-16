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
#include "ToolBlock.h"
#include "PermissionCard.h"

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
        // 防御收口：正常契约下待决权限会暂停队列、finished 不会先于裁决到达；
        // 若出现残留待决卡片，落为"已拒绝"留痕（不再转呼 resolvePermission，交给后端收口）
        if (m_permissionCard && !m_permissionCard->isResolved())
            m_permissionCard->resolveDenySilently();
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
        // 用户 stop/异常时后端对待决询问已自动按拒绝回填：卡片同步收口为"已拒绝"留痕，
        // 不重复调用 resolvePermission（避免双重裁决）
        if (m_permissionCard && !m_permissionCard->isResolved())
            m_permissionCard->resolveDenySilently();
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
    // 工具执行可视化：按到达顺序内嵌到当前流式气泡的时间线中（正文与工具块交替出现）。
    // 信号契约：toolOutputReady(toolName, summary, output)，summary 为关键参数
    connect(m_agentLoop, &AgentLoop::toolOutputReady, this,
            [this](const QString &toolName, const QString &summary, const QString &output) {
                if (m_currentBubble)
                {
                    m_currentBubble->appendToolExecution(toolName, summary, output);
                    QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
                    return;
                }
                // 边界情况（无流式气泡，如信号在回合外到达）：独立气泡兜底，避免信息静默丢失
                addMessage(MessageBubbleWidget::Role::Assistant,
                           tr("%1 %2:\n```\n%3\n```\n\n输出:\n```\n%4\n```")
                               .arg(ToolBlock::toolTitleText(toolName), toolName, summary, output));
            });

    // 权限确认：工具即将执行但需用户裁决，后端队列暂停直至 resolvePermission。
    // 卡片挂载在会话流末尾（当前流式气泡之下、随列表滚动）；裁决后收为单行留痕。
    // 信号契约：permissionRequired(toolName, summary, reason)，reason 为英文短句（卡片内转译中文）
    connect(m_agentLoop, &AgentLoop::permissionRequired, this,
            [this](const QString &toolName, const QString &summary, const QString &reason) {
                // 防御：契约保证同一时刻至多一个待决；若残留未裁决旧卡直接丢弃（后端自行收口）
                if (m_permissionCard && !m_permissionCard->isResolved())
                    m_permissionCard->deleteLater();
                auto *card = new PermissionCard(this);
                connect(card, &PermissionCard::userResolved, this,
                        [this](bool allow) { m_agentLoop->resolvePermission(allow); });
                card->setPermissionRequest(toolName, summary, reason);
                m_permissionCard = card;
                m_scrollView->getMainLayout()->addWidget(card);
                QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
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
    // 清屏时若仍有待决权限卡：视为拒绝对待（后端队列停在裁决上，必须收口后再删控件）
    if (m_permissionCard && !m_permissionCard->isResolved())
        m_agentLoop->resolvePermission(false);
    m_permissionCard = nullptr;
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
