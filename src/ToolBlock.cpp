#include "ToolBlock.h"
#include "ToolTagKind.h"
#include "ToolNames.h" // 工具名集中常量（lcc a6d29b9）；"memory" 为 UI 伪键保持字面量
#include "AgentConstants.h" // kSubagentProgressMaxLines 进度行上限

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>

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

    // 成败字形位（B1）：终态卡点亮 ✓/✕（失败红色由 QSS [outcome="fail"] 决定），
    // live 进行态隐藏；未知态时保持中性隐藏，隐藏件不占宽度与间距
    m_outcomeLabel = createGlyphLabel("toolOutcome");
    m_outcomeLabel->hide();

    m_summaryLabel = new QLabel(m_header);
    m_summaryLabel->setObjectName("toolCommand");
    m_summaryLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    m_arrowLabel = createGlyphLabel("toolArrow");

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addWidget(m_tagLabel);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_outcomeLabel);
    headerLayout->addWidget(m_summaryLabel, 1);
    headerLayout->addWidget(m_arrowLabel);

    // ---- 输出内容区：纯文本，尺寸固定为 min(自然高度, 上限)，动画期间仅平移 ----
    initContent("toolOutput");

    // 主题装配必须在子类构造尾：initTheme 内虚派发 refreshIcons 需要
    // m_arrowLabel 已赋值（基类构造期调虚函数的经典陷阱）
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
    if (toolName == ToolNames::SPAWN_TEAMMATE)
        return tr("已建队友"); // 创建队友（s13 团队系统）
    if (toolName == ToolNames::LIST_TEAMMATES)
        return tr("队友清单"); // 列出在簿队友（s13 团队系统）
    if (toolName == ToolNames::SEND_MESSAGE)
        return tr("已发消息"); // 向队友/Lead 投递消息（s13 团队系统）
    if (toolName == ToolNames::REQUEST_SHUTDOWN)
        return tr("已请下线"); // 请求队友下线（s13 团队系统）
    if (toolName == ToolNames::REQUEST_PLAN)
        return tr("已索计划"); // 要求队友先交计划（s13 团队系统）
    if (toolName == ToolNames::REVIEW_PLAN)
        return tr("已评计划"); // 批准/驳回队友计划（s13 团队系统）
    if (toolName == ToolNames::CREATE_WORKTREE)
        return tr("已建工作树"); // 创建并绑定任务 worktree（s13 团队系统）
    return tr("已执行");
}

