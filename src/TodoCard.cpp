#include "TodoCard.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <FluUtils.h>

// 行几何常量与 QSS #todoRow/#todoList 保持耦合：
//   行高 26px、列表上下边距各 4px、行间距 2px（改动需同步 stylesheet/*/TodoCard.qss 注释）
static constexpr int kRowHeight = 26;
static constexpr int kListVerticalMargin = 4;
static constexpr int kListSpacing = 2;

TodoCard::TodoCard(QWidget *parent) : CollapsibleBlock(parent)
{
    // ---- 标题栏：[任务清单] …… [done/total] [▼]，与 ToolBlock 头部同款实色底与圆角 ----
    auto *headerLayout = initHeader("todoHeader", 12, 0, 10, 0);

    m_titleLabel = createTitleLabel("todoTitle");
    m_titleLabel->setText(tr("任务清单"));

    // 计数标签为本类独有件，不进基类装配管线
    m_countLabel = new QLabel(m_header);
    m_countLabel->setObjectName("todoCount");

    m_arrowLabel = createGlyphLabel("todoArrow");

    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_countLabel);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 列表区：QScrollArea 承载行容器，超上限内部滚动 ----
    // 基类装配：NoFrame / 横条恒关 / 竖条按需 + stackUnder 头部之下 + 点击过滤 + 最小高度
    m_scroll = initScrollContent("todoScroll");

    m_list = new QWidget(m_scroll);
    m_list->setObjectName("todoList");
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, kListVerticalMargin, 0, kListVerticalMargin);
    m_listLayout->setSpacing(kListSpacing);
    m_scroll->setWidget(m_list);
    m_scroll->setWidgetResizable(true); // 行容器随视口取宽，自然高度超出视口时纵向滚动

    // 限高走基类默认策略（expandedHeightCap 返回此值），单源引用常量
    m_maxExpandedHeight = kMaxListHeight;

    // 主题装配：基类 initTheme 委托 ThemeAware::bind —— QSS 首刷 + refreshIcons 首刷
    // （构造尾调用，虚派发安全）+ themeChanged 订阅；三态颜色全部由 QSS 属性选择器控制
    initTheme("TodoCard.qss");

    // 初始态与 Thinking/Tool（initCollapsed）不同：状态面板默认展开是 lcc 面板的核心信息；
    // 但无任务即无高度——内容区隐藏，等待第一次 setTodos
    m_expanded = true;
    m_contentArea->hide();
}

void TodoCard::setTodos(const QJsonArray &todos)
{
    setVisible(!todos.isEmpty()); // 空列表整卡收起：保留挂载位，等待下次出现

    rebuildRows(todos);

    // 头部完成计数（done/total），朴素呈现，语义在 tooltip
    m_countLabel->setText(QStringLiteral("%1/%2").arg(m_doneCount).arg(m_totalCount));
    m_countLabel->setToolTip(tr("已完成 %1/%2").arg(m_doneCount).arg(m_totalCount));

    scheduleMeasure(); // 下一帧按最终宽度/行高同步高度并平滑跟到位移动画
}

