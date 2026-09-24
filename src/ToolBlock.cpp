#include "ToolBlock.h"
#include "ToolTagKind.h"
#include "ToolNames.h" // 工具名集中常量（lcc a6d29b9）；"memory" 为 UI 伪键保持字面量

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>

#include <FluUtils.h>

ToolBlock::ToolBlock(QWidget *parent) : CollapsibleBlock(parent)
{
    // 展开限高恒为 kMaxOutputHeight（基类默认 expandedHeightCap 即返回该值）
    m_maxExpandedHeight = kMaxOutputHeight;

    // ---- 标题栏：bash 为 [$ + 已执行]，其余工具为 [等宽工具名标签 + 中文词条]，
    //      再接单行省略的关键参数与折叠箭头 ----
    auto *headerLayout = initHeader("toolHeader", 12, 4, 10, 4);

    m_iconLabel = createGlyphLabel("toolIcon");
    m_iconLabel->setText(QStringLiteral("$"));

    // 非 bash 工具的等宽名字标签（read_file / write_file / ...），默认隐藏；
    // 与 $ 提示符互斥显示，隐藏时 QHBoxLayout 不计入宽度与间距
    m_tagLabel = new QLabel(m_header);
    m_tagLabel->setObjectName("toolTag");
    m_tagLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_tagLabel->hide();

    m_titleLabel = createTitleLabel("toolTitle");
    m_titleLabel->setText(tr("已执行"));

    m_summaryLabel = new QLabel(m_header);
    m_summaryLabel->setObjectName("toolCommand");
    m_summaryLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    m_arrowLabel = createGlyphLabel("toolArrow");

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_tagLabel);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_summaryLabel, 1);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 输出内容区：纯文本，尺寸固定为 min(自然高度, 上限)，动画期间仅平移 ----
    initContent("toolOutput");

    // 主题装配必须在子类构造尾：initTheme 内虚派发 refreshIcons 需要
    // m_arrowLabel 已赋值（基类构造期调虚函数的经典陷阱，勿复制 BasePage 债务）
    initTheme("ToolBlock.qss");

    initCollapsed();
}

QString ToolBlock::toolTitleText(const QString &toolName)
{
    // 中文完成词条：与 lcc 各工具语义对齐，朴素直译；未知工具回退"已执行"
    if (toolName == ToolNames::BASH)
        return tr("已执行");
    if (toolName == ToolNames::READ_FILE)
        return tr("已读取");
    if (toolName == ToolNames::WRITE_FILE)
        return tr("已写入");
    if (toolName == ToolNames::EDIT_FILE)
        return tr("已编辑");
    if (toolName == ToolNames::GLOB)
        return tr("已查找");
    if (toolName == ToolNames::TASK)
        return tr("已代办"); // 子代理代为完成该任务（s06 task 工具）
    if (toolName == ToolNames::LOAD_SKILL)
        return tr("已加载"); // 按技能名载入 SKILL.md 全文（s07 load_skill 工具）
    if (toolName == ToolNames::COMPACT)
        return tr("已压缩"); // 压缩会话上下文释放额度（s08 compact 工具）
    if (toolName == QLatin1String("memory"))
        return tr("已记忆"); // 记忆写入/合并等事件的合成卡片（s09 记忆系统）
    if (toolName == ToolNames::CREATE_TASK)
        return tr("已建任务"); // 创建任务节点（s10 任务系统）
    if (toolName == ToolNames::UPDATE_TASK)
        return tr("已连依赖"); // 更新任务/建立依赖边（s10 任务系统）
    if (toolName == ToolNames::LIST_TASKS)
        return tr("任务清单"); // 列出全部任务概览（s10 任务系统）
    if (toolName == ToolNames::GET_TASK)
        return tr("任务详情"); // 查看单个任务详情（s10 任务系统）
    if (toolName == ToolNames::CLAIM_TASK)
        return tr("已认领"); // 认领任务（s10 任务系统）
    if (toolName == ToolNames::COMPLETE_TASK)
        return tr("已完成"); // 完成任务（s10 任务系统）
    if (toolName == ToolNames::SCHEDULE_CRON)
        return tr("已排定时"); // 登记定时任务（s12 cron 系统）
    if (toolName == ToolNames::LIST_CRONS)
        return tr("定时清单"); // 列出定时任务（s12 cron 系统）
    if (toolName == ToolNames::CANCEL_CRON)
        return tr("已取消定时"); // 取消定时任务（s12 cron 系统）
    return tr("已执行");
}

