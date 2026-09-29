#include "ChatSessionPage.h"
#include <FluVScrollView.h>
#include <FluThemeUtils.h>
#include <FluUtils.h>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "AppSettings.h"
#include "ChatMsgEdit.h"
#include "AgentLoop.h"
#include "AgentConstants.h"
#include "CompactManager.h"
#include "ToolBlock.h"
#include "PermissionCard.h"
#include "SessionSidebar.h"
#include "TodoCard.h"
#include "LayoutConstants.h"
#include "ThemeAware.h"
#include "WorkDirPathBar.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QPair>

// 构造拆分①：主布局 + 滚动消息列 + 底部输入组（工作目录条 + ChatMsgEdit）。
// 纯搬移自原构造函数，摆位/参数/注释逐句不变；m_inputSection 入列提前到本段末尾——
// 构造期无布局 activate/show，widget 几何不受 addWidget 时机影响，行为等价
void ChatSessionPage::buildLayout()
{
    // 外层水平两列：左 = 原消息列 + 输入组竖排（整体搬入 leftColumn，参数逐句不变），
    // 右 = 会话侧栏。侧栏隐藏时 QBoxLayout 自动剔除其占位与间隙，左列回收全宽
    auto hRootLayout = new QHBoxLayout(this);
    hRootLayout->setContentsMargins(0, 0, 0, 0);
    hRootLayout->setSpacing(LayoutConst::kSidebarGap);
    setLayout(hRootLayout);

    auto leftColumn = new QWidget(this);
    auto vMainLayout = new QVBoxLayout(leftColumn);
    vMainLayout->setContentsMargins(LayoutConst::kSideMargin, 35, LayoutConst::kSideMargin, 35);
    vMainLayout->setSpacing(15);

    m_scrollView = new FluVScrollView(leftColumn);
    m_scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    m_scrollView->getMainLayout()->setContentsMargins(15, 15, 15, 15);
    m_scrollView->getMainLayout()->setSpacing(15);
    // 消息列与底部输入组同栏宽（resizeEvent 钳制 min(800, 可用宽)）并居中成同一阅读列
    vMainLayout->addWidget(m_scrollView, 1, Qt::AlignHCenter);

    // 底部输入区：只读工作目录条在上、ChatMsgEdit 在下，同栏同宽（与消息列同列，
    // 栏宽由 resizeEvent 钳制并居中；栏内子控件铺满栏宽，摆位关系不变）
    m_inputSection = new QWidget(leftColumn);
    auto sectionLayout = new QVBoxLayout(m_inputSection);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(8); // 与 NewChatPage 输入栏同参数，路径条与输入框读作同一组件

    // 工作目录页眉：只读展示，无浏览入口、不可修改。WorkDirPathBar 与 NewChatPage
    // 共用同一组件（「工作目录  <中间省略全路径>」排版、12px 次要灰字、Resize 重算
    // 省略、ToolTip 全路径兜底等细节见其实现），配色仍由本页三主题 QSS 的
    // QLabel#workDirCaption/#workDirPath 后代选择器命中
    m_workDirBar = new WorkDirPathBar(m_inputSection);
    sectionLayout->addWidget(m_workDirBar);

    m_inputEdit = new ChatMsgEdit(m_inputSection);
    sectionLayout->addWidget(m_inputEdit);

    vMainLayout->addWidget(m_inputSection, 0, Qt::AlignHCenter);

    hRootLayout->addWidget(leftColumn, 1);

    // 侧栏：固定宽纯信息面板（类 opencode），数据全部由本页接线经 setter 推入
    m_sidebar = new SessionSidebar(this);
    hRootLayout->addWidget(m_sidebar);
    connect(m_sidebar, &SessionSidebar::hideRequested, this,
            [this]() { setSidebarVisible(false); });

    // 收起后的浮动展开钮：右上角手动摆位（resizeEvent 跟随），与侧栏共用同一份
    // SessionSidebar.qss 取色（#sidebarRestoreBtn 选择器，按钮自身 bind 生效）
    m_restoreBtn = new QToolButton(this);
    m_restoreBtn->setObjectName(QStringLiteral("sidebarRestoreBtn"));
    m_restoreBtn->setFixedSize(LayoutConst::kSidebarRestoreBtnSize, LayoutConst::kSidebarRestoreBtnSize);
    m_restoreBtn->setIconSize(QSize(14, 14));
    m_restoreBtn->setCursor(Qt::PointingHandCursor);
    m_restoreBtn->setToolTip(tr("展开侧边栏"));
    connect(m_restoreBtn, &QToolButton::clicked, this,
            [this]() { setSidebarVisible(true); });
    ThemeAware::bind("SessionSidebar.qss", m_restoreBtn, [this]() {
        m_restoreBtn->setIcon(FluIconUtils::getFluentIconPixmap(
            FluAwesomeType::ChevronLeft, FluThemeUtils::getUtils()->getTheme(), 14, 14));
    });

    // 显隐偏好恢复（settings.ini 键 sidebarVisible，默认显示）；放在最后令首帧布局即按最终态钳宽
    setSidebarVisible(AppSettings::ini().value(QStringLiteral("sidebarVisible"), true).toBool());
}

