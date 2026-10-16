#include "SessionSidebar.h"
#include "ThemeAware.h"
#include "LayoutConstants.h"
#include "UsageTokens.h"   // 占用比例算术（usageRatio）——状态条数字与本节占用条共用同一实现

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QLocale>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <FluThemeUtils.h>
#include <FluUtils.h>

// ============================================================================
// 文件内辅助控件（无自定义信号槽，故不带 Q_OBJECT；所有 tr() 文案由
// SessionSidebar 组好后注入，保证翻译上下文统一挂在本类名下）
// ============================================================================

/// 侧栏轻量折叠节壳：28px 头部行（标题 + 等宽计数 + 箭头）+ 内容 maximumHeight 动画。
/// 折叠策略（≤2 恒展开、>2 才可折叠）由属主判定后经 setCollapsible 下发。
class SidebarSection : public QWidget {
public:
    explicit SidebarSection(const QString& title, QWidget* parent = nullptr)
        : QWidget(parent), m_title(title) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        m_header = new QWidget(this);
        m_header->setObjectName(QStringLiteral("sbSectionHead"));
        m_header->setFixedHeight(28);
        m_header->setProperty("clickable", false);
        auto* hv = new QHBoxLayout(m_header);
        hv->setContentsMargins(8, 0, 8, 0);
        hv->setSpacing(6);
        m_titleLabel = new QLabel(title, m_header);
        m_titleLabel->setObjectName(QStringLiteral("sbSectionTitle"));
        m_countLabel = new QLabel(m_header);
        m_countLabel->setObjectName(QStringLiteral("sbSectionCount"));
        m_arrow = new QLabel(m_header);
        m_arrow->setObjectName(QStringLiteral("sbSectionArrow"));
        m_arrow->setAlignment(Qt::AlignCenter);
        hv->addWidget(m_titleLabel);
        hv->addStretch();
        hv->addWidget(m_countLabel);
        hv->addWidget(m_arrow);
        m_header->installEventFilter(this);

        m_body = new QWidget(this);
        m_body->setObjectName(QStringLiteral("sbSectionBody"));
        m_bodyLayout = new QVBoxLayout(m_body);
        m_bodyLayout->setContentsMargins(0, 2, 0, 4);
        m_bodyLayout->setSpacing(2);

        v->addWidget(m_header);
        v->addWidget(m_body);

        m_anim = new QPropertyAnimation(m_body, "maximumHeight", this);
        m_anim->setDuration(300);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QPropertyAnimation::finished, this, [this] {
            // 展开到位后解除高度枷锁：其后内容增删可自然撑高节体
            if (m_expanded) m_body->setMaximumHeight(QWIDGETSIZE_MAX);
        });

        refreshArrow();
    }

    QVBoxLayout* body() const { return m_bodyLayout; }

    void setCollapsible(bool on) {
        m_collapsible = on;
        m_header->setProperty("clickable", on);
        refreshArrow();
        if (!on && !m_expanded) setExpanded(true, false); // 失去折叠资格 → 恒展开
    }
    void setCount(const QString& text) { m_countLabel->setText(text); }
    void setSummary(const QString& text) { m_summary = text; applyTitle(); }
    bool isExpanded() const { return m_expanded; }

    void setExpanded(bool expanded, bool animate) {
        if (m_expanded == expanded) {
            const int mh = m_body->maximumHeight();
            if (expanded && mh == QWIDGETSIZE_MAX) return;   // 已完全展开
            if (!expanded && mh == 0) { m_anim->stop(); return; } // 已完全折叠
        }
        m_expanded = expanded;
        applyTitle();
        refreshArrow();

        const int content = m_body->sizeHint().height();
        if (!animate) {
            m_anim->stop();
            m_body->setMaximumHeight(expanded ? QWIDGETSIZE_MAX : 0);
            return;
        }
        m_anim->stop();
        const int from = (m_body->maximumHeight() == QWIDGETSIZE_MAX) ? content
                                                                      : m_body->maximumHeight();
        m_anim->setStartValue(from);
        m_anim->setEndValue(expanded ? content : 0);
        m_anim->start();
    }

    /// 主题切换时重取箭头（chevron 位图按当前主题着色）
    void refreshArrow() {
        if (!m_collapsible) {
            m_arrow->clear();
            m_arrow->setFixedWidth(0);
            return;
        }
        m_arrow->setFixedWidth(18);
        const FluAwesomeType type = m_expanded ? FluAwesomeType::ChevronUp
                                              : FluAwesomeType::ChevronDown;
        m_arrow->setPixmap(FluIconUtils::getFluentIconPixmap(
            type, FluThemeUtils::getUtils()->getTheme(), 12, 12));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_header && event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (m_collapsible && me->button() == Qt::LeftButton)
                setExpanded(!m_expanded, true);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void applyTitle() {
        if (!m_expanded && !m_summary.isEmpty())
            m_titleLabel->setText(m_title + QStringLiteral("（") + m_summary + QStringLiteral("）"));
        else
            m_titleLabel->setText(m_title);
    }

    QString m_title;
    QString m_summary;
    bool m_collapsible = false;
    bool m_expanded = true;
    QWidget* m_header = nullptr;
    QWidget* m_body = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_countLabel = nullptr;
    QLabel* m_arrow = nullptr;
    QPropertyAnimation* m_anim = nullptr;
};

