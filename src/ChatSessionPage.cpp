#include "ChatSessionPage.h"
#include <FluVScrollView.h>
#include <FluThemeUtils.h>
#include <FluUtils.h>
#include <FluInfoBar.h>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "AppSettings.h"
#include "ChatMsgEdit.h"
#include "AgentLoop.h"
#include "AgentLoopInternal.h"
#include "AgentConstants.h"
#include "ToolNames.h"
#include "ToolBlock.h"
#include "PermissionCard.h"
#include "SessionSidebar.h"
#include "ChatStatusStrip.h"
#include "TodoCard.h"
#include "LayoutConstants.h"
#include "ThemeAware.h"
#include "WorkDirPathBar.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QPair>
#include <QStringList>

// 居中行容器：列 widget 以 stretch 1 铺满行，两侧留白由钳制函数手动写入布局边距，
// 列宽靠 maximumWidth 封顶（宽窗定宽居中、窄窗吃满）。水平 Ignored 策略令本行
// 不参与父布局 minimumSize 计算，断开「边距/列固有最小宽 → 上传窗口最小宽」的
// 收缩棘轮（旧 setFixedWidth + AlignHCenter 会把推荐最小宽顶成窗口地板）
static QWidget *makeCenteringRow(QWidget *column, QWidget *parent, QHBoxLayout **outLayout)
{
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    rowLayout->addWidget(column, 1);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    *outLayout = rowLayout;
    return row;
}

// P1-9 通知条自动淡出时长：错误 8s / 换模 6s。取这两个数是因为条内文本是「一句可读完
// 的短说明」（错误详情另在气泡里全文留痕），8s 足够读完又不至于长期挡阅读列；
// 换模是状态告知而非需要裁决的事，比错误更短一档。重试条不在此列——它常驻到首个增量
// 到达（见 clearRetryNotice），因为「还在退避等待」这件事本身就是要持续可见的状态
constexpr int kNoticeErrorMs = 8000;
constexpr int kNoticeInfoMs = 6000;

// 构造拆分①：主布局 + 滚动消息列 + 底部输入组（工作目录条 + ChatMsgEdit + 其下的
// ChatStatusStrip 状态条）。
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
    // 页面留白与栏内间隙单源（P2-D3）：垂直 35 与 SettingsPage 同源，15 与输入框工具集行同源
    vMainLayout->setContentsMargins(LayoutConst::kSideMargin, LayoutConst::kPageVMargin,
                                    LayoutConst::kSideMargin, LayoutConst::kPageVMargin);
    vMainLayout->setSpacing(LayoutConst::kColGap);

    // P1-9 通知条：消息列正上方的非阻塞 FluInfoBar（重试态/换模/错误三类即时提示）。
    // 刻意不用 FluentUI 的 FluInfoBarMgr 浮层——那是绝对定位的 toast，与本页「列宽手动
    // 钳制 + Ignored 行容器」的构图纪律冲突（脱栏、随窗口缩放压到侧栏上）；进布局的常驻
    // 条天然同栏，且关闭时 setIsOpen(false) 自带淡出并 hide()，隐藏件不占布局空间。
    m_noticeBar = new FluInfoBar(FluInfoBarSeverity::Informational, leftColumn);
    m_noticeBar->setObjectName(QStringLiteral("sessionNotice"));
    // FluInfoBar 构造写死 minimumWidth 320：最小窗（900）下消息列可用宽仅 ~282，
    // 320 地板会把条撑出栏外。本条宽度全权交给 applyColumnWidth 钳制，地板拆掉。
    m_noticeBar->setMinimumWidth(0);
    m_noticeBar->setIsClosable(true);
    m_noticeBar->setIsOpen(false);
    // FluInfoBar 构造里 m_isOpen 初值即 false，上面那句会走「状态相同早退」分支，
    // 永远触发不了淡出末尾的 hide()——而构造器自己也不 hide，子 widget 默认可见，
    // 于是从未弹过通知的新会话页面会挂一条空内容的条在顶部。显式收口一次；
    // 后续 showNotice 走 setIsOpen(true) 自带 show()，不受此 hide 影响。
    m_noticeBar->hide();
    m_noticeRow = makeCenteringRow(m_noticeBar, leftColumn, &m_noticeRowLayout);
    vMainLayout->addWidget(m_noticeRow);

    // 通知条自动淡出定时器（singleShot，时长由 showNotice 逐次设定）：0/负数＝常驻，
    // 由下一次通知或 clearRetryNotice 收口——重试态属前者（成功即被首个增量关掉），
    // 错误/换模属后者（一次性告知，读完就该自己走，不留残条挡阅读列）
    m_noticeDismissTimer = new QTimer(this);
    m_noticeDismissTimer->setSingleShot(true);
    connect(m_noticeDismissTimer, &QTimer::timeout, this, &ChatSessionPage::hideNotice);

    m_scrollView = new FluVScrollView(leftColumn);
    m_scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    // 栏内间隙单源（P2-D3）：此前 15 在本页两处与 ChatMsgEdit 工具集行各抄一遍
    m_scrollView->getMainLayout()->setContentsMargins(LayoutConst::kColGap, LayoutConst::kColGap,
                                                      LayoutConst::kColGap, LayoutConst::kColGap);
    m_scrollView->getMainLayout()->setSpacing(LayoutConst::kColGap);

    // P1-9 空态引导卡：消息列首个件，空会话时给出三条用法锚点（能问什么 / 怎么停止 /
    // 侧栏有什么），任何内容入列即永久收起。刻意做成布局内件而非浮层：浮层要随滚动区
    // 视口手动摆位（同 m_jumpBottomBtn 那套），而引导文本需要随阅读列宽换行，进布局
    // 即白拿 applyColumnWidth 的栏宽钳制与三主题 QSS 取色
    m_emptyHint = new QWidget(m_scrollView);
    m_emptyHint->setObjectName(QStringLiteral("emptyHint"));
    // 自定义 QWidget 子类默认不渲染 QSS 盒模型（背景/边框），必须显式开启（同 ChatStatusStrip）
    m_emptyHint->setAttribute(Qt::WA_StyledBackground, true);
    auto hintLayout = new QVBoxLayout(m_emptyHint);
    hintLayout->setContentsMargins(18, 16, 18, 16);
    hintLayout->setSpacing(6);
    auto hintTitle = new QLabel(tr("这个会话还没有消息"), m_emptyHint);
    hintTitle->setObjectName(QStringLiteral("emptyHintTitle"));
    hintLayout->addWidget(hintTitle);
    // 三条锚点各占一行：合成一段会被读成一句广告，分行才读作「可以做的三件事」
    const QStringList hintLines = {
        tr("直接提问即可——我能读改本目录文件、执行命令、跑测试并逐项汇报结果。"),
        tr("回合运行中，发送钮会切成停止形态，点一下即可中止当前回合。"),
        tr("右侧信息面板常驻上下文占用、任务清单与团队状态，可收起，收起后输入框下方仍保留计量读数。"),
    };
    for (const QString &line : hintLines)
    {
        auto *item = new QLabel(QStringLiteral("· ") + line, m_emptyHint);
        item->setObjectName(QStringLiteral("emptyHintLine"));
        item->setWordWrap(true);
        hintLayout->addWidget(item);
    }
    m_scrollView->getMainLayout()->addWidget(m_emptyHint);
    // 消息列与底部输入组同栏宽（applyColumnWidth 钳 max(800, 可用宽)），经居中行
    // 容器手动留白成同一阅读列（不再 AlignHCenter+setFixedWidth，修窗口收缩棘轮）
    vMainLayout->addWidget(makeCenteringRow(m_scrollView, leftColumn, &m_scrollRowLayout), 1);

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

    // 输入框下方状态条：承载轮步/消耗/速度三段文字（纯视图，数据仍由本页接线经 setter
    // 推入；侧栏收起时照样显示——这是它留在输入组下方的理由）。
    // 占用**条**与占用**数字**、「运行状态」三态灯都在侧栏：条与数字是同一读数的两种画法，
    // 一次 usageRatio 同时喂两处（数字随条迁入面板是用户裁决，代价是收起侧栏时一并不可见，
    // 见 SessionSidebar.h / ChatStatusStrip.h）
    m_statusStrip = new ChatStatusStrip(m_inputSection);
    sectionLayout->addWidget(m_statusStrip);

    vMainLayout->addWidget(makeCenteringRow(m_inputSection, leftColumn, &m_inputRowLayout), 0);

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

    // 「回到底部」浮动钮（评审 B1）：用户上翻离开底部后常驻滚动区右下角，
    // 点击=强制贴底并恢复跟随；挂 m_scrollView 本体（非视口），浮于内容之上
    m_jumpBottomBtn = new QToolButton(m_scrollView);
    m_jumpBottomBtn->setObjectName(QStringLiteral("jumpBottomBtn"));
    m_jumpBottomBtn->setFixedSize(LayoutConst::kJumpBottomBtnSize, LayoutConst::kJumpBottomBtnSize);
    m_jumpBottomBtn->setIconSize(QSize(14, 14));
    m_jumpBottomBtn->setCursor(Qt::PointingHandCursor);
    m_jumpBottomBtn->setToolTip(tr("回到底部"));
    m_jumpBottomBtn->setVisible(false);
    connect(m_jumpBottomBtn, &QToolButton::clicked, this,
            [this]() { scrollToBottom(); });

    // 显隐偏好恢复（config.json 键 sidebarVisible，默认显示）；放在最后令首帧布局即按最终态钳宽
    setSidebarVisible(AppSettings::value(QStringLiteral("sidebarVisible"), true).toBool());
}