QString ToolBlock::toolFailText(const QString &toolName)
{
    // 中文失败词条（B1）：与成功词条同密度朴素直译，未知工具回退"执行失败"
    if (toolName == ToolNames::BASH)
        return tr("执行失败");
    if (toolName == ToolNames::READ_FILE)
        return tr("读取失败");
    if (toolName == ToolNames::WRITE_FILE)
        return tr("写入失败");
    if (toolName == ToolNames::EDIT_FILE)
        return tr("编辑失败");
    if (toolName == ToolNames::GLOB)
        return tr("查找失败");
    if (toolName == ToolNames::TASK)
        return tr("代办失败"); // 子代理任务未达成（s06 task 工具）
    if (toolName == ToolNames::LOAD_SKILL)
        return tr("加载失败"); // 技能载入失败（s07 load_skill 工具）
    if (toolName == ToolNames::COMPACT)
        return tr("压缩失败"); // 上下文压缩失败（s08 compact 工具）
    if (toolName == QLatin1String("memory"))
        return tr("记忆失败"); // 记忆事件合成卡的失败态（s09 记忆系统）
    if (toolName == ToolNames::CREATE_TASK)
        return tr("建任务失败"); // 任务节点创建失败（s10 任务系统）
    if (toolName == ToolNames::UPDATE_TASK)
        return tr("依赖登记失败"); // 依赖边更新失败（s10 任务系统）
    if (toolName == ToolNames::LIST_TASKS)
        return tr("清单读取失败"); // 任务总览读取失败（s10 任务系统）
    if (toolName == ToolNames::GET_TASK)
        return tr("详情读取失败"); // 单任务详情读取失败（s10 任务系统）
    if (toolName == ToolNames::CLAIM_TASK)
        return tr("认领失败"); // 任务认领失败（s10 任务系统）
    if (toolName == ToolNames::COMPLETE_TASK)
        return tr("完结失败"); // 任务完结失败（s10 任务系统）
    if (toolName == ToolNames::SCHEDULE_CRON)
        return tr("定时登记失败"); // 定时任务登记失败（s12 cron 系统）
    if (toolName == ToolNames::LIST_CRONS)
        return tr("定时清单读取失败"); // 定时任务总览读取失败（s12 cron 系统）
    if (toolName == ToolNames::CANCEL_CRON)
        return tr("取消定时失败"); // 定时任务取消失败（s12 cron 系统）
    if (toolName == ToolNames::SPAWN_TEAMMATE)
        return tr("建队友失败"); // 队友创建失败（s13 团队系统）
    if (toolName == ToolNames::LIST_TEAMMATES)
        return tr("队友清单读取失败"); // 在簿队友读取失败（s13 团队系统）
    if (toolName == ToolNames::SEND_MESSAGE)
        return tr("消息投递失败"); // 团队消息投递失败（s13 团队系统）
    if (toolName == ToolNames::REQUEST_SHUTDOWN)
        return tr("下线请求失败"); // 队友下线请求失败（s13 团队系统）
    if (toolName == ToolNames::REQUEST_PLAN)
        return tr("索计划失败"); // 计划要求投递失败（s13 团队系统）
    if (toolName == ToolNames::REVIEW_PLAN)
        return tr("计划评审失败"); // 计划批复失败（s13 团队系统）
    if (toolName == ToolNames::CREATE_WORKTREE)
        return tr("工作树创建失败"); // worktree 创建失败（s13 团队系统）
    return tr("执行失败");
}

void ToolBlock::setToolExecution(const QString &toolName, const QString &summary, const QString &output, bool ok)
{
    // live 进行态收口：停轮播（基类 stopLiveTimer 复位 m_live），标题/标签由下方按终态重建
    stopLiveTimer();
    m_taskLive = false;

    m_toolName = toolName;
    m_summary = summary;

    // 头部呈现方案：bash 保留 "$" 提示符观感；其余工具隐藏 $、显示等宽工具名标签。
    // 中文词条统一承担"动作 + 成败态"语义（ok 选完成/失败词表），工具名标签承担"哪个工具"。
    const bool prompt = usesPromptGlyph();
    m_iconLabel->setVisible(prompt);
    m_tagLabel->setVisible(!prompt);
    if (!prompt)
        m_tagLabel->setText(toolName);
    // 工具名标签按语义类别着色：这里只打类别属性 toolTagKind，具体颜色由主题 QSS 的
    // [toolTagKind=...] 决定；未知工具归 other，沿用中性小片底色。
    // 类别色与状态色分层（ToolTagKind 头注释承诺）：成败只上字形位，不染标签。
    ToolTagKind::applyTo(m_tagLabel, toolName);
    m_titleLabel->setText(ok ? toolTitleText(toolName) : toolFailText(toolName));

    // 成败字形位点亮（B1，判定单源在 AgentLoop::isToolFailure，本类不嗅探输出）
    applyOutcome(ok ? "ok" : "fail", ok ? QStringLiteral("\u2713") : QStringLiteral("\u2715"));

    refreshSummaryLabel();

    // 输出走纯文本路径（50k 字符内性能可控），颜色/等宽字体由 QSS 控制；
    // task 卡带子代理进度日志（live 收口路径）：进度段 + 分隔行前置在结果之前，
    // 执行轨迹与终态输出同区保留可回看（进度行为 UI 瞬态，历史重放无此数据属预期）
    QString body = output.isEmpty() ? tr("(无输出)") : output;
    if (!m_subagentLines.isEmpty())
    {
        QStringList lines = displayedSubagentLines();
        lines.append(QString());
        lines.append(tr("──── 子代理最终结果 ────"));
        lines.append(QString());
        lines.append(body);
        body = lines.join(QLatin1Char('\n'));
    }
    m_content->setPlainText(body);
}