// ============================================================================
// ContextMeter —— 「上下文」节的占用条（纯图形，无文字）。
//
// 数字不在这里：精确读数（≈used / limit token · %）在输入框下方的 ChatStatusStrip 单行，
// 本节只画比例。两处必须给同一个数，所以比例算术只在 UsageTokens::usageRatio 实现一次，
// 本类与状态条都调用它——各自除一遍会在同一时刻给出两个读数。
// 取色走三主题 SessionSidebar.qss 的 #ctxTrack / #ctxFill（含 level="warn" 变体）。
// ============================================================================
class ContextMeter : public QWidget {
public:
    explicit ContextMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("ctxTrack"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(kBarHeight);
        m_fill = new QWidget(this);
        m_fill->setObjectName(QStringLiteral("ctxFill"));
        m_fill->setFixedHeight(kBarHeight);
    }

    void setRatio(double ratio) {
        m_ratio = ratio;
        applyLevel();
        relayoutFill();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        relayoutFill();
    }

private:
    static constexpr int kBarHeight = 6;
    static constexpr double kWarnRatio = 0.8;

    void applyLevel() {
        const QString level = m_ratio >= kWarnRatio ? QStringLiteral("warn") : QStringLiteral("normal");
        if (m_level == level)
            return;
        m_level = level;
        m_fill->setProperty("level", level);
        m_fill->style()->unpolish(m_fill);
        m_fill->style()->polish(m_fill);
    }

    void relayoutFill() {
        if (width() <= 0)
            return;
        m_fill->setGeometry(0, 0, int(m_ratio * double(width())), height());
    }

    double   m_ratio = 0.0;
    QString  m_level;
    QWidget* m_fill = nullptr;
};

// ============================================================================
// StatusRow —— 「状态」节的运行状态灯：圆点 + 文案，state 属性（idle|running|
// waiting）驱动整行 QSS 着色。
//
// 只画灯不画数：占用数字/消耗/速度三段文字在输入框下方的 ChatStatusStrip 单行
// （见 ChatStatusStrip.h）。本行刻意**不**复用那个多段行——把灯与数塞进同一行会
// 让侧栏与状态条同时各有一份读数，而这两处的可见性不同（侧栏可收起）。
//
// 描边已撤：QSS 单侧 border 画在控件矩形内侧，会让该行圆点与文字比相邻行右移
// 3px（行 setContentsMargins 8,0,8,0），强调改由淡染底 + 加粗 + 琥珀色承载；
// waiting 与消息流审批卡、#sbRow[status="active"] 同语言。
// ============================================================================
class StatusRow : public QWidget {
public:
    explicit StatusRow(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("statusRow"));
        setProperty("state", QStringLiteral("idle"));
        setFixedHeight(26);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(8, 0, 8, 0);
        h->setSpacing(7);
        m_dot = new QLabel(QStringLiteral("\u25CF"), this);
        m_dot->setObjectName(QStringLiteral("statusDot"));
        m_dot->setFixedWidth(14);
        m_dot->setAlignment(Qt::AlignCenter);
        m_text = new QLabel(this);
        m_text->setObjectName(QStringLiteral("statusText"));
        h->addWidget(m_dot);
        h->addWidget(m_text);
        h->addStretch();
    }

    /// 三态切换：state 变了才重 polish（属性驱动 QSS），文案每次直写
    void setState(const QString& state, const QString& text) {
        if (property("state").toString() != state) {
            style()->unpolish(this);
            setProperty("state", state);
            style()->polish(this);
        }
        m_text->setText(text);
    }