ChatSessionPage::ChatSessionPage(const QString &sessionDataId, const QString &workDir,
                                 QWidget *parent) : BasePage(parent)
{
    buildLayout();

    // 滚动合并定时器（效率 P5）：流式期间 QOpenAi 每帧 SSE 都发一次 textDelta/thinkingDelta，
    // 每帧直调 scrollToBottom 就是每帧对整个消息列做一遍完整布局 pass（vlayout->activate()）
    // 加滚动条写值——主线程最重且最频繁的冗余。此处以 30ms singleShot 窗口把窗口内的多次
    // 滚动请求折叠成一次真实滚动（量级与 MessageBubbleWidget 流式测量节流 50ms 对齐，
    // 气泡自身先按 50ms 结算尺寸，本页 30ms 采点不会漏掉最终态）。
    // 一次性路径（用户发送/新气泡/历史回放/终态）仍直调 scrollToBottom 保持零延迟。
    m_scrollCoalesceTimer = new QTimer(this);
    m_scrollCoalesceTimer->setInterval(30);
    m_scrollCoalesceTimer->setSingleShot(true);
    // 超时走跟随入口（评审 B1）：用户上翻离开底部时不拽回，只浮出回底钮
    connect(m_scrollCoalesceTimer, &QTimer::timeout, this, &ChatSessionPage::followScrollToBottom);

    // 用户滚动意图探测：拖滑块走 sliderMoved（程序化 setValue 不发此信号，天然区分），
    // 滚轮经视口事件过滤器捕获——两路都只重算跟随态
    if (auto *bar = m_scrollView->verticalScrollBar())
    {
        connect(bar, &QScrollBar::sliderMoved, this, [this](int) { evaluateFollowState(); });
        m_scrollView->viewport()->installEventFilter(this);
    }

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
        // 回底钮箭头随主题重着色（钮本体样式走页面级 #jumpBottomBtn 后代选择器）
        if (m_jumpBottomBtn)
            m_jumpBottomBtn->setIcon(FluIconUtils::getFluentIconPixmap(
                FluAwesomeType::ChevronDown, FluThemeUtils::getUtils()->getTheme(), 14, 14));
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
        // 侧栏收口：审批灯兜底回灭 + 终局再算一次占用（完整刷新时机见 refreshContextUsage 注释）
        m_sidebar->setPermissionPending(false);
        refreshContextUsage();
        // 终态强制结算滚动（效率 P5）：取消在途合并窗口立即贴底。记忆相位分支下
        // memoryPhaseStarted 紧随同栈到达，其进度卡另起窗口、由 memoryChainFinished 再结算
        flushPendingScroll();
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
        // P1-9 通知条：气泡是**留痕**（进时间线、会被滚走），通知条是**即时提示**——
        // 二者互补而非替代：只留通知条会让历史里看不到失败，只留气泡则用户上翻回看时
        // 完全不知道刚发生了什么。用户主动停止的终局不走本档（见 stop()）。
        if (m_stopRequested)
            hideNotice(); // 停止前若挂着重试条，一并收掉：停都停了，别再报「正在重试」
        else
            showNotice(NoticeLevel::Error, tr("本轮失败"), err, kNoticeErrorMs);
        m_stopRequested = false;
        // 侧栏收口：错误终局同样灭审批灯 + 重算占用（与 finished 对称）
        m_sidebar->setPermissionPending(false);
        refreshContextUsage();
        // 错误终局（含用户 stop 的「已停止。」回流）：取消在途合并窗口并立即贴底
        flushPendingScroll();
    });
    // s17 goal 回显（port ② 裁决）：goalNotice 载引擎英文契约串（/goal 状态·清除回显与
    // achieved/failed/limit/error 终局回显），仿 error 气泡先例落本地临时气泡——
    // 不入 AgentLoop::m_messages、不入 history.json（persistHistory 只序列化 m_messages）；
    // 串为 GoalRules 禁翻区原文，宿主侧无新增 tr() 词条
    connect(m_agentLoop, &AgentLoop::goalNotice, this, [this](const QString &text) {
        // s17 Gate2 MAJOR-A：以 isRunning() 区分两形。false=零模型回显形（run() 同栈早退、
        // 尚未 setRunning；startAssistantStream 刚建的「处理中…」占位整拍没有任何事件到达）
        // ——先 deleteLater 移除占位泡再回显，**禁 finishStreaming**（会把「处理中…」定稿
        // 成永久静态泡、随下次发送悬空累积）。true=终局回显形（挂起评估在途、回合仍活着）
        // ——不碰真回复气泡，提前定稿会让 finished 兜底路径把回复发双份
        if (!m_agentLoop->isRunning() && m_currentBubble)
        {
            m_currentBubble->deleteLater();
            m_currentBubble = nullptr;
        }
        addMessage(MessageBubbleWidget::Role::Assistant, text);
        flushPendingScroll();
    });
    connect(m_agentLoop, &AgentLoop::thinkingDelta, this, [this](const QString &delta) {
        clearRetryNotice(); // 重试已自愈：首个增量即证据，条再挂着会读成「还在等」
        if (m_currentBubble)
        {
            m_currentBubble->appendThinkingText(delta);
            // 每帧 SSE 思考增量都请求贴底 → 走 30ms 合并窗口（效率 P5）
            requestScrollToBottom();
        }
    });
    connect(m_agentLoop, &AgentLoop::textDelta, this, [this](const QString &delta) {
        clearRetryNotice(); // 同上：正文增量也是重试成功的直接证据
        if (m_currentBubble)
        {
            m_currentBubble->appendText(delta);
            // 每帧 SSE 正文增量都请求贴底 → 走 30ms 合并窗口（效率 P5，头号 UI 卡顿点）
            requestScrollToBottom();
        }
    });
    // 工具执行可视化：按到达顺序内嵌到当前流式气泡的时间线中（正文与工具块交替出现）。
    // 信号契约：toolOutputReady(toolName, summary, output, ok)，summary 为关键参数，
    // ok = AgentLoop::isToolFailure 取反的成败判定（B1 单源，UI 不再嗅探输出）
    connect(m_agentLoop, &AgentLoop::toolOutputReady, this,
            [this](const QString &toolName, const QString &summary, const QString &output, bool ok) {
                // 占用条随手补刷一次：延后一拍是因为 tool 结果此刻还压在 m_toolResultsReady，
                // 同栈的 runNextTool 批尾才回填历史（compact 卡片到达时历史也已替换完）
                QTimer::singleShot(0, this, [this]() { refreshContextUsage(); });
                if (m_currentBubble)
                {
                    m_currentBubble->appendToolExecution(toolName, summary, output, ok);
                    // 在途工具终态卡：原延一帧（singleShot(0)）滚底，现统一走 30ms 合并窗口
                    // （窗口到期本就晚于一帧，延帧语义被包含，效率 P5）
                    requestScrollToBottom();
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
                                .arg(ok ? ToolBlock::toolTitleText(toolName)
                                        : ToolBlock::toolFailText(toolName),
                                     toolName, summary, output));
             });

    // 工具开始执行 → 事前 live 卡（AgentLoop 经权限门放行时发射，B3）。
    // 无宿主气泡防御忽略：事前卡是终态卡（toolOutputReady 兜底链）的进度前戏，
    // 回合外到达无时间线可挂、终态照常留痕不丢信息
    connect(m_agentLoop, &AgentLoop::toolStarted, this,
            [this](const QString &toolName, const QString &summary) {
                if (!m_currentBubble)
                    return;
                m_currentBubble->appendToolStart(toolName, summary);
                // 事前 live 卡：改走合并窗口（一回合内多工具调用成串到达，逐条 activate 冗余）
                requestScrollToBottom();
            });

    // task 子代理实时进度行 → 时间线 task live 卡（AgentLoop 直连转发 SubAgent::progressEmitted）。
    // 无宿主气泡防御忽略：进度行是辅助展示，不像 toolOutputReady 那样兜底独立气泡
    //（回合外残帧极罕见——子代理随 cancelSubAgent 同步静默，仅 UI 事件排队深度造成瞬时错位）
    connect(m_agentLoop, &AgentLoop::subagentProgress, this,
            [this](int turnNo, const QString &toolName, const QString &summary) {
                if (!m_currentBubble)
                    return;
                m_currentBubble->appendSubagentProgress(turnNo, toolName, summary);
                // 子代理进度行按轮次高频刷屏：改走合并窗口（效率 P5）
                requestScrollToBottom();
            });

    // s13 观测面 a：队友活动 → 按名活卡 live 卡（AgentLoop 转发 TeammateRuntime 展示信号，
    // turn/result/error/idle_notification 数据 token 在 ToolBlock 内映射中文）。首活动经
    // ensureTeammateCard 建卡挂当时气泡，后续活动按名回挂原卡——队友回合不随 Lead 回合终结，
    // 活卡跨 Lead 回合存续（刻意不占 m_liveTaskBlock 单槽，finishStreaming 不清扫）。
    // idle_notification 不建卡（零活动可叙的开场不值得插卡扰动），只注记已有卡。
    connect(m_agentLoop, &AgentLoop::teammateProgress, this,
            [this](const QString &teammateName, const QString &type, const QString &content) {
                auto it = m_liveTeammateCards.find(teammateName);
                if (it == m_liveTeammateCards.end() || !it.value())
                {
                    if (it != m_liveTeammateCards.end())
                        m_liveTeammateCards.erase(it);   // 卡被异常销毁后的空位
                    if (type == QLatin1String("idle_notification") || !m_currentBubble)
                        return;
                    ToolBlock *card = m_currentBubble->ensureTeammateCard(teammateName);
                    if (!card)
                        return;
                    it = m_liveTeammateCards.insert(teammateName, card);
                }
                it.value()->appendTeammateProgress(type, content);
                requestScrollToBottom();
            });

    // P8 需求2：逐工具活动行 → 同一张按名活卡的既有子代理式滑窗链（ToolBlock::
    // appendSubagentProgress 复用，行形「第 N 轮 · 工具名  摘要」，零新文案）。建卡/
    // 回挂/空位清理纪律与上方事件行完全同款；completed 终局后队友若继续领活，
    // 同名新活动自然另起新卡（settle 已把表位出清）。
    connect(m_agentLoop, &AgentLoop::teammateToolProgress, this,
            [this](const QString &teammateName, int turnNo, const QString &toolName,
                   const QString &summary) {
                auto it = m_liveTeammateCards.find(teammateName);
                if (it == m_liveTeammateCards.end() || !it.value())
                {
                    if (it != m_liveTeammateCards.end())
                        m_liveTeammateCards.erase(it);   // 卡被异常销毁后的空位
                    if (!m_currentBubble)
                        return;
                    ToolBlock *card = m_currentBubble->ensureTeammateCard(teammateName);
                    if (!card)
                        return;
                    it = m_liveTeammateCards.insert(teammateName, card);
                }
                it.value()->appendSubagentProgress(turnNo, toolName, summary);
                requestScrollToBottom();
            });

    // 队友终态三通道（completed/exited/settled）→ 就地冻结收壳。收口不滚动：旧卡终态
    // 可能远晚于其位置（跨回合），强滚会把正在回看历史的用户视野粗暴拉走
    connect(m_agentLoop, &AgentLoop::teammateSettled, this,
            [this](const QString &teammateName, const QString &outcome) {
                settleTeammateCard(teammateName, outcome);
            });

    // s13 观测面 b：名册变更边沿广播 → 侧栏队友节（setter 推入的纯视图纪律不变）
    connect(m_agentLoop, &AgentLoop::teamRosterChanged, this, [this]() {
        m_sidebar->setTeammates(m_agentLoop->teammateRoster());
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
            // 记忆进度 live 卡：改走合并窗口（后续结果卡/定稿仍会贴底，终态由 memoryChainFinished 强制结算）
            requestScrollToBottom();
        }
    });

    // 记忆链收口（P2）：补做被 finished 让渡的气泡定稿（MessageBubbleWidget::
    // finishStreaming 幂等——live 进度卡未转结果卡时收壳删除、文本段重复归档安全），
    // 随后释放保留引用；若新回合已抢占槽位则只收自己的账，不动 m_currentBubble
    // 已知限制（审查 L1，挂账观察）：pending 链经 AgentLoop singleShot(0) 重启时不重发
    // memoryPhaseStarted，其结果卡被上方 M1 闸静默丢弃（数据仍落盘）；极端交叠下旧链尾
    // 的 finished 可能提前定稿新相位气泡。修它需给两信号加世代参数，收益不配成本
    connect(m_agentLoop, &AgentLoop::memoryChainFinished, this, [this]() {
        // 记忆链真正的终态：先收尾保留气泡，再无条件强制结算滚动（链尾结果卡/定稿刚走合并
        // 窗口，此处立即贴底防停在半截）。原「无保留气泡早退」改为条件体，行为等价
        if (m_memoryBubble)
        {
            m_memoryBubble->finishStreaming();
            if (m_currentBubble == m_memoryBubble)
                m_currentBubble = nullptr;
            m_memoryBubble = nullptr;
        }
        flushPendingScroll();
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
                            // 键盘可达（P1-7）：裁决后本卡两钮禁用、焦点悬空无落点。
                            // 回合多半仍在继续（输入框禁用，focusInput 会主动跳过），
                            // 故焦点交给停止钮——此刻唯一可交互控件，Enter 即中止。
                            // 若 resolvePermission 同步把回合直接推到终局，runningChanged(false)
                            // 那条链随后会 focusInput 把焦点收回输入框，两者不冲突（后者覆盖）。
                            if (m_agentLoop->isRunning() && m_inputEdit)
                                m_inputEdit->focusSendButton();
                        });
                card->setPermissionRequest(toolName, summary, reason);
                m_permissionCard = card;
                if (m_currentBubble)
                    m_currentBubble->appendPermissionCard(card);
                else
                    m_scrollView->getMainLayout()->addWidget(card);
                // 审批卡属在途时间线插卡（最多晚 30ms 贴底，肉眼无感）：走合并窗口
                requestScrollToBottom();
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
                // 外层 singleShot(0) 的「延一帧插卡」时序语义保持不变，仅把其内的延帧滚底
                // 换为合并窗口（到期本就晚于一帧）
                requestScrollToBottom();
            });
        }
        // 侧栏任务清单同步（最新态观看，含清空事件；样式语义与快照卡状态点一致）
        m_sidebar->setTodos(todos);
    });

    // s17 P1 观测面三广播 → 侧栏直连（载荷即引擎快照，仿 todoUpdated 事件即数据形态；
    // 侧栏保持纯视图不读引擎。context 收口在 m_sidebar，析构自动断连）
    connect(m_agentLoop, &AgentLoop::backgroundTasksChanged, m_sidebar,
            &SessionSidebar::setBackgroundTasks);
    connect(m_agentLoop, &AgentLoop::cronJobsChanged, m_sidebar,
            &SessionSidebar::setCronJobs);
    connect(m_agentLoop, &AgentLoop::goalStateChanged, m_sidebar,
            &SessionSidebar::setGoalState);

    // 定时任务送达（lcc s12）：后端空闲 tick 交付——展示走带前缀文本，活跃请求走无原文本
    //（lcc deliver 双形态）；复用用户发消息的既有两步链路。首行 isRunning 防御分支
    // 同栈直连下理论不可达（tryDeliverCron 已查 m_running），保留作后端契约变动的保险
    connect(m_agentLoop, &AgentLoop::scheduledUserMessage, this,
            [this](const QString &displayText, const QString &activeRequestText) {
        if (m_agentLoop->isRunning())
            return;
        // lcc s15 af26862 bg 唤醒空回合（第 4 事件源）：displayText 为空则不建用户气泡——
        // addMessage 无条件 new 气泡且联动标题捕获，空串会留空气泡、并把会话标题提前定稿为
        // 「新会话」并锁死 maybeCaptureSessionTitle（真实首条用户消息将失去标题派生）。
        // 收割内容由 run() 圈顶 injectBackgroundResults 注入历史，UI 无需用户气泡
        // （对照 lcc bg 分支空 prompt 不注入 user 消息）；cron/team 唤醒文本非空走原路径
        if (!displayText.isEmpty())
            addMessage(MessageBubbleWidget::Role::User, displayText);
        startAssistantStream(activeRequestText);
    });

    // 本会话输入侧禁用（异步化 P2 引入，设计文档 §3.5b）：runningChanged 覆盖整回合含 P1
    // 召回异步飞行期——旧输入禁用只在 startAssistantStream/finished 两端切换，召回段
    // （可达 120s）输入可发但必被 run() 卫兵拒绝弹错误提示，现提前到 setRunning(true)
    // 即禁、终局即放。P4 起为唯一输入禁用来源（原 ChatMsgEdit 内跨会话全局等待态网关
    // 随同步链清退一并删除）
    connect(m_agentLoop, &AgentLoop::runningChanged, this, [this](bool running) {
        // 导航徽章联动（评审 B3）：运行态广播宿主，后台会话在导航列挂 ●/✓
        emit navRunningChanged(running);
        if (m_inputEdit)
            m_inputEdit->setTurnBusy(running);
        // 侧栏状态灯：running 置亮/熄灭；终局（先于 finished 到达）兜底回灭审批灯
        m_sidebar->setRunning(running);
        if (!running)
        {
            m_sidebar->setPermissionPending(false);
            // 键盘可达（评审 B2）：权限卡裁决后钮禁用、焦点悬空无落点——回合终局输入解禁时，
            // 若本页正是前台活动页则把焦点交还输入框，Enter 续发不断链
            if (isVisible() && window() && window()->isActiveWindow() && m_inputEdit)
                m_inputEdit->focusInput();
            // s13 兜底：Lead 回合结束时清算「已不在名册却还挂着的活卡」（终态广播
            // 漏发的幽灵位）。仍在名册的队友保卡——队友回合不随 Lead 回合终结，
            // 误冻结会让续跑活动凭空多出一张新卡（跨回合存续偏差已登记）
            const QList<QPair<QString, QString>> roster = m_agentLoop->teammateRoster();
            const QStringList liveNames = m_liveTeammateCards.keys();
            for (const QString &name : liveNames)
            {
                bool alive = false;
                for (const auto &entry : roster)
                {
                    if (entry.first == name)
                    {
                        alive = true;
                        break;
                    }
                }
                if (!alive)
                    settleTeammateCard(name, QStringLiteral("settled"));
            }
        }
    });

    // P1-9 观测面：传输层退避重试对 UI 此前完全不可见——请求发出后界面静默 1s/2s/4s…
    // （上限 64s，maxRetries 默认 2 时累计约 3s，配置拉满 5 次可达 31s），用户看到的
    // 与「卡死了」毫无区别。引擎单源发射（scheduleRetry 一处覆盖 idle 超时与 429/5xx
    // 两条路径），本页只翻译成人话，不自算任何退避时序。
    connect(m_agentLoop, &AgentLoop::streamRetryScheduled, this,
            [this](int attempt, int totalAttempts, int delayMs) {
        showNotice(NoticeLevel::Warning, tr("正在重试"),
                   tr("第 %1/%2 次重试，%3 秒后重新发送（服务限流或无响应）。")
                       .arg(attempt)
                       .arg(totalAttempts)
                       .arg(delayMs / 1000),
                   0); // 常驻：成功由首个增量收口，失败由错误通知覆盖，不留残条也不早退
    });
    // 529 过载换模（s15）：引擎只打 LOG_WARN，用户看不到「这轮答案来自另一个模型」——
    // 与输入框下拉仍显示原模型名构成两个读数，故在此显形。下拉不改（生效模型仍由后端
    // model() 单源决定，换模是回合内粘滞、不写配置）
    connect(m_agentLoop, &AgentLoop::streamFallbackModel, this, [this](const QString &model) {
        showNotice(NoticeLevel::Info, tr("已切换到备用模型"),
                   tr("服务端过载，本轮改用 %1 继续。").arg(model), kNoticeInfoMs);
    });
}