void ToolBlock::startLive(const QString &liveTitle)
{
    if (m_live)
        return;
    m_live = true;
    m_liveTitle = liveTitle;
    m_liveDots = 0;

    // live 期工具身份未知：隐藏 $ 提示符与工具名标签，仅留轮播标题 + 箭头；
    // 成败字形位同隐（结果尚未产生，不许预设）
    m_iconLabel->hide();
    m_tagLabel->hide();
    m_outcomeLabel->hide();
    m_summaryLabel->clear();
    m_titleLabel->setText(liveText());

    // 圆点轮播骨架（400ms 相位 0..3 循环）已下沉基类
    startLiveTimer();
}

QString ToolBlock::liveText() const
{
    return m_liveTitle + QStringLiteral(".").repeated(m_liveDots);
}

// ---- task 子代理 live 进度 ----

void ToolBlock::startTaskLive()
{
    if (m_taskLive)
        return;
    m_taskLive = true;
    m_live = true;
    m_liveTitle = tr("子代理执行中");
    m_liveDots = 0;

    // 与记忆 startLive 不同：工具身份恒知（task），保留等宽工具名标签按 delegate
    // 类别着色；头部形态 = [task 标签 + 轮播标题 + 最新进度行（关键参数位）]；
    // 成败字形位隐藏（子代理尚未收口，不许预设）
    m_toolName = ToolNames::TASK;
    m_iconLabel->hide();
    m_tagLabel->show();
    m_tagLabel->setText(ToolNames::TASK);
    ToolTagKind::applyTo(m_tagLabel, ToolNames::TASK);
    m_outcomeLabel->hide();
    m_titleLabel->setText(liveText());

    startLiveTimer();
    setExpanded(true);   // 自动展开露出进度区（ThinkingBlock 流式先例；空内容测得≈行距高，
                         // 首行到达经 contentsChanged→scheduleMeasure 跟高跳变）
}