void TodoCard::rebuildRows(const QJsonArray &todos)
{
    clearRows();

    m_totalCount = todos.size();
    m_doneCount = 0;

    for (const QJsonValue &value : todos)
    {
        const QJsonObject obj = value.toObject();
        const QString text = obj.value(QLatin1String("content")).toString();
        const QString status = obj.value(QLatin1String("status")).toString();

        // 三态归一：in_progress→active（最强权重）、completed→done（降级留痕）、
        // 其余/未知一律按 pending 渲染（对后端扩展状态前向兼容）
        const bool active = status == QLatin1String("in_progress");
        const bool done = status == QLatin1String("completed");
        if (done)
            m_doneCount++;
        const QLatin1String rowStatus = active ? QLatin1String("active")
                                               : (done ? QLatin1String("done") : QLatin1String("pending"));

        auto *row = new QWidget(m_list);
        row->setObjectName("todoRow");
        row->setProperty("status", QString::fromLatin1(rowStatus.data(), rowStatus.size()));
        row->setFixedHeight(kRowHeight);
        row->setToolTip(text);

        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 0, 10, 0);
        rowLayout->setSpacing(8);

        // 状态记号：文本字形（○ 待办 / ● 进行中 / ✓ 完成），颜色由 QSS 行状态选择器控制
        auto *mark = new QLabel(row);
        mark->setObjectName("todoMark");
        mark->setFixedSize(16, 16);
        mark->setAlignment(Qt::AlignCenter);
        mark->setText(active ? QStringLiteral("\u25CF")   // ●
                             : (done ? QStringLiteral("\u2713")  // ✓
                                     : QStringLiteral("\u25CB"))); // ○

        auto *textLabel = new QLabel(row);
        textLabel->setObjectName("todoText");
        textLabel->setTextInteractionFlags(Qt::NoTextInteraction);

        rowLayout->addWidget(mark);
        rowLayout->addWidget(textLabel, 1);
        m_listLayout->addWidget(row);

        m_rows.append(Row{row, textLabel, text});
    }

    refreshRowTexts();
    updateGeometry();
}

void TodoCard::clearRows()
{
    for (const Row &row : m_rows)
        delete row.widget; // 同时从列表布局摘除
    m_rows.clear();
}

QString TodoCard::elidedFor(const Row &row, int avail) const
{
    QFontMetrics fm(row.textLabel->font());
    return fm.elidedText(row.fullText, Qt::ElideRight, avail);
}

void TodoCard::refreshRowTexts()
{
    // 行可用宽 = 视口宽 - 行左侧透明描边 2 - 行边距 10+10 - 记号 16 - 间距 8 - 余量 2
    const int avail = qMax(40, m_scroll->viewport()->width() - (2 + 10 + 16 + 8 + 10 + 2));
    for (const Row &row : m_rows)
        row.textLabel->setText(elidedFor(row, avail));
}

void TodoCard::refreshIcons()
{
    const FluTheme theme = FluThemeUtils::getUtils()->getTheme();
    m_arrowLabel->setPixmap(FluIconUtils::getFluentIconPixmap(
        m_expanded ? FluAwesomeType::ChevronUp : FluAwesomeType::ChevronDown, theme, 14, 14));
}

void TodoCard::scheduleMeasure()
{
    // 不复用基类实现的动画守卫：该守卫防的是流式 token 风暴下"动画中途重测覆盖
    // 终点回弹"；本类测量即重定向动画（measureContent 直接 startHeightAnimation），
    // 动画中途来新数据若被守卫拦下，终点仍按旧行数计算，行会被裁剪。
    // 触发源均为低频用户事件（setTodos/块宽变化），无风暴风险。
    // 不能以 &CollapsibleBlock::measureContent 取基类 protected 成员（C2248），
    // 用 lambda 经 this 虚派发落到本类 measureContent
    QTimer::singleShot(0, this, [this] { measureContent(); });
}

void TodoCard::measureContent()
{
    // 列表自然高度纯由行数决定（单行等高，无需文档测量）；封顶后滚动条占据的宽度
    // 由 QScrollArea 自行处理，行省略宽经 onGeometryApplied 随视口刷新
    const int n = m_rows.size();
    const int naturalHeight = n == 0
                                  ? 0
                                  : kListVerticalMargin * 2 + n * kRowHeight + (n - 1) * kListSpacing;
    m_fullContentHeight = qMin(naturalHeight, expandedHeightCap());

    // 依据当前展开态把内容高度平滑跟到新值（数量变化时的"就地刷新"动效仅此一处，克制）；
    // 目标与当前可见高度一致则无需起动画
    const int target = m_expanded ? m_fullContentHeight : 0;
    if (m_contentHeight == target)
    {
        updateGeometry();
        return;
    }
    startHeightAnimation(target);
}

void TodoCard::onGeometryApplied()
{
    // 行文本省略宽随视口变化（高度不随宽度变，无需重测/重排）
    refreshRowTexts();
}