// s13 观测面：按名收口活卡（置终态 + 除表，幂等——三路 settle/兜底重复到达 no-op）
void ChatSessionPage::settleTeammateCard(const QString &teammateName, const QString &outcome)
{
    QPointer<ToolBlock> card = m_liveTeammateCards.take(teammateName);
    if (!card)
        return;
    card->finishTeammateLive(outcome);
    if (auto *bubble = qobject_cast<MessageBubbleWidget *>(card->parentWidget()))
        bubble->refreshSize();
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

// P1-9 通知条三件套（show/hide/clearRetry）：一条常驻件承载三类即时提示，
// 状态位只有一位「当前是不是重试条」——它决定收口时机（重试条由首个增量自发关闭，
// 其余由淡出定时器或下一条通知关闭），其余差异都在展示文案里，无需更多状态。
void ChatSessionPage::showNotice(NoticeLevel level, const QString &title,
                                 const QString &message, int autoDismissMs)
{
    if (!m_noticeBar)
        return;
    // 枚举→FluentUI 档位单点映射：页面 API 面不暴露 FluInfoBarSeverity（见 .h 声明处），
    // 取色/图标全在 FluentUI 自带 FluInfoBar.qss 的 severity 选择器里，本仓零新增 QSS
    m_noticeBar->setSeverity(level == NoticeLevel::Info    ? FluInfoBarSeverity::Informational
                             : level == NoticeLevel::Warning ? FluInfoBarSeverity::Warning
                                                             : FluInfoBarSeverity::Error);
    m_noticeBar->setTitle(title);
    m_noticeBar->setMessage(message);
    m_noticeIsRetry = (level == NoticeLevel::Warning);
    m_noticeBar->setIsOpen(true);
    if (autoDismissMs > 0)
        m_noticeDismissTimer->start(autoDismissMs);
    else
        m_noticeDismissTimer->stop();
    // 条从「无内容」到「有内容」会改整列高度：延帧重钳一次栏宽并贴底，
    // 否则首帧可能出现条宽未跟上限、或新条把当前视野顶走而滚动位置没跟上
    QTimer::singleShot(0, this, [this]() {
        applyColumnWidth();
        followScrollToBottom();
    });
}

void ChatSessionPage::hideNotice()
{
    if (!m_noticeBar)
        return;
    m_noticeDismissTimer->stop();
    m_noticeBar->setIsOpen(false); // 自带 250ms 淡出，动画末尾 hide() → 布局剔除占位
    m_noticeIsRetry = false;
}

void ChatSessionPage::clearRetryNotice()
{
    // 只收重试条：错误/换模通知正挂着时被增量事件顺手关掉，等于把「本轮失败」藏了
    if (m_noticeIsRetry)
        hideNotice();
}

void ChatSessionPage::dismissEmptyHint()
{
    // 一次性收起：气泡产生后永不再显（见 .h 声明处口径说明），故不做「扫全列判空」
    if (m_emptyHint && !m_emptyHint->isHidden())
        m_emptyHint->hide();
}

void ChatSessionPage::addMessage(MessageBubbleWidget::Role role, const QString &content)
{
    // 会话标题单点捕获：startConversation/定时送达/历史重放的用户消息全部经过本函数
    if (role == MessageBubbleWidget::Role::User)
        maybeCaptureSessionTitle(content);
    dismissEmptyHint(); // P1-9 空态引导：任何内容入列即收起引导卡
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
    // B2：提交到首 token（含记忆召回异步期）以占位文案消除流内空白，
    // 首个思考/正文/工具事件到达即由气泡内部 dropPlaceholder 清除
    m_currentBubble->startStreaming(tr("处理中…"));
    dismissEmptyHint(); // P1-9：本路径不经 addMessage 建泡（占位泡直挂滚动列），引导卡在此也要收
    m_scrollView->getMainLayout()->addWidget(m_currentBubble);
    scrollToBottom();
    m_agentLoop->run(userText);
    // 用户消息已在 run() 内同步入历史，此刻刷一次：否则发送后要等整回合终局才见抬升
    refreshContextUsage();
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
    m_replaying = true; // 逐泡贴底抑制，尾部单次结算（评审 C1）
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
                // 回放同步即填满，占位无滞留窗口（与实时链路同参传法保持三调用点一致）
                m_currentBubble->startStreaming(tr("处理中…"));
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
                    m_currentBubble->startStreaming(tr("处理中…"));   // B2 占位（回放同步即被内容/工具块清除）
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
            const QJsonObject args = AgentLoopDetail::parseToolArgsText(argsStr);
            const QString summary = AgentLoopDetail::toolSummary(toolName, args);
            const QString content = msg.value(QStringLiteral("content")).toString();
            // 回放成败与实时链路共享单源判定（AgentLoop::isToolFailure）
            const bool ok = !AgentLoop::isToolFailure(content);
            m_currentBubble->appendToolExecution(toolName, summary, content, ok);
            // 任务清单回放重建（s17 缺陷修复：回会话侧栏「任务清单」与快照卡要等新回合模型
            // 重发 todo_write 才出现——实时链路只靠 todoUpdated 事件更新、历史从不重放，
            // 与第四部分 §12「重启后看到的与当时看到的必然一致」的既定契约相悖）。
            // 成功的 todo_write 按实时同一契约走查：校验归一复用 AgentLoop::normalizeTodoSnapshot
            // 单源（禁在回放侧重写一套口径）；非空且与上一快照不同 → 嵌快照卡——实时"延一拍"
            // 是让位自家工具块落位，此刻 appendToolExecution 已同步落位，直接追加无需延帧；
            // 空清单=「计划清空」事件不嵌隐形卡；侧栏每次事件都推，回放完即停在其终态
            if (toolName == ToolNames::TODO_WRITE && ok)
            {
                QJsonArray snapshot;
                if (AgentLoop::normalizeTodoSnapshot(args, &snapshot))
                {
                    if (!snapshot.isEmpty() && snapshot != m_lastTodoSnapshot)
                    {
                        auto *card = new TodoCard(this);
                        card->setTodos(snapshot);
                        m_lastTodoSnapshot = snapshot;
                        m_currentBubble->appendTimelineSection(card);
                    }
                    m_sidebar->setTodos(snapshot);
                }
            }
            scrollToBottom();
            continue;
        }
    }
    closeReplayBubble(); // 收尾末段 assistant（含被中断的 tool_calls）
    m_replaying = false;
    scrollToBottom();    // 回放全程唯一一次贴底结算
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