void ToolBlock::appendSubagentProgress(int turnNo, const QString &toolName, const QString &summary)
{
    // 单行 = 「第 N 轮 · 工具名  关键参数」；summary 压单行（bash 命令行可能含换行）
    QString line = tr("第 %1 轮").arg(turnNo) + QStringLiteral(" · ") + toolName;
    if (!summary.isEmpty())
        line += QStringLiteral("  ") + summary.simplified();

    // 超限策略：滑窗丢最旧（上限见 AgentConst::kSubagentProgressMaxLines），
    // 省略条数在日志顶部常驻标注——整体重组写法下提示行只有一条、永不重复
    while (m_subagentLines.size() >= AgentConst::kSubagentProgressMaxLines)
    {
        m_subagentLines.removeFirst();
        ++m_subagentDropped;
    }
    m_subagentLines.append(line);

    renderSubagentLog();

    // 展开态钉底跟随最新行（ThinkingBlock::appendLiveText 同款范式：延一帧等测高后取最大）
    if (m_expanded)
    {
        QTimer::singleShot(0, this, [this]() {
            QScrollBar *bar = m_content->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    }

    // 头部关键参数位同步为最新行（折叠态也可见进展；tooltip 全文，宽度省略按块宽重算）
    m_summary = line;
    refreshSummaryLabel();
}

void ToolBlock::finishTaskLiveAborted()
{
    if (!m_taskLive)
        return;
    m_taskLive = false;
    stopLiveTimer();

    // 中断终局：task 未达成「已代办」语义，标题切「已中断」，头部关键参数位保留最新
    // 进度行作现场线索；折叠但日志不销毁（stop/error 终局后 toolOutputReady("task")
    // 不再到达，此处是唯一收口点——由气泡 finishStreaming 兜底驱动）
    m_titleLabel->setText(tr("已中断"));
    applyOutcome("stopped", QStringLiteral("\u25a0")); // 灰方块：无结果终态（与常规卡同纪律）
    setExpanded(false);
}

// ---- s13 队友 live 卡三件套（观测面 a；信号语义见 AgentLoop::teammateProgress/
// teammateSettled 注释，type/outcome 均为数据域 token，本类只负责译词条展示）----

namespace {
// 队友活动行 content 段字符上限（超限截断加省略号）：AgentConstants.h 不在本轮写域，
// 文件内常量登记偏差——纯观感参数，不涉协议/数据兼容
constexpr int kTeammateContentMaxChars = 120;
} // namespace

void ToolBlock::startTeammateLive(const QString &teammateName)
{
    if (m_teammateLive)
        return;
    m_teammateLive = true;
    m_live = true;
    m_teammateName = teammateName;
    m_liveTitle = tr("队友 %1 执行中").arg(teammateName);
    m_liveDots = 0;

    // 身份标签用派生工具名 spawn_teammate（ToolTagKind plan 类别着色，与团队工具卡
    // 同源色系；成败字形位在终局前隐藏——队友没有「工具成败」只有生命周期）
    m_toolName = ToolNames::SPAWN_TEAMMATE;
    m_iconLabel->hide();
    m_tagLabel->show();
    m_tagLabel->setText(ToolNames::SPAWN_TEAMMATE);
    ToolTagKind::applyTo(m_tagLabel, ToolNames::SPAWN_TEAMMATE);
    m_outcomeLabel->hide();
    m_titleLabel->setText(liveText());

    startLiveTimer();
    setExpanded(true);   // 自动展开供活动行可见（task 卡先例；用户折叠后不跟高）
}

void ToolBlock::appendTeammateProgress(const QString &type, const QString &content)
{
    // 数据域 token → 中文词条；未知 token 原样透传（宁可显协议名，不编造误导词条）
    QString label;
    if (type == QLatin1String("turn"))
        label = tr("回合推进");
    else if (type == QLatin1String("result"))
        label = tr("交付成果");
    else if (type == QLatin1String("error"))
        label = tr("出错");
    else if (type == QLatin1String("idle_notification"))
        label = tr("空闲待命");
    else
        label = type;

    // 轮播标题随相位切换：空闲期不再显「执行中」；result/error/turn 均属工作相位切回
    // 执行中（词条与 startTeammateLive 初值同源字面）
    if (type == QLatin1String("idle_notification"))
        m_liveTitle = tr("队友 %1 空闲待命").arg(m_teammateName);
    else
        m_liveTitle = tr("队友 %1 执行中").arg(m_teammateName);
    if (m_live)
        m_titleLabel->setText(liveText());

    // 单行 = 「词条 · 内容摘要」；content 为队友侧文本可能多行/超长：压单行 + 截断
    //（上限文件内常量——AgentConstants.h 不在本轮写域，偏差已登记）
    QString line = label;
    if (!content.isEmpty())
    {
        QString flat = content.simplified();
        if (flat.size() > kTeammateContentMaxChars)
            flat = flat.left(kTeammateContentMaxChars) + QStringLiteral("…");
        line += QStringLiteral(" · ") + flat;
    }

    // 超限策略/钉底跟随/头部关键参数位同步：与 appendSubagentProgress 同款（复用
    // 同一 m_subagentLines 窗口与渲染链——task/队友两形态互斥共槽，一卡只走一条链）
    while (m_subagentLines.size() >= AgentConst::kSubagentProgressMaxLines)
    {
        m_subagentLines.removeFirst();
        ++m_subagentDropped;
    }
    m_subagentLines.append(line);
    renderSubagentLog();
    if (m_expanded)
    {
        QTimer::singleShot(0, this, [this]() {
            QScrollBar *bar = m_content->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    }
    m_summary = line;
    refreshSummaryLabel();
}

void ToolBlock::finishTeammateLive(const QString &outcome)
{
    if (!m_teammateLive)
        return;                          // 幂等：兜底扫与逐名终局不双收口
    m_teammateLive = false;
    stopLiveTimer();

    // 三种终局语义（见 AgentLoop::teammateSettled 注释）：自报完成点亮 ok 字形；
    // 退出/中止共用灰方块「无结果终态」（与 task 中断/常规卡 stopped 同纪律）
    if (outcome == QLatin1String("completed"))
    {
        m_titleLabel->setText(tr("队友 %1 已交付").arg(m_teammateName));
        applyOutcome("ok", QStringLiteral("\u2713"));
    }
    else if (outcome == QLatin1String("exited"))
    {
        m_titleLabel->setText(tr("队友 %1 已退出").arg(m_teammateName));
        applyOutcome("stopped", QStringLiteral("\u25a0"));
    }
    else
    {
        m_titleLabel->setText(tr("队友 %1 已中止").arg(m_teammateName));
        applyOutcome("stopped", QStringLiteral("\u25a0"));
    }
    setExpanded(false);
}

// ---- B3 常规工具事前 live 卡 ----

void ToolBlock::startToolLive(const QString &toolName, const QString &summary)
{
    if (m_live)
        return;
    m_live = true;
    m_liveTitle = tr("执行中");
    m_liveDots = 0;

    // 身份可见形态与终态卡同款（bash $ / 其余等宽标签 + 类别着色）——toolStarted
    // 已带 AgentLoopDetail::toolSummary 同源摘要，头部即展示「哪个工具 + 关键参数」的进行态版本；
    // 成败字形位隐藏（结果尚未产生）
    m_toolName = toolName;
    m_summary = summary;
    const bool prompt = usesPromptGlyph();
    m_iconLabel->setVisible(prompt);
    m_tagLabel->setVisible(!prompt);
    if (!prompt)
        m_tagLabel->setText(toolName);
    ToolTagKind::applyTo(m_tagLabel, toolName);
    m_outcomeLabel->hide();
    m_titleLabel->setText(liveText());
    refreshSummaryLabel();

    // 圆点轮播骨架（400ms 相位 0..3 循环）已下沉基类；保持 initCollapsed 折叠形态，
    // 不自动展开（区别 task 卡）——高频工具执行时防内容区闪跳
    startLiveTimer();
}

void ToolBlock::finishToolLiveAborted()
{
    // 仅 live 进行态可收中断终局（已被 setToolExecution 收过终态/从未 live 均 no-op）
    if (!m_live)
        return;
    stopLiveTimer();

    // 中断终局（stop/error 收口时该工具不再经 toolOutputReady 回流）：标题切
    // 「已停止」+ 灰方块字形位，折叠留痕；输出区留空属预期（结果从未产生）
    m_titleLabel->setText(tr("已停止"));
    applyOutcome("stopped", QStringLiteral("\u25a0"));
    setExpanded(false);
}

void ToolBlock::applyOutcome(const char *state, const QString &glyph)
{
    // 照 PermissionCard::applyTrace 模式：动态属性喂 QSS 选择器 + unpolish/polish
    // 重取样式，字形用文本字符零新图标资源
    m_outcomeLabel->setText(glyph);
    m_outcomeLabel->setProperty("outcome", QLatin1String(state));
    m_outcomeLabel->show();
    style()->unpolish(m_outcomeLabel);
    style()->polish(m_outcomeLabel);
}

QStringList ToolBlock::displayedSubagentLines() const
{
    QStringList display = m_subagentLines;
    if (m_subagentDropped > 0)
        display.prepend(tr("…更早 %1 条进度已省略").arg(m_subagentDropped));
    return display;
}

void ToolBlock::renderSubagentLog()
{
    m_content->setPlainText(displayedSubagentLines().join(QLatin1Char('\n')));
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
    // 成败字形位点亮时（终态卡）再占 16 宽 + 1 段间距；live 期隐藏不计
    const int outcomeExt = m_outcomeLabel->isVisible() ? 16 + 8 : 0;
    const int avail = qMax(40, width() - kHeaderChrome - outcomeExt - leadWidth
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