private:
    QLabel* m_dot = nullptr;
    QLabel* m_text = nullptr;
};

// ============================================================================
// SessionSidebar
// ============================================================================

namespace {
constexpr int kPanelHMargin = 14;    // 面板左右内边距
constexpr int kRowHeight = 24;       // TODO 行高（比消息流 TodoCard 稍密）
constexpr int kMarkWidth = 16;       // 行首状态点宽
constexpr int kRowHPadding = 20;     // 行左右留白 + 点距（8+8+6 的取整用途常量，估算可用文本宽）
constexpr int kBodyHPadding = 4;     // 节体左右缘修正

/// 生成一行「状态点 + 文本」：propName/propValue 决定 QSS 属性着色（如 status=…），
/// 文本按 avail 宽度截断省略，tooltip 保留全文
QWidget* makeRow(const char* propName, const QString& propValue,
                 const QString& markText, const QString& fullText,
                 int availWidth, Qt::TextElideMode mode) {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("sbRow"));
    row->setProperty(propName, propValue);
    row->setFixedHeight(kRowHeight);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 8, 0);
    h->setSpacing(6);
    auto* mark = new QLabel(markText, row);
    mark->setObjectName(QStringLiteral("sbMark"));
    mark->setFixedWidth(kMarkWidth);
    mark->setAlignment(Qt::AlignCenter);
    auto* text = new QLabel(row);
    text->setObjectName(QStringLiteral("sbText"));
    text->setTextFormat(Qt::PlainText);
    text->setText(QFontMetrics(text->font()).elidedText(fullText, mode, qMax(availWidth, 60)));
    text->setToolTip(fullText);
    h->addWidget(mark);
    h->addWidget(text, 1);
    return row;
}

/// 节体可用文本宽（行内边距/状态点之外的余量）；未布局时 width() 不可信，回退面板宽推算
int bodyTextAvail(int bodyWidth, int sidebarWidth) {
    const int raw = (bodyWidth > 60 ? bodyWidth : sidebarWidth - 2 * kPanelHMargin - 2 * kBodyHPadding)
                    - 2 * 8 - kMarkWidth - 6;
    return qMax(raw, 60);
}
} // namespace

