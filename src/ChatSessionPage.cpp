#include "ChatSessionPage.h"
#include <FluUtils.h>
#include <FluThemeUtils.h>
#include <FluVScrollView.h>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFontMetrics>
#include "ChatMsgEdit.h"
#include "AgentLoop.h"
#include "ToolBlock.h"
#include "PermissionCard.h"
#include "TodoCard.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QPair>

// 消息列与底部输入组的统一栏宽上限（与 ChatMsgEdit::setMaximumWidth(800) 及 NewChatPage 输入栏同参）、
// 页面水平留白（与下方 setContentsMargins 对齐），resizeEvent 据此钳制栏宽并水平居中
static constexpr int kColumnMaxWidth = 800;
static constexpr int kSideMargin = 35;

ChatSessionPage::ChatSessionPage(const QString &sessionDataId, const QString &workDir,
                                 QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(kSideMargin, 35, kSideMargin, 35);
    vMainLayout->setSpacing(15);
    setLayout(vMainLayout);

    m_scrollView = new FluVScrollView(this);
    m_scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    m_scrollView->getMainLayout()->setContentsMargins(15, 15, 15, 15);
    m_scrollView->getMainLayout()->setSpacing(15);
    // 消息列与底部输入组同栏宽（resizeEvent 钳制 min(800, 可用宽)）并居中成同一阅读列
    vMainLayout->addWidget(m_scrollView, 1, Qt::AlignHCenter);

    // 底部输入区：只读工作目录条在上、ChatMsgEdit 在下，同栏同宽（与消息列同列，
    // 栏宽由 resizeEvent 钳制并居中；栏内子控件铺满栏宽，摆位关系不变）
    m_inputSection = new QWidget(this);
    auto sectionLayout = new QVBoxLayout(m_inputSection);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(8); // 与 NewChatPage 输入栏同参数，路径条与输入框读作同一组件

    // 工作目录页眉：「工作目录  <中间省略全路径>」——只读展示，无浏览入口、不可修改；
    // 12px 次要灰字弱化，视觉语言与 NewChatPage 路径条一致（配色见各主题 ChatSessionPage.qss）
    auto workDirRow = new QHBoxLayout();
    workDirRow->setContentsMargins(4, 0, 0, 0); // 与下方输入框内文字起点同列
    workDirRow->setSpacing(8);

    auto workDirCaption = new QLabel(tr("工作目录"), m_inputSection);
    workDirCaption->setObjectName("workDirCaption");

    m_workDirLabel = new QLabel(m_inputSection);
    m_workDirLabel->setObjectName("workDirPath");
    m_workDirLabel->setMinimumWidth(0);               // 允许被压缩，配合中间省略截断超长路径
    m_workDirLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_workDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse); // 仅可选中复制，无修改交互
    m_workDirLabel->installEventFilter(this);         // Resize 时按新宽度重新省略

    // 字号在代码里设定（与 elide 的 QFontMetrics 量纲一致），配色交给三主题 QSS 跟随变色
    QFont secondaryFont = workDirCaption->font();
    secondaryFont.setPixelSize(12);
    workDirCaption->setFont(secondaryFont);
    m_workDirLabel->setFont(secondaryFont);

    workDirRow->addWidget(workDirCaption);
    workDirRow->addWidget(m_workDirLabel);
    sectionLayout->addLayout(workDirRow);

    m_inputEdit = new ChatMsgEdit(m_inputSection);
    sectionLayout->addWidget(m_inputEdit);

    // Agent Loop：真实模型回复 + 工具调用循环（流式打字机渲染）
    // 会话数据 ID + 工作目录经构造注入：前者令任务图/记忆/压缩转写/定时台账等落盘按会话隔离，
    // 后者令上述数据根、技能目录与 bash/子代理进程 cwd 全部随所选工作目录解析（空则回落进程当前目录）
    m_agentLoop = new AgentLoop(sessionDataId, workDir, this);
    // 模型切换接线：用户在下拉框改选 → 后端 setModel（下一轮请求生效）
    connect(m_inputEdit, &ChatMsgEdit::modelChanged, m_agentLoop, &AgentLoop::setModel);
    // 初始显示同步为后端生效模型（MODEL_ID 环境变量值不在两选项内时，下拉回落显示 qwen3.8-flash）
    m_inputEdit->setCurrentModel(m_agentLoop->model());
    // 工作目录在会话存续期固定（构造注入 AgentLoop），只读取一次生效值（含恢复会话的台账回填目录）
    updateWorkDirDisplay();
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

    // 定时任务送达（lcc s12）：后端空闲 tick 交付——展示走带前缀文本，活跃请求走无原文本
    //（lcc deliver 双形态）；复用用户发消息的既有两步链路。首行 isRunning 防御分支
    // 同栈直连下理论不可达（tryDeliverCron 已查 m_running），保留作后端契约变动的保险
    connect(m_agentLoop, &AgentLoop::scheduledUserMessage, this,
            [this](const QString &displayText, const QString &activeRequestText) {
        if (m_agentLoop->isRunning())
            return;
        addMessage(MessageBubbleWidget::Role::User, displayText);
        startAssistantStream(activeRequestText);
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

    vMainLayout->addWidget(m_inputSection, 0, Qt::AlignHCenter);

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

void ChatSessionPage::closeReplayBubble()
{
    if (m_currentBubble)
    {
        m_currentBubble->finishStreaming();
        m_currentBubble = nullptr;
    }
}

void ChatSessionPage::restoreFromDisk()
{
    // 载入内存历史失败（无 ID/文件不存在/损坏）→ 保持空会话（全新会话即此态）
    if (!m_agentLoop->loadSavedHistory())
        return;
    replayHistory(m_agentLoop->messages());
    // 恢复后端生效模型到下拉框；setCurrentModel 不发 modelChanged，无回环
    m_inputEdit->setCurrentModel(m_agentLoop->model());
}

void ChatSessionPage::replayHistory(const QVector<QJsonObject> &messages)
{
    // messages 为剔除 system 的会话主体（AgentLoop::messages() 已跳过下标 0）。
    // user 走 setContent；assistant 段（含其后的 tool 结果）合入同一条流式气泡，
    // 依「正文 → 工具块」到达顺序镜像实时渲染。简化偏差：assistant 正文一次性成段
    // 输出后再接工具块（实时为交替），且 reasoning_content 不重放（恢复态不显思考块）。
    QHash<QString, QPair<QString, QString>> pendingToolCalls; // tool_call_id -> {工具名, 参数 JSON 串}
    for (const QJsonObject &msg : messages)
    {
        const QString role = msg.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("user"))
        {
            closeReplayBubble(); // 收束上一段 assistant
            addMessage(MessageBubbleWidget::Role::User,
                       msg.value(QStringLiteral("content")).toString());
            pendingToolCalls.clear();
            continue;
        }
        if (role == QLatin1String("assistant"))
        {
            const QJsonArray toolCalls = msg.value(QStringLiteral("tool_calls")).toArray();
            if (toolCalls.isEmpty())
            {
                // 终态回复（无工具调用）：独立流式气泡承载正文后收尾
                closeReplayBubble();
                m_currentBubble = new MessageBubbleWidget(MessageBubbleWidget::Role::Assistant, this);
                m_currentBubble->startStreaming();
                m_scrollView->getMainLayout()->addWidget(m_currentBubble);
                scrollToBottom();
                const QString content = msg.value(QStringLiteral("content")).toString();
                if (!content.isEmpty())
                    m_currentBubble->appendText(content);
                closeReplayBubble();
            }
            else
            {
                // 带工具的中间 assistant：惰性开气泡（tool 消息可能无正文），续写正文并登记工具调用
                if (!m_currentBubble)
                {
                    m_currentBubble = new MessageBubbleWidget(MessageBubbleWidget::Role::Assistant, this);
                    m_currentBubble->startStreaming();
                    m_scrollView->getMainLayout()->addWidget(m_currentBubble);
                    scrollToBottom();
                }
                const QString content = msg.value(QStringLiteral("content")).toString();
                if (!content.isEmpty())
                    m_currentBubble->appendText(content);
                for (const QJsonValue &c : toolCalls)
                {
                    const QJsonObject co = c.toObject();
                    const QJsonObject fn = co.value(QStringLiteral("function")).toObject();
                    pendingToolCalls.insert(
                        co.value(QStringLiteral("id")).toString(),
                        qMakePair(fn.value(QStringLiteral("name")).toString(),
                                  fn.value(QStringLiteral("arguments")).toString()));
                }
            }
            continue;
        }
        if (role == QLatin1String("tool"))
        {
            const QString callId = msg.value(QStringLiteral("tool_call_id")).toString();
            auto it = pendingToolCalls.find(callId);
            if (it == pendingToolCalls.end() || !m_currentBubble)
                continue; // 无配对/无气泡：跳过（理论上恢复历史已补齐配对，此为防御）
            const QString toolName = it.value().first;
            const QString argsStr = it.value().second;
            pendingToolCalls.erase(it);
            const QJsonObject args =
                QJsonDocument::fromJson(argsStr.toUtf8()).object();
            const QString summary = AgentLoop::toolSummaryOf(toolName, args);
            m_currentBubble->appendToolExecution(
                toolName, summary, msg.value(QStringLiteral("content")).toString());
            scrollToBottom();
            continue;
        }
    }
    closeReplayBubble(); // 收尾末段 assistant（含被中断的 tool_calls）
}

void ChatSessionPage::setModel(const QString &model)
{
    // 宿主注入初始模型（如新建会话页继承用户选择）：编辑器与后端同步设置，
    // 两条路径均不回环信号，无循环触发风险
    m_agentLoop->setModel(model);
    m_inputEdit->setCurrentModel(model);
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
    // lcc s12：stop() 不杀 cron 运行时，清史也不停调度器——durable 任务跨会话存活（勿在此停表）
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
    // 统一钳制消息列与底部输入组栏宽为 min(800, 可用宽)（纯布局 stretch 无法表达"撑到上限后居中"，
    // 与 NewChatPage 同款手法）；路径条随组宽变化触发 Resize，经 eventFilter 重新中间省略
    const int columnWidth = qMin(kColumnMaxWidth, width() - 2 * kSideMargin);
    if (m_scrollView)
        m_scrollView->setFixedWidth(columnWidth);
    if (m_inputSection)
        m_inputSection->setFixedWidth(columnWidth);

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

bool ChatSessionPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_workDirLabel && event->type() == QEvent::Resize)
        updateWorkDirDisplay();
    return BasePage::eventFilter(watched, event);
}

void ChatSessionPage::updateWorkDirDisplay()
{
    if (!m_workDirLabel || !m_agentLoop)
        return;
    const QString dir = m_agentLoop->workDir();
    m_workDirLabel->setToolTip(dir);                  // 全路径经 ToolTip 兜底
    const int w = m_workDirLabel->width();
    m_workDirLabel->setText(w > 0
                            ? QFontMetrics(m_workDirLabel->font()).elidedText(dir, Qt::ElideMiddle, w)
                            : dir);
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