ChatSessionPage::ChatSessionPage(const QString &sessionDataId, const QString &workDir,
                                 QWidget *parent) : BasePage(parent)
{
    buildLayout();

    // Agent Loop：真实模型回复 + 工具调用循环（流式打字机渲染）
    // 会话数据 ID + 工作目录经构造注入：前者令任务图/记忆/压缩转写/定时台账等落盘按会话隔离，
    // 后者令上述数据根、技能目录与 bash/子代理进程 cwd 全部随所选工作目录解析（空则回落进程当前目录）
    m_agentLoop = new AgentLoop(sessionDataId, workDir, this);
    // 模型切换接线：用户在下拉框改选 → 后端 setModel（下一轮请求生效）
    connect(m_inputEdit, &ChatMsgEdit::modelChanged, m_agentLoop, &AgentLoop::setModel);
    // 侧栏模型名联动（同一 modelChanged 第二订阅，不回环）
    connect(m_inputEdit, &ChatMsgEdit::modelChanged, this, [this](const QString &model) {
        m_sidebar->setSessionMeta(m_sessionTitle, model);
    });
    // 初始显示同步为后端生效模型（MODEL_ID 环境变量值不在两选项内时，下拉回落显示 qwen3.8-flash）
    m_inputEdit->setCurrentModel(m_agentLoop->model());
    // 工作目录在会话存续期固定（构造注入 AgentLoop，新建/恢复两条路径都在构造时传最终
    // 生效目录），故路径条只读一次快照；后续 Resize 重排仅对该快照重新省略，与组件契约一致
    m_workDirBar->setPath(m_agentLoop->workDir());

    // 侧栏初始快照：工作目录 + 会话元（标题占位「新会话」，首条用户消息后经
    // maybeCaptureSessionTitle 定稿）+ 后端生效模型
    m_sidebar->setWorkDir(m_agentLoop->workDir());
    m_sidebar->setSessionMeta(m_sessionTitle, m_agentLoop->model());

    // AgentLoop 输出信号 → UI 的接线整体搬移至 wireAgent()（纯移动，不改任何 lambda 逻辑）
    wireAgent();

    connect(m_inputEdit, &ChatMsgEdit::sendMessage, this, [this](const QString &text) {
        // 运行态预查：运行中不建气泡、不动旧现场。若照旧走 startAssistantStream，
        // 旧气泡会被先冻结、新气泡又被 run() 拒绝后的 error 链收掉置空，
        // 旧循环后续 delta 全部丢失、时间线撕裂（重入提示改由本 handler 独立气泡给出）。
        if (m_agentLoop->isRunning())
        {
            // 待决权限按拒绝放行队列（防死锁收口，含旧卡留痕，见 dismissPendingPermission）
            dismissPendingPermission();
            addMessage(MessageBubbleWidget::Role::Assistant,
                       tr("*Error:* Agent 仍在运行中，请等待完成后再发送"));
            return;
        }
        addMessage(MessageBubbleWidget::Role::User, text);
        startAssistantStream(text); // 创建流式气泡并启动代理循环
    });

    // 停止入口收口：runningChanged(true) 经 setTurnBusy 已把发送钮切成停止形态，
    // 点击回流本页 stop() → AgentLoop::stop() 既有终局链（子代理/权限/进程/流全收口，
    // 终局经 error("已停止。") 与 runningChanged(false) 复原钮形态），零后端改动
    connect(m_inputEdit, &ChatMsgEdit::stopRequested, this, &ChatSessionPage::stop);

    // 页面级 QSS：bind 完成首载与 themeChanged 联动。不再手工 connect
    // themeChanged→onThemeChanged——FluWidget 基类构造已连接并虚派发（重复连接
    // 曾致每次主题切换双份刷新）；气泡重 polish 作为 extraRefresh 挂在 QSS 重载后
    // （其 role 样式来自 ChatSessionPage.qss 的 #msgBrowser[role] 后代选择器，
    // 必须先有新主题级联再重 polish，相对时序与旧实现一致）
    ThemeAware::bind("ChatSessionPage.qss", this, [this]() {
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
    });
}