void ToolBlock::setToolExecution(const QString &toolName, const QString &summary, const QString &output)
{
    // live 进行态收口：停轮播（基类 stopLiveTimer 复位 m_live），标题/标签由下方按终态重建
    stopLiveTimer();

    m_toolName = toolName;
    m_summary = summary;

    // 头部呈现方案：bash 保留 "$" 提示符观感；其余工具隐藏 $、显示等宽工具名标签。
    // 中文词条统一承担"动作 + 完成态"语义，工具名标签承担"哪个工具"。
    const bool prompt = usesPromptGlyph();
    m_iconLabel->setVisible(prompt);
    m_tagLabel->setVisible(!prompt);
    if (!prompt)
        m_tagLabel->setText(toolName);
    // 工具名标签按语义类别着色：这里只打类别属性 toolTagKind，具体颜色由主题 QSS 的
    // [toolTagKind=...] 决定；未知工具归 other，沿用中性小片底色。
    ToolTagKind::applyTo(m_tagLabel, toolName);
    m_titleLabel->setText(toolTitleText(toolName));

    refreshSummaryLabel();

    // 输出走纯文本路径（50k 字符内性能可控），颜色/等宽字体由 QSS 控制
    m_content->setPlainText(output.isEmpty() ? tr("(无输出)") : output);
}

void ToolBlock::startLive(const QString &liveTitle)
{
    if (m_live)
        return;
    m_live = true;
    m_liveTitle = liveTitle;
    m_liveDots = 0;

    // live 期工具身份未知：隐藏 $ 提示符与工具名标签，仅留轮播标题 + 箭头
    m_iconLabel->hide();
    m_tagLabel->hide();
    m_summaryLabel->clear();
    m_titleLabel->setText(liveText());

    // 圆点轮播骨架（400ms 相位 0..3 循环）已下沉基类
    startLiveTimer();
}

QString ToolBlock::liveText() const
{
    return m_liveTitle + QStringLiteral(".").repeated(m_liveDots);
}

QString ToolBlock::singleLineSummary() const
{
    // 头部单行显示：换行/制表压成空格，连续空白折叠（tooltip 与展开输出保留原文）
    QString line = m_summary;
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    line.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return line.simplified();
}

void ToolBlock::refreshSummaryLabel()
{
    if (m_summary.isEmpty())
    {
        m_summaryLabel->setText(QString());
        m_summaryLabel->setToolTip(QString());
        return;
    }

    const QString text = singleLineSummary();

    // 头部可用宽度 = 块宽 - 固定装饰（左右边距 12+10、前置标签、箭头 16、3 段间距 8*3）- 标题宽。
    // 前置标签：bash 为 16px 的 $；其余为按内容自适应的等宽工具名标签（sizeHint 含 QSS padding）
    const int leadWidth = usesPromptGlyph()
                              ? m_iconLabel->width()
                              : m_tagLabel->sizeHint().width();
    static constexpr int kHeaderChrome = 12 + 10 + 16 + 8 * 3;
    const int avail = qMax(40, width() - kHeaderChrome - leadWidth
                                  - m_titleLabel->sizeHint().width());

    QFontMetrics fm(m_summaryLabel->font());
    m_summaryLabel->setText(fm.elidedText(text, Qt::ElideMiddle, avail));
    m_summaryLabel->setToolTip(text);
    m_header->setToolTip(usesPromptGlyph()
                             ? text
                             : QStringLiteral("%1  %2").arg(m_toolName, text));
}

void ToolBlock::onGeometryApplied()
{
    // 基类 resizeEvent 已按块宽定位头部/内容几何，此处重排关键参数省略
    refreshSummaryLabel();
}

void ToolBlock::refreshIcons()
{
    const FluTheme theme = FluThemeUtils::getUtils()->getTheme();
    m_arrowLabel->setPixmap(FluIconUtils::getFluentIconPixmap(
        m_expanded ? FluAwesomeType::ChevronUp : FluAwesomeType::ChevronDown, theme, 14, 14));
}