SessionSidebar::SessionSidebar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("SessionSidebar"));
    // 自定义 QWidget 子类默认不渲染 QSS 盒模型（背景/边框），必须显式开启
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(LayoutConst::kSidebarWidth);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kPanelHMargin, 12, kPanelHMargin, 12);
    root->setSpacing(6);

    // ---- 面板头部：标题 + 收起钮 ----
    auto* head = new QHBoxLayout;
    head->setSpacing(6);
    auto* panelTitle = new QLabel(tr("会话面板"), this);
    panelTitle->setObjectName(QStringLiteral("sbPanelTitle"));
    m_hideBtn = new QToolButton(this);
    m_hideBtn->setObjectName(QStringLiteral("sidebarHideBtn"));
    m_hideBtn->setAutoRaise(true);
    m_hideBtn->setIconSize(QSize(14, 14));
    m_hideBtn->setCursor(Qt::PointingHandCursor);
    m_hideBtn->setToolTip(tr("收起侧边栏"));
    connect(m_hideBtn, &QToolButton::clicked, this, &SessionSidebar::hideRequested);
    head->addWidget(panelTitle);
    head->addStretch();
    head->addWidget(m_hideBtn);
    root->addLayout(head);

    // ---- 会话标题 / 模型名 ----
    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("sbSessTitle"));
    m_titleLabel->setTextFormat(Qt::PlainText);
    m_modelLabel = new QLabel(this);
    m_modelLabel->setObjectName(QStringLiteral("sbSessModel"));
    m_modelLabel->setTextFormat(Qt::PlainText);
    root->addWidget(m_titleLabel);
    root->addWidget(m_modelLabel);

    auto* separator = new QWidget(this);
    separator->setObjectName(QStringLiteral("sbSeparator"));
    separator->setFixedHeight(1);
    root->addWidget(separator);

    // ---- 滚动体：信息节（内容可超屏：长 TODO 清单） ----
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("sbScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    inner->setObjectName(QStringLiteral("sbScrollInner"));
    auto* iv = new QVBoxLayout(inner);
    iv->setContentsMargins(0, 4, 0, 4);
    iv->setSpacing(10);

    // ① 上下文占用条（只有条：精确数字在输入框下方的状态条单行）。不可折叠——一节一行
    //    没有可折叠的内容；count 留空（本节再写一遍数会给出两处读数），口径写进条的 tooltip。
    m_contextSection = new SidebarSection(tr("上下文"), inner);
    m_contextMeter = new ContextMeter(m_contextSection);
    m_contextSection->body()->addWidget(m_contextMeter);
    iv->addWidget(m_contextSection);

    // ② 运行状态灯（恒展开；三态合一 空闲/运行中/等待审批）。灯在本节、不在状态条：
    //    状态条只承载数字/消耗/速度那类「收起侧栏也必须常驻」的计量数据，而灯是面板的
    //    身份信息——两处同时亮会在同一时刻给出两个读数（与上面条/字「同一读数」同纪律）。
    m_statusSection = new SidebarSection(tr("状态"), inner);
    m_statusRow = new StatusRow(m_statusSection);
    m_statusSection->body()->addWidget(m_statusRow);
    iv->addWidget(m_statusSection);

    // ②b s13 队友名册（空名册整节隐藏；≤2 恒展开，>2 可折叠，同任务清单策略）
    m_teammateSection = new SidebarSection(tr("队友清单"), inner);
    m_teammateSection->hide();
    m_teammateList = m_teammateSection->body();
    iv->addWidget(m_teammateSection);

    // ③ 任务清单（≤2 恒展开，>2 可折叠）
    m_todoSection = new SidebarSection(tr("任务清单"), inner);
    m_todoList = m_todoSection->body();
    m_todoEmpty = new QLabel(tr("暂无任务"), inner);
    m_todoEmpty->setObjectName(QStringLiteral("sbEmptyHint"));
    m_todoList->addWidget(m_todoEmpty);
    iv->addWidget(m_todoSection);

    iv->addStretch();
    scroll->setWidget(inner);
    root->addWidget(scroll, 1);

    // ---- 页脚：工作目录 + 版本（版本号取运行时，禁硬编码） ----
    m_footerDir = new QLabel(this);
    m_footerDir->setObjectName(QStringLiteral("sbFooterDir"));
    m_footerDir->setTextFormat(Qt::PlainText);
    m_footerVersion = new QLabel(tr("lite-harness %1").arg(QCoreApplication::applicationVersion()), this);
    m_footerVersion->setObjectName(QStringLiteral("sbFooterVersion"));
    root->addWidget(m_footerDir);
    root->addWidget(m_footerVersion);

    // 状态灯初值：构造期就落到「空闲」态并写好文案，否则面板首帧是一行没有文字的空灯行
    applyRunState();

    // 主题挂接惯例：构造函数末尾 bind；extraRefresh 重取按主题着色的 chevron 图标
    ThemeAware::bind(QStringLiteral("SessionSidebar.qss"), this, [this] {
        refreshIcons();
    });
}

void SessionSidebar::setSessionMeta(const QString& title, const QString& model) {
    m_sessionTitle = title.isEmpty() ? tr("新会话") : title;
    m_sessionModel = model;
    // 标题可能超宽：按面板内容宽 elide（tooltip 保留全文）
    const int avail = width() - 2 * kPanelHMargin;
    m_titleLabel->setText(fontMetrics().elidedText(m_sessionTitle, Qt::ElideRight, qMax(avail, 80)));
    m_titleLabel->setToolTip(m_sessionTitle);
    m_modelLabel->setText(model);
}