// 构造拆分②：AgentLoop 全部输出信号到 UI 的接线（自原构造函数纯搬移，lambda 逻辑逐句不变）
void ChatSessionPage::wireAgent()
{
    connect(m_agentLoop, &AgentLoop::finished, this, [this](const QString &reply) {
        // 防御收口：正常契约下待决权限会暂停队列、finished 不会先于裁决到达；
        // 若出现残留待决卡片，落为"已拒绝"留痕（不再转呼 resolvePermission，交给后端收口）
        if (m_permissionCard && !m_permissionCard->isResolved())
            m_permissionCard->resolveDenySilently();
        // 记忆相位气泡保留（异步化 P2，设计文档 §3.5a）：P2 新终局序下 finished 与
        // memoryPhaseStarted 同栈相邻发射，有流式气泡时此处**不**收尾定稿、不清槽位——
        // 定稿与挂 live 进度卡由紧随其后的 memoryPhaseStarted 处理段承接（并记录保留
        // 引用），气泡生命周期移交 memoryChainFinished 收口。消除旧序"finished 先清槽 →
        // 记忆链在途期间结果卡到达无气泡可挂 → 落独立噪声气泡兜底"的路径；
        // 无气泡（如前序 error 链已清槽位）仍走独立气泡兜底不丢回复
        if (!m_currentBubble)
            addMessage(MessageBubbleWidget::Role::Assistant, reply);
        // 侧栏收口：审批灯兜底回灭 + 回合终了重算上下文占用（勿接 textDelta 高频信号）
        m_sidebar->setPermissionPending(false);
        refreshContextUsage();
    });
    connect(m_agentLoop, &AgentLoop::error, this, [this](const QString &err) {
        // 后端收口：若仍待决权限（如挂起期间用户又发了消息 → run() 拒绝 → error），
        // 显式按拒绝放行队列并收旧卡（见 dismissPendingPermission），否则
        // m_awaitingPermission/m_running 永真导致会话死锁
        dismissPendingPermission();
        if (m_currentBubble)
        {
            m_currentBubble->finishStreaming();
            m_currentBubble = nullptr;
        }
        // 模板整体入 tr：英文译文恒等保留 "*Error:* %1"（Error 为气泡渲染约定的 markdown 前缀）
        addMessage(MessageBubbleWidget::Role::Assistant, tr("*Error:* %1").arg(err));
        // 侧栏收口：错误终局同样灭审批灯 + 重算占用（与 finished 对称）
        m_sidebar->setPermissionPending(false);
        refreshContextUsage();
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
                // 侧栏变更文件台账：write/edit 的 summary 即裸路径串（AgentLoop::toolSummary 契约），
                // 放在所有早退分支之前保证不漏记
                if (toolName == QLatin1String("write_file") || toolName == QLatin1String("edit_file"))
                    recordModifiedFile(toolName, summary);
                if (m_currentBubble)
                {
                    m_currentBubble->appendToolExecution(toolName, summary, output);
                    QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
                    return;
                }
                // 记忆结果卡特殊闸（审查 M1）：清屏/双回合交叠时 m_memoryBubble 已空，
                // 在途记忆链的结果卡若走下方兜底会在清空后的视图里造孤儿气泡——记忆数据
                // 已落盘（.memory/），此卡纯 UI 留痕，相位丢失时静默丢弃即可
                if (toolName == QLatin1String("memory") && !m_memoryBubble)
                    return;
                // 边界情况（无流式气泡，如信号在回合外到达）：独立气泡兜底，避免信息静默丢失
                 addMessage(MessageBubbleWidget::Role::Assistant,
                            tr("%1 %2:\n```\n%3\n```\n\n输出:\n```\n%4\n```")
                                .arg(ToolBlock::toolTitleText(toolName), toolName, summary, output));
             });

    // task 子代理实时进度行 → 时间线 task live 卡（AgentLoop 直连转发 SubAgent::progressEmitted）。
    // 无宿主气泡防御忽略：进度行是辅助展示，不像 toolOutputReady 那样兜底独立气泡
    //（回合外残帧极罕见——子代理随 cancelSubAgent 同步静默，仅 UI 事件排队深度造成瞬时错位）
    connect(m_agentLoop, &AgentLoop::subagentProgress, this,
            [this](int turnNo, const QString &toolName, const QString &summary) {
                if (!m_currentBubble)
                    return;
                m_currentBubble->appendSubagentProgress(turnNo, toolName, summary);
                QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
            });

    // 记忆沉淀相位开始（仅自然结束分支，P2 起为异步链启动前、与 finished 同栈相邻发射）：
    // 正文就地定稿 markdown 并在气泡时间线挂「记忆整理中…」live 进度卡；提取/合并结果卡
    // （toolOutputReady toolName="memory"）到达后就地切换为终态留痕。
    // 记录保留引用（§3.5a）：链在途期间该气泡不被 finished 清槽。多轮交叠语义：新回合
    // startAssistantStream 有「先收尾旧气泡」兜底，本引用被新相位覆盖时旧气泡已定稿、
    // QPointer 不悬空；结果卡跨窗落进新回合气泡属 §6-5 接受的边缘错位（窗口极窄：
    // 需用户在 ≤LLM 尾链期内开启新回合且旧链恰在覆盖后才发结果卡）
    connect(m_agentLoop, &AgentLoop::memoryPhaseStarted, this, [this]() {
        m_memoryBubble = m_currentBubble;
        if (m_currentBubble)
        {
            m_currentBubble->appendMemoryProgress();
            QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
        }
    });

    // 记忆链收口（P2）：补做被 finished 让渡的气泡定稿（MessageBubbleWidget::
    // finishStreaming 幂等——live 进度卡未转结果卡时收壳删除、文本段重复归档安全），
    // 随后释放保留引用；若新回合已抢占槽位则只收自己的账，不动 m_currentBubble
    // 已知限制（审查 L1，挂账观察）：pending 链经 AgentLoop singleShot(0) 重启时不重发
    // memoryPhaseStarted，其结果卡被上方 M1 闸静默丢弃（数据仍落盘）；极端交叠下旧链尾
    // 的 finished 可能提前定稿新相位气泡。修它需给两信号加世代参数，收益不配成本
    connect(m_agentLoop, &AgentLoop::memoryChainFinished, this, [this]() {
        if (!m_memoryBubble)
            return;
        m_memoryBubble->finishStreaming();
        if (m_currentBubble == m_memoryBubble)
            m_currentBubble = nullptr;
        m_memoryBubble = nullptr;
    });

    // 权限确认：工具即将执行但需用户裁决，后端队列暂停直至 resolvePermission。
    // 卡片挂进当前流式气泡的时间线（与工具块同一套约定：裁决留痕停在对应工具执行
    // 之前，后续工具块/正文出现在其后）；回合外兜底（无流式气泡）挂会话流末尾。
    // 信号契约：permissionRequired(toolName, summary, reason)，reason 为英文短句（卡片内转译中文）
    connect(m_agentLoop, &AgentLoop::permissionRequired, this,
            [this](const QString &toolName, const QString &summary, const QString &reason) {
                // 侧栏审批灯置亮（waiting 优先于 running 由侧栏内部归并）
                m_sidebar->setPermissionPending(true);
                // 防御：契约保证同一时刻至多一个待决；若残留未裁决旧卡直接丢弃（后端自行收口）
                if (m_permissionCard && !m_permissionCard->isResolved())
                    m_permissionCard->deleteLater();
                auto *card = new PermissionCard(this);
                connect(card, &PermissionCard::userResolved, this,
                        [this](bool allow) {
                            // 先灭灯再放行队列：resolvePermission 同步续跑若立刻再发
                            // permissionRequired，新灯的置亮不被本行覆盖
                            m_sidebar->setPermissionPending(false);
                            m_agentLoop->resolvePermission(allow);
                        });
                card->setPermissionRequest(toolName, summary, reason);
                m_permissionCard = card;
                if (m_currentBubble)
                    m_currentBubble->appendPermissionCard(card);
                else
                    m_scrollView->getMainLayout()->addWidget(card);
                QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
            });

    // 任务清单：时点快照留痕 —— 每次 todoUpdated（状态改变事件）在消息流当前位置嵌入
    // 一张冻结该时刻的折叠快照卡，不再维护「底部活卡」（最新态观看职能归右侧侧栏
    // 「任务清单」节）。与上一快照全同的更新（模型原表重写/幂等重试）不重复造卡；
    // 空列表是「计划清空」事件不嵌隐形卡（侧栏收口照发）。
    // 信号契约：todoUpdated(todos)，元素 {content, status: pending|in_progress|completed}
    connect(m_agentLoop, &AgentLoop::todoUpdated, this, [this](const QJsonArray &todos) {
        if (!todos.isEmpty() && todos != m_lastTodoSnapshot)
        {
            auto *card = new TodoCard(this);
            // 快照形态：构造即真折叠（initCollapsed），灌数据不起展开动画；用户点击
            // 头部展开按已测 m_fullContentHeight 动画到位（行高公式与宽度无关）
            card->setTodos(todos);
            m_lastTodoSnapshot = todos;
            // 延后一拍插入：todoUpdated 在 runTodoWrite handler 内先发，早于该
            // todo_write 调用的 toolOutputReady（同栈同步发射链），立即嵌卡会排在
            // 自家工具块之前；singleShot(0) 后工具块先落位，快照卡紧跟其后，
            // 符合「更新清单执行后直接嵌入」。有在途气泡则嵌其内部时间线
            // （后续思考/工具内容出现在卡下方，时序不打乱，所有权随附气泡）；
            // 回合间隙无气泡兜底插滚动布局末尾
            QPointer<MessageBubbleWidget> anchor = m_currentBubble;
            QTimer::singleShot(0, this, [this, card, anchor]() {
                if (anchor)
                    anchor->appendTimelineSection(card);
                else
                    m_scrollView->getMainLayout()->addWidget(card);
                QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
            });
        }
        // 侧栏任务清单同步（最新态观看，含清空事件；样式语义与快照卡状态点一致）
        m_sidebar->setTodos(todos);
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

    // 本会话输入侧禁用（异步化 P2 引入，设计文档 §3.5b）：runningChanged 覆盖整回合含 P1
    // 召回异步飞行期——旧输入禁用只在 startAssistantStream/finished 两端切换，召回段
    // （可达 120s）输入可发但必被 run() 卫兵拒绝弹错误提示，现提前到 setRunning(true)
    // 即禁、终局即放。P4 起为唯一输入禁用来源（原 ChatMsgEdit 内跨会话全局等待态网关
    // 随同步链清退一并删除）
    connect(m_agentLoop, &AgentLoop::runningChanged, this, [this](bool running) {
        if (m_inputEdit)
            m_inputEdit->setTurnBusy(running);
        // 侧栏状态灯：running 置亮/熄灭；终局（先于 finished 到达）兜底回灭审批灯
        m_sidebar->setRunning(running);
        if (!running)
            m_sidebar->setPermissionPending(false);
    });
}

// error 链与运行中 sendMessage 拒绝分支共用的待决权限收口（原两处逐句重复，抽取单源）：
// Gate2 MAJOR-1 约定——先捕获事发时的旧卡，resolvePermission(false) 同步续跑队列时，
// 同批下一个待询问调用可能就地再建"新卡"（handler 置 m_permissionCard）。只收旧卡、
// 新卡留给用户裁决，否则后端再次永久挂起且无卡可裁。无待决询问时 resolvePermission
// 的待决守卫使其成为 no-op，不会双重裁决；resolveDenySilently 不发 userResolved，
// 避免二次调用 resolvePermission。
void ChatSessionPage::dismissPendingPermission()
{
    QPointer<PermissionCard> staleCard = m_permissionCard;
    // 侧栏审批灯先灭再放行：resolvePermission(false) 同步续跑若立刻再建待决卡，
    // permissionRequired 接线会重新置亮，不被本行覆盖；无新待决则保持灭
    m_sidebar->setPermissionPending(false);
    m_agentLoop->resolvePermission(false);
    if (staleCard && !staleCard->isResolved())
        staleCard->resolveDenySilently();
}

void ChatSessionPage::addMessage(MessageBubbleWidget::Role role, const QString &content)
{
    // 会话标题单点捕获：startConversation/定时送达/历史重放的用户消息全部经过本函数
    if (role == MessageBubbleWidget::Role::User)
        maybeCaptureSessionTitle(content);
    auto bubble = new MessageBubbleWidget(role, this);
    bubble->setContent(content);
    m_scrollView->getMainLayout()->addWidget(bubble);
    scrollToBottom();
}

void ChatSessionPage::startAssistantStream(const QString &userText)
{
    // 兜底：上一轮未收到 finished 时收尾清场。m_currentBubble 已为 QPointer，
    // 下一行即赋新气泡，原此处的手工置空为死写入（被立即覆盖），随类型迁移一并去除；
    // 其余置空点（finished/error/closeReplayBubble）承担「关闭流式槽位」
    // 或「deleteLater 等待期立即隔离」语义，非冗余，保留
    if (m_currentBubble)
        m_currentBubble->finishStreaming();

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
    // 侧栏上下文占用基线：重放完成后按内存历史估算一次
    refreshContextUsage();
}

void ChatSessionPage::replayHistory(const QVector<QJsonObject> &messages)
{
    // messages 为剔除 system 的会话主体（AgentLoop::messages() 已跳过下标 0）。
    // user 走 setContent；assistant 段（含其后的 tool 结果）合入同一条流式气泡，
    // 依「思考 → 正文 → 工具块」到达顺序镜像实时渲染。简化偏差：assistant 正文一次性成段
    // 输出后再接工具块（实时为交替）；思考块仅重放内容（时长不入库，无时长终态标题）。
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
                // Replay the thinking block before the body text (content-only,
                // no duration is persisted with history)
                const QString reasoning = msg.value(QStringLiteral("reasoning_content")).toString();
                if (!reasoning.isEmpty())
                    m_currentBubble->appendHistoryThinkingText(reasoning);
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
                // Replay the thinking block before the body text (same as the
                // live per-round order: thinking → text → tool blocks)
                const QString reasoning = msg.value(QStringLiteral("reasoning_content")).toString();
                if (!reasoning.isEmpty())
                    m_currentBubble->appendHistoryThinkingText(reasoning);
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
            // 历史回放顺带重建侧栏变更文件台账（与实时链路同一提取语义）
            if (toolName == QLatin1String("write_file") || toolName == QLatin1String("edit_file"))
                recordModifiedFile(toolName, summary);
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
    // setCurrentModel 不发 modelChanged，宿主注入路径需自行同步侧栏模型名
    if (m_sidebar)
        m_sidebar->setSessionMeta(m_sessionTitle, model);
}

void ChatSessionPage::startConversation(const QString &text)
{
    addMessage(MessageBubbleWidget::Role::User, text);
    startAssistantStream(text);
}

void ChatSessionPage::scrollToBottom()
{
    // 先把挂起的布局变更结算，再读取 maximum——否则新增卡片（记忆进度卡/工具块等）
    // 后立刻调用时，滚动条 range 仍是旧值，滚不到真正底部。
    if (auto* vlayout = m_scrollView->getMainLayout())
        vlayout->activate();
    auto scrollBar = m_scrollView->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void ChatSessionPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);
    // 浮动展开钮跟随右上角（无论显隐都更新摆位，保证出现瞬间即对位）
    if (m_restoreBtn)
    {
        m_restoreBtn->move(width() - LayoutConst::kSideMargin - LayoutConst::kSidebarRestoreBtnSize,
                           LayoutConst::kSidebarRestoreTop);
    }
    applyColumnWidth();

    QTimer::singleShot(0, this, [this]() { applyColumnWidth(); });
}

QString ChatSessionPage::sessionDataId() const
{
    return m_agentLoop ? m_agentLoop->sessionDataId() : QString();
}

QString ChatSessionPage::sessionDataRoot() const
{
    return m_agentLoop ? m_agentLoop->sessionDataRoot() : QString();
}

bool ChatSessionPage::isRunning() const
{
    return m_agentLoop && m_agentLoop->isRunning();
}

void ChatSessionPage::stop()
{
    if (m_agentLoop)
        m_agentLoop->stop();
}

// 侧栏显隐切换单点：偏好落 settings.ini（键 sidebarVisible，默认显示）+ 浮动钮互斥 +
// 消息列重钳。构造期 buildLayout 尾部也走此函数完成首帧恢复
void ChatSessionPage::setSidebarVisible(bool visible)
{
    AppSettings::ini().setValue(QStringLiteral("sidebarVisible"), visible);
    m_sidebar->setVisible(visible);
    m_restoreBtn->setVisible(!visible);
    if (!visible)
    {
        // 浮动钮是页面直接子件且手动几何：布局不管理它，raise 保证盖过滚动区等兄弟
        m_restoreBtn->move(width() - LayoutConst::kSideMargin - LayoutConst::kSidebarRestoreBtnSize,
                           LayoutConst::kSidebarRestoreTop);
        m_restoreBtn->raise();
    }
    applyColumnWidth();
    // 切换不触发本页 resizeEvent：补一次延帧重钳（布局结算后气泡按新列宽重排）
    QTimer::singleShot(0, this, [this]() { applyColumnWidth(); });
}

// 消息列/输入组同栏宽钳制：min(800, 可用宽)，侧栏可见时扣除「宽+隙」整块
// （隐藏时 QBoxLayout 自动剔除该列与 spacing，左列占满，无扣除）；含气泡宽度刷新，
// resizeEvent 与 setSidebarVisible 共用
void ChatSessionPage::applyColumnWidth()
{
    const int sidebarReserve = (m_sidebar && !m_sidebar->isHidden())
        ? LayoutConst::kSidebarWidth + LayoutConst::kSidebarGap
        : 0;
    const int columnWidth = qMin(LayoutConst::kColumnMaxWidth,
                                 width() - 2 * LayoutConst::kSideMargin - sidebarReserve);
    if (m_scrollView)
        m_scrollView->setFixedWidth(columnWidth);
    if (m_inputSection)
        m_inputSection->setFixedWidth(columnWidth);

    if (!m_scrollView)
        return;
    auto mainLayout = m_scrollView->getMainLayout();
    for (int i = 0; i < mainLayout->count(); ++i)
    {
        auto bubble = qobject_cast<MessageBubbleWidget *>(mainLayout->itemAt(i)->widget());
        if (bubble)
            bubble->refreshSize();
    }
}

// 上下文占用快照 → 侧栏。口径对齐压缩管线：system 消息不计入会话占用
// （AgentLoop::messages() 含下标 0 的 system，防御式仅当首元素确为 system 才剔除）
void ChatSessionPage::refreshContextUsage()
{
    if (!m_agentLoop || !m_sidebar)
        return;
    QVector<QJsonObject> msgs = m_agentLoop->messages();
    if (!msgs.isEmpty() &&
        msgs.first().value(QStringLiteral("role")).toString() == QLatin1String("system"))
    {
        msgs.removeFirst();
    }
    m_sidebar->setContextUsage(CompactManager::estimateChars(msgs),
                               AgentConst::contextCharLimitValue());
}

// write_file/edit_file 变更登记（path 即 toolSummary 的裸路径语义），去重/排序由侧栏消化
void ChatSessionPage::recordModifiedFile(const QString &toolName, const QString &path)
{
    if (path.isEmpty() || !m_sidebar)
        return;
    m_sidebar->addModifiedFile(toolName == QLatin1String("write_file")
            ? QStringLiteral("write")
            : QStringLiteral("edit"),
        path);
}

// 抄宿主 LiteHarness 的标题派生规则（首条用户消息 simplified，超 12 字截断加省略号，
// 空回退「新会话」）；仅首次捕获生效——导航树重命名不回传本页面（已知限制）
void ChatSessionPage::maybeCaptureSessionTitle(const QString &userText)
{
    if (!m_sessionTitle.isEmpty())
        return;
    constexpr int kTitlePreviewChars = 12;
    QString title = userText.simplified();
    if (title.length() > kTitlePreviewChars)
        title = title.left(kTitlePreviewChars) + QStringLiteral("...");
    if (title.isEmpty())
        title = tr("新会话");
    m_sessionTitle = title;
    if (m_sidebar)
        m_sidebar->setSessionMeta(m_sessionTitle, m_agentLoop->model());
}
