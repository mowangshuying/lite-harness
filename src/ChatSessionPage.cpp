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
#include "TodoCard.h"

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
        // 后端收口：若仍待决权限（如挂起期间用户又发了消息 → run() 拒绝 → error），
        // 必须显式按拒绝放行队列，否则 m_awaitingPermission/m_running 永真导致会话死锁；
        // stop() 路径后端已自行回填时，resolvePermission 的待决守卫使其成为 no-op，不会双重裁决。
        // 卡片同步落为"已拒绝"留痕（不发 userResolved，避免二次调用 resolvePermission）。
        // Gate2 MAJOR-1：先捕获事发时的旧卡——resolvePermission(false) 同步续跑队列时，
        // 同批下一个待询问调用可能就地再建"新卡"（handler 置 m_permissionCard）。
        // 只收旧卡，新卡留给用户裁决，否则后端再次永久挂起且无卡可裁。
        QPointer<PermissionCard> staleCard = m_permissionCard;
        m_agentLoop->resolvePermission(false);
        if (staleCard && !staleCard->isResolved())
            staleCard->resolveDenySilently();
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

    // 记忆沉淀阶段开始（仅自然结束分支，阻塞提取/合并前发射）：正文就地定稿
    // markdown 并在气泡时间线挂「记忆整理中...」live 进度卡；提取结果卡
    // （toolOutputReady toolName="memory"）到达后就地切换为终态留痕
    connect(m_agentLoop, &AgentLoop::memoryPhaseStarted, this, [this]() {
        if (m_currentBubble)
        {
            m_currentBubble->appendMemoryProgress();
            QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
        }
    });

    // 权限确认：工具即将执行但需用户裁决，后端队列暂停直至 resolvePermission。
    // 卡片挂进当前流式气泡的时间线（与工具块同一套约定：裁决留痕停在对应工具执行
    // 之前，后续工具块/正文出现在其后）；回合外兜底（无流式气泡）挂会话流末尾。
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
                if (m_currentBubble)
                    m_currentBubble->appendPermissionCard(card);
                else
                    m_scrollView->getMainLayout()->addWidget(card);
                QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
            });

    // 任务清单：会话流常驻卡片 —— 首次 todoUpdated 时挂到流末尾（锚定在首次出现的
    // 时间线位置，之后只就地刷新内容、不再增殖；lcc s05 全量替换语义由 TodoCard 内部消化）。
    // 信号契约：todoUpdated(todos)，元素 {content, status: pending|in_progress|completed}
    connect(m_agentLoop, &AgentLoop::todoUpdated, this, [this](const QJsonArray &todos) {
        if (!m_todoCard)
        {
            auto *card = new TodoCard(this);
            // QPointer：clearMessages 销毁或异常删除后自动置空，下次信号到达再自动重挂
            m_todoCard = card;
            m_scrollView->getMainLayout()->addWidget(card);
        }
        m_todoCard->setTodos(todos);
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    });

    connect(m_inputEdit, &ChatMsgEdit::sendMessage, this, [this](const QString &text) {
        // 运行态预查：运行中不建气泡、不动旧现场。若照旧走 startAssistantStream，
        // 旧气泡会被先冻结、新气泡又被 run() 拒绝后的 error 链收掉置空，
        // 旧循环后续 delta 全部丢失、时间线撕裂（重入提示改由本 handler 独立气泡给出）。
        if (m_agentLoop->isRunning())
        {
            // 待决权限按拒绝放行队列（防死锁：挂起期间不放行则 m_awaitingPermission/
            // m_running 永真）；与 error handler 同一套 staleCard 约定——resolvePermission(false)
            // 同步续跑时同批下一个待询问调用可能就地再建新卡，只收旧卡、新卡留给用户裁决。
            // 无待决询问时 resolvePermission 的待决守卫使其成为 no-op。
            QPointer<PermissionCard> staleCard = m_permissionCard;
            m_agentLoop->resolvePermission(false);
            if (staleCard && !staleCard->isResolved())
                staleCard->resolveDenySilently();
            addMessage(MessageBubbleWidget::Role::Assistant,
                       QStringLiteral("*Error:* Agent 仍在运行中，请等待完成后再发送"));
            return;
        }
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
    // 先停后端：stop() 的待决分支按拒绝回填且**不续跑队列**——若只调 resolvePermission(false)
    // 会同步续跑，同轮第二个待询问工具可能当场重建权限卡，随即被下面的 teardown 删掉，
    // 后端再次挂起且无卡可裁决（Gate1 MAJOR-2）。收口后再删控件。
    m_agentLoop->stop();
    if (m_permissionCard && !m_permissionCard->isResolved())
        m_permissionCard->resolveDenySilently();
    m_permissionCard = nullptr;
    m_todoCard = nullptr; // 常驻任务卡随下方销毁循环一并移除（QPointer 本会自动置空，显式清引用表明意图）
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