void SessionSidebar::setWorkDir(const QString& dir) {
    const int avail = width() - 2 * kPanelHMargin;
    m_footerDir->setText(QFontMetrics(font()).elidedText(dir, Qt::ElideMiddle, qMax(avail, 80)));
    m_footerDir->setToolTip(dir);
}

void SessionSidebar::setTodos(const QJsonArray& todos) {
    // 清旧行（hide + deleteLater：立即让出视觉与布局，事件循环内安全销毁）；占位行摘出不销毁
    while (QLayoutItem* item = m_todoList->takeAt(0)) {
        QWidget* w = item->widget();
        if (w == m_todoEmpty) { delete item; continue; }
        if (w) { w->hide(); w->deleteLater(); }
        delete item;
    }

    qsizetype done = 0, active = 0;
    const int avail = bodyTextAvail(m_todoSection->width(), width());
    for (const QJsonValue& v : todos) {
        const QJsonObject o = v.toObject();
        const QString status = o.value(QStringLiteral("status")).toString();
        const QString content = o.value(QStringLiteral("content")).toString();
        QString state, mark;
        if (status == QLatin1String("in_progress")) { state = QStringLiteral("active"); mark = QStringLiteral("\u25CF"); ++active; }
        else if (status == QLatin1String("completed")) { state = QStringLiteral("done"); mark = QStringLiteral("\u2713"); ++done; }
        else { state = QStringLiteral("pending"); mark = QStringLiteral("\u25CB"); }
        m_todoList->addWidget(makeRow("status", state, mark, content, avail, Qt::ElideRight));
    }
    if (todos.isEmpty()) m_todoList->addWidget(m_todoEmpty);
    m_todoEmpty->setVisible(todos.isEmpty());

    m_todoCount = int(todos.size());
    refreshTodoSummary(todos.size(), done, active);
    // 折叠策略：≤2 恒展开；>2 获得折叠资格并保留用户当前选择
    const bool collapsible = m_todoCount > 2;
    if (!collapsible && !m_todoSection->isExpanded())
        m_todoSection->setExpanded(true, false);
    m_todoSection->setCollapsible(collapsible);
}

void SessionSidebar::refreshTodoSummary(qsizetype total, qsizetype done, qsizetype active) {
    m_todoSection->setCount(QString("%1/%2").arg(done).arg(total));
    QStringList parts;
    if (active > 0) parts << tr("%1 进行中").arg(active);
    if (total - done - active > 0) parts << tr("%1 待办").arg(total - done - active);
    if (done > 0) parts << tr("%1 完成").arg(done);
    m_todoSection->setSummary(parts.isEmpty() ? QString::number(total)
                                              : parts.join(QStringLiteral(" · ")));
}