// 宿主回传会话标题：右侧信息面板是纯视图，其标题唯一来源就是本页 m_sessionTitle，
// 导航子项改名只动 nav label + index.json，不回填此处即表现为「左边已改、右边仍显旧标题」。
// 定值后 maybeCaptureSessionTitle 的「非空即 no-op」顺带保证改名不被后续首条消息派生标题覆盖。
void ChatSessionPage::setSessionTitle(const QString &title)
{
    m_sessionTitle = title.simplified();
    if (m_sidebar)
        m_sidebar->setSessionMeta(m_sessionTitle, m_agentLoop ? m_agentLoop->model() : QString());
}

void ChatSessionPage::startConversation(const QString &text)
{
    addMessage(MessageBubbleWidget::Role::User, text);
    startAssistantStream(text);
}

void ChatSessionPage::scrollToBottom()
{
    // 强制贴底（用户发送/新气泡/回放收尾/回底钮点击）：清除离开旗，恢复跟随
    m_userScrolledAway = false;
    updateJumpBottomBtn();
    doScrollToBottom();
}

void ChatSessionPage::doScrollToBottom()
{
    // 回放期整体抑制逐泡贴底（评审 C1：每条气泡一次全列 activate 是 O(n²)），
    // 由 replayHistory 尾部单次结算
    if (m_replaying)
        return;
    // 先把挂起的布局变更结算，再读取 maximum——否则新增卡片（记忆进度卡/工具块等）
    // 后立刻调用时，滚动条 range 仍是旧值，滚不到真正底部。
    if (auto* vlayout = m_scrollView->getMainLayout())
        vlayout->activate();
    auto scrollBar = m_scrollView->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void ChatSessionPage::followScrollToBottom()
{
    // 跟随式贴底（评审 B1）：用户上翻离开底部期间，流式增量与在途插卡不再拽回视野，
    // 只保持回底钮在场；用户点钮或自行滚回近底后恢复跟随
    if (m_userScrolledAway)
    {
        updateJumpBottomBtn();
        return;
    }
    doScrollToBottom();
}

void ChatSessionPage::evaluateFollowState()
{
    // 用户滚动动作后的跟随态重算：近底（距 maximum ≤ 阈值）=回到跟随；否则=离开
    auto *bar = m_scrollView->verticalScrollBar();
    if (!bar)
        return;
    m_userScrolledAway = bar->value() < bar->maximum() - LayoutConst::kNearBottomPx;
    updateJumpBottomBtn();
}

void ChatSessionPage::updateJumpBottomBtn()
{
    if (!m_jumpBottomBtn)
        return;
    m_jumpBottomBtn->setVisible(m_userScrolledAway);
    if (m_userScrolledAway)
    {
        // 右下角摆位（滚动区本体坐标）；侧栏切换/窗口缩放后经 resize 链重算
        m_jumpBottomBtn->move(m_scrollView->width() - m_jumpBottomBtn->width() - LayoutConst::kJumpBottomBtnMargin,
                              m_scrollView->height() - m_jumpBottomBtn->height() - LayoutConst::kJumpBottomBtnMargin);
        m_jumpBottomBtn->raise();
    }
}

void ChatSessionPage::scheduleRestoreFromDisk()
{
    m_restorePending = true;
}

void ChatSessionPage::focusInput()
{
    if (m_inputEdit)
        m_inputEdit->focusInput();
}

void ChatSessionPage::showEvent(QShowEvent *event)
{
    BasePage::showEvent(event);
    // 惰性恢复（评审 C1）：启动路径只建占位页，首次切入本会话才载盘重放历史——
    // 数百气泡的 widget 树不再随主窗口构造同栈全量重建
    if (m_restorePending)
    {
        m_restorePending = false;
        restoreFromDisk();
    }
}

bool ChatSessionPage::eventFilter(QObject *watched, QEvent *event)
{
    // 滚轮=用户主动滚动意图：上翻立即置离开旗；下翻待滚动落定后按位置重算
    // （过滤器先于 QScrollArea 处理事件，value 尚未更新，延一拍再 evaluate）
    if (watched == m_scrollView->viewport() && event->type() == QEvent::Wheel)
    {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->angleDelta().y() > 0)
        {
            m_userScrolledAway = true;
            updateJumpBottomBtn();
        }
        else
            QTimer::singleShot(0, this, [this]() { evaluateFollowState(); });
    }
    return BasePage::eventFilter(watched, event);
}

