#include "SessionSidebar.h"
#include "ThemeAware.h"
#include "LayoutConstants.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPropertyAnimation>
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

/// 上下文占用计量：双色自绘条（QSS 子对象 #ctxTrack/#ctxFill）+ 属主注入的说明文案。
/// 用自绘而非 FluProgressBar——第三方控件把主题 QSS 挂自身，覆盖其取色需斗争样式表；
/// 两根受控 QSS 小条在本仓「自身样式表」机制下天然可主题化。
class ContextMeter : public QWidget {
public:
    explicit ContextMeter(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 4, 8, 6);
        v->setSpacing(6);

        m_track = new QWidget(this);
        m_track->setObjectName(QStringLiteral("ctxTrack"));
        m_track->setFixedHeight(6);
        m_fill = new QWidget(m_track);
        m_fill->setObjectName(QStringLiteral("ctxFill"));
        m_fill->setProperty("level", QStringLiteral("normal"));

        m_numbers = new QLabel(this);
        m_numbers->setObjectName(QStringLiteral("ctxNumbers"));
        m_numbers->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        v->addWidget(m_track);
        v->addWidget(m_numbers);
    }

    /// ratio 钳到 [0,1]；warn 阈值（≥0.8）由此切琥珀色并触发属性重 polish
    void setUsage(double ratio, const QString& numbersText) {
        m_ratio = qBound(0.0, ratio, 1.0);
        m_numbers->setText(numbersText);
        const QString level = m_ratio >= 0.8 ? QStringLiteral("warn") : QStringLiteral("normal");
        if (m_fill->property("level").toString() != level) {
            m_fill->style()->unpolish(m_fill);
            m_fill->setProperty("level", level);
            m_fill->style()->polish(m_fill);
        }
        relayoutFill();
    }

protected:
    void resizeEvent(QResizeEvent*) override { relayoutFill(); }

private:
    void relayoutFill() {
        const int w = qMax(m_ratio > 0.0 ? 3 : 0,
                           int(m_track->width() * m_ratio));
        m_fill->setGeometry(0, 0, qMin(w, m_track->width()), m_track->height());
    }

    QWidget* m_track = nullptr;
    QWidget* m_fill = nullptr;
    QLabel* m_numbers = nullptr;
    double m_ratio = 0.0;
};

/// 运行状态灯行：圆点 + 文案；state 属性（idle|running|waiting）驱动整行 QSS 着色，
/// waiting 与 TodoCard active 同语言（淡染底 + 左描边）。
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

    // ① 上下文占用（恒展开）
    m_contextSection = new SidebarSection(tr("上下文"), inner);
    m_contextSection->setCollapsible(false);
    m_meter = new ContextMeter;
    m_contextSection->body()->addWidget(m_meter);
    iv->addWidget(m_contextSection);

    // ② 运行状态（恒展开）
    m_statusSection = new SidebarSection(tr("状态"), inner);
    m_statusSection->setCollapsible(false);
    m_statusRow = new StatusRow;
    m_statusSection->body()->addWidget(m_statusRow);
    iv->addWidget(m_statusSection);

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
    m_footerVersion = new QLabel(tr("lite-harness v%1").arg(QCoreApplication::applicationVersion()), this);
    m_footerVersion->setObjectName(QStringLiteral("sbFooterVersion"));
    root->addWidget(m_footerDir);
    root->addWidget(m_footerVersion);

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

void SessionSidebar::setContextUsage(qsizetype usedTokens, qsizetype limitTokens) {
    const double ratio = limitTokens > 0 ? double(usedTokens) / double(limitTokens) : 0.0;
    const QLocale loc(QLocale::c());
    const QString text = tr("≈%1 / %2 token · %3%")
                             .arg(loc.toString(usedTokens),
                                  loc.toString(limitTokens),
                                  QString::number(ratio * 100.0, 'f', 1));
    m_meter->setUsage(ratio, text);
}

void SessionSidebar::setRunning(bool running) {
    m_running = running;
    applyRunState();
}

void SessionSidebar::setPermissionPending(bool pending) {
    m_permPending = pending;
    applyRunState();
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

void SessionSidebar::applyRunState() {
    // waiting 优先：审批挂起时无论 running 与否都亮警示态
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
    m_todoSection->refreshArrow();
}