void SessionSidebar::setTeammates(const QList<QPair<QString, QString>>& roster) {
    // 清旧行（与 setTodos 同款 hide + deleteLater；无占位行——空名册直接隐藏整节）
    while (QLayoutItem* item = m_teammateList->takeAt(0)) {
        QWidget* w = item->widget();
        if (w) { w->hide(); w->deleteLater(); }
        delete item;
    }
    if (roster.isEmpty()) {
        m_teammateSection->setSummary(QString());
        m_teammateSection->hide();
        return;
    }
    m_teammateSection->show();

    // statusName token（引擎单源英文数据域）→ 行态/圆点/中文文案；未知 token 走兜底行
    int working = 0, waiting = 0, idle = 0, stopping = 0;
    const int avail = bodyTextAvail(m_teammateSection->width(), width());
    for (const auto& entry : roster) {
        const QString& name = entry.first;
        const QString& token = entry.second;
        QString state, mark, label;
        if (token == QLatin1String("working")) {
            state = QStringLiteral("active"); mark = QStringLiteral("\u25CF");
            label = tr("%1 · 执行中").arg(name); ++working;
        } else if (token == QLatin1String("waiting_approval")) {
            state = QStringLiteral("active"); mark = QStringLiteral("\u25D0");
            label = tr("%1 · 等待审批").arg(name); ++waiting;
        } else if (token == QLatin1String("stopping")) {
            state = QStringLiteral("pending"); mark = QStringLiteral("\u25A0");
            label = tr("%1 · 停止中").arg(name); ++stopping;
        } else {
            state = QStringLiteral("pending"); mark = QStringLiteral("\u25CB");
            label = tr("%1 · 空闲").arg(name); ++idle;
        }
        m_teammateList->addWidget(
            makeRow("status", state, mark, label, avail, Qt::ElideRight));
    }

    m_teammateSection->setCount(QString("%1/%2").arg(working).arg(roster.size()));
    QStringList parts;
    if (working > 0) parts << tr("%1 执行中").arg(working);
    if (waiting > 0) parts << tr("%1 待审批").arg(waiting);
    if (stopping > 0) parts << tr("%1 停止中").arg(stopping);
    if (idle > 0) parts << tr("%1 空闲").arg(idle);
    m_teammateSection->setSummary(parts.join(QStringLiteral(" · ")));

    // 折叠策略与任务清单同规则：≤2 恒展开；>2 获得折叠资格并保留用户当前选择
    const bool collapsible = roster.size() > 2;
    if (!collapsible && !m_teammateSection->isExpanded())
        m_teammateSection->setExpanded(true, false);
    m_teammateSection->setCollapsible(collapsible);
}

void SessionSidebar::setContextUsage(qsizetype usedTokens, qsizetype limitTokens)
{
    if (!m_contextMeter)
        return;
    // 比例算术只在 UsageTokens::usageRatio 实现一次：状态条的数字与本节的条分属两个视图，
    // 各自除一遍会在同一时刻给出两个读数（一个显示 100%，另一个画 120% 宽）。
    m_contextMeter->setRatio(UsageTokens::usageRatio(usedTokens, limitTokens));

    const QLocale loc(QLocale::c());
    // tooltip 写口径边界：条本身只说比例，不说分母是 settings.ini 字符串上限的派生值（/4），
    // 也不说估算与压缩触发同源、会被后续回读覆盖——这两句决定读数能不能用来判断会不会被
    // 压缩改写。精确数字在输入框下方的状态条单行，这里只给指向，不重复成一份独立读数。
    m_contextMeter->setToolTip(tr("上下文占用条：≈%1 token 估算 ÷ %2 token 预算（预算是 settings.ini 字符串上限的派生值 /4，与压缩触发同源，会被后续回读覆盖）；精确数字见输入框下方状态条。")
                                   .arg(loc.toString(usedTokens), loc.toString(limitTokens)));
}

// 状态灯的两个推入口（纯视图纪律不破：本页不订阅 AgentLoop，running/permission 由
// ChatSessionPage 接线转发）。setter 只写缓存态再交 applyRunState 归并——两个信号各自
// 到达时都不能独立决定显示什么（waiting 优先于 running 是跨信号的关系，不是单信号的事实）。

void SessionSidebar::setRunning(bool running) {
    m_running = running;
    applyRunState();
}

void SessionSidebar::setPermissionPending(bool pending) {
    m_permPending = pending;
    applyRunState();
}

void SessionSidebar::applyRunState() {
    if (!m_statusRow)
        return;
    // waiting 优先：审批挂起时无论 running 与否都亮警示态（「在等你」比「在跑」更该被看见）
    if (m_permPending) {
        m_statusRow->setState(QStringLiteral("waiting"), tr("等待审批"));
    } else if (m_running) {
        m_statusRow->setState(QStringLiteral("running"), tr("运行中"));
    } else {
        m_statusRow->setState(QStringLiteral("idle"), tr("空闲"));
    }
}

void SessionSidebar::refreshIcons() {
    m_hideBtn->setIcon(FluIconUtils::getFluentIconPixmap(
        FluAwesomeType::ChevronRight, FluThemeUtils::getUtils()->getTheme(), 14, 14));
    m_contextSection->refreshArrow();
    m_statusSection->refreshArrow();
    m_teammateSection->refreshArrow();
    m_todoSection->refreshArrow();
}