// 滚动合并入口（效率 P5）：流式高频与在途内容事件（每个 SSE delta 的思考/正文增量、工具起止、
// 子代理进度行、权限卡、清单快照卡、记忆进度卡）统一走此口。30ms 窗口内的多次请求只在窗口
// 起点挂一次定时器，到期由 scrollToBottom 做一次布局结算并贴底——把「每帧一次全列 activate」
// 降为「每 30ms 至多一次」。窗口已在途则直接复用，不延长（保证最多晚 30ms 而非持续顺延）
void ChatSessionPage::requestScrollToBottom()
{
    if (m_scrollCoalesceTimer && !m_scrollCoalesceTimer->isActive())
        m_scrollCoalesceTimer->start();
}

// 终态强制结算：丢弃在途合并窗口（不再等 30ms）并立即贴底，供 finished/error/记忆链收口/
// 用户 stop 调用，确保会话结束瞬间滚动停在真正的底部、无残留延迟滚动帧
void ChatSessionPage::flushPendingScroll()
{
    if (m_scrollCoalesceTimer)
        m_scrollCoalesceTimer->stop();
    // 终态结算走跟随入口（评审 B1）：用户上翻回看时，回合结束不粗暴拽回底部，
    // 由回底钮承接「跳看最新」；用户发送/点钮等主动路径仍走强制 scrollToBottom
    followScrollToBottom();
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
    // P1-9：用户主动停止的终局同样走 AgentLoop::error("已停止。")，不区分会被点成红色
    // 「本轮失败」通知条——把「我按了停止」报成失败。置位后由 error 接线消费（单点复位）
    m_stopRequested = true;
    // 用户主动停止即刻强制结算滚动（效率 P5）：不等终态信号回流，视图立即停在当前底部；
    // 其后 error("已停止。")/finished 终态链还会各自再结算一次，幂等无害
    flushPendingScroll();
    if (m_agentLoop)
        m_agentLoop->stop();
}

// 侧栏显隐切换单点：偏好落 config.json（键 sidebarVisible，默认显示）+ 浮动钮互斥 +
// 消息列重钳。构造期 buildLayout 尾部也走此函数完成首帧恢复
void ChatSessionPage::setSidebarVisible(bool visible)
{
    AppSettings::setValue(QStringLiteral("sidebarVisible"), visible);
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

// 消息列/输入组同栏宽钳制：列宽 = min(800, 可用宽)，侧栏可见时扣除「宽+隙」整块
// （隐藏时 QBoxLayout 自动剔除该列与 spacing，左列占满，无扣除）。列宽只设
// maximumWidth、居中留白手动写入行容器边距，配合 Ignored 行容器彻底断开窗口
// 收缩棘轮；含气泡宽度刷新，resizeEvent 与 setSidebarVisible 共用
void ChatSessionPage::applyColumnWidth()
{
    const int sidebarReserve = (m_sidebar && !m_sidebar->isHidden())
        ? LayoutConst::kSidebarWidth + LayoutConst::kSidebarGap
        : 0;
    const int avail = qMax(0, width() - 2 * LayoutConst::kSideMargin - sidebarReserve);
    const int columnWidth = qMin(LayoutConst::kColumnMaxWidth, avail);
    // 两侧留白对称居中；奇数富余 1px 落在右侧，与旧 AlignHCenter 结算一致
    const int pad = qMax(0, (avail - columnWidth) / 2);
    const int padRight = qMax(0, avail - columnWidth - pad);
    if (m_scrollRowLayout)
        m_scrollRowLayout->setContentsMargins(pad, 0, padRight, 0);
    if (m_inputRowLayout)
        m_inputRowLayout->setContentsMargins(pad, 0, padRight, 0);
    // P1-9 通知条同栏：行容器留白与消息列一致，条本身钳到列宽上限（其 sizePolicy
    // 是 Expanding，不钳会铺满左列、与阅读列脱栏）
    if (m_noticeRowLayout)
        m_noticeRowLayout->setContentsMargins(pad, 0, padRight, 0);
    if (m_noticeBar)
        m_noticeBar->setMaximumWidth(columnWidth);
    if (m_scrollView)
        m_scrollView->setMaximumWidth(columnWidth);
    if (m_inputSection)
        m_inputSection->setMaximumWidth(columnWidth);

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

// 上下文占用快照 → 侧栏「上下文」节（条+数字）+ 底部状态条（轮步/消耗/速度）。刷新时机
// （全是低频点，刻意不接 textDelta/thinkingDelta 高频流）：
//   ① 用户消息入历史后（startAssistantStream）——发送即见抬升，不必等整回合；
//   ② 每个工具结果到达（toolOutputReady，含 compact 卡片）——多轮 ReAct 途中逐轮跟手；
//   ③ 回合终局（finished / error）；④ 会话首建重放历史后（restoreFromDisk）。
// 口径（规格修4）：≈token 估算与压缩触发同源——服务端 usage.prompt_tokens 锚 + 本地增量估算，
// 锚失效时兜底全量本地估算（含 system + tools schema + 注入块，即 B4 修复）；
// 预算为 config.json 字符上限的纯派生值（/4），键语义不变。
void ChatSessionPage::refreshContextUsage()
{
    if (!m_agentLoop || !m_statusStrip)
        return;
    const qsizetype used = m_agentLoop->estimatedContextTokens();
    const qsizetype limit = AgentConst::contextTokenBudget();
    // 占用（条+数字）整体推侧栏「上下文」节——比例走 UsageTokens::usageRatio 单源，
    // 条与数字同一时刻同一读数（见 SessionSidebar.h 的分工说明）
    if (m_sidebar)
        m_sidebar->setContextUsage(used, limit);
    // 消耗累计同批推入（同一刷新时机、同一数据源，不再另起信号链）
    m_statusStrip->setTokenUsage(m_agentLoop->sessionUsageTotals());
    // 输出速度同批推入（同一刷新时机、同一数据源）：刷新点已覆盖 usage 到达——usage 恒先于
    // messageFinished，工具轮在 toolOutputReady 补刷、文本轮在 finished/error 终局刷新
    m_statusStrip->setOutputRate(m_agentLoop->outputRate());
    // 轮步读数同批推入（复用本函数的四个低频刷新点，不另起信号链、不接 textDelta 高频流）。
    // 步数在 toolOutputReady 那一拍跟手（延一帧的 singleShot 与占用条同栈，此时
    // m_toolIterations 已在 continueWithToolResults 前 ++ 完成）；回合数在
    // startAssistantStream（用户消息已入历史、run() 已自增）即抬升，发送即见「第 N 轮」。
    // 上限走 AgentConst::maxToolIterationsValue() 现读喂进 tooltip——本类是纯视图不读
    // config.json，与上面 contextTokenBudget() 同批取值同型纪律
    m_statusStrip->setTurnStep(m_agentLoop->userTurnCount(),
                               m_agentLoop->toolIterationCount(),
                               AgentConst::maxToolIterationsValue());
}

// 抄宿主 LiteHarness 的标题派生规则（首条用户消息 simplified，超 12 字截断加省略号，
// 空回退「新会话」）；仅首次捕获生效——宿主改名/恢复经 setSessionTitle 先行定值后即不再派生覆盖
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
