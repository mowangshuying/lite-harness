#pragma once

#include "CollapsibleBlock.h"

#include <QStringList>

class QLabel;

// 工具执行折叠块（对齐 ThinkingBlock 的视觉与交互语言）：
//   标题栏（bash：$ 提示符；其余工具：等宽工具名标签 + 中文动作词条 + 成败字形位 ✓/✕
//   + 单行省略的关键参数）+ 可展开/折叠的输出内容区。
// 数据来源为 AgentLoop::toolOutputReady（工具执行完成时发射并自带成败 ok，头部按
// ok 选完成/失败词条并点亮字形位）；事前形态由 toolStarted 驱动：常规工具
// startToolLive 先挂"执行中"圆点轮播（保持折叠一行头部），toolOutputReady 到达
// 就地收口；记忆沉淀阶段 startLive、task 子代理 startTaskLive 为两个既有特例。
// 折叠/动画/测量骨架已下沉 CollapsibleBlock（与 ThinkingBlock 逐行等价的部分）；
// 本类仅保留工具专属差异：头部双形态（$/工具名标签）、成败字形位、关键参数省略、中文词条表。
class ToolBlock : public CollapsibleBlock
{
    Q_OBJECT

public:
    // 输出可见区最大高度（px）：超限后内容区内部滚动，防止撑爆气泡
    static constexpr int kMaxOutputHeight = 160;

    explicit ToolBlock(QWidget *parent = nullptr);

    // 工具名 -> 中文完成词条（已执行/已读取/...），未知工具回退"已执行"。
    // 公开静态：供会话页兜底气泡复用同一套文案
    static QString toolTitleText(const QString &toolName);

    // 工具名 -> 中文失败词条（执行失败/读取失败/...），未知工具回退"执行失败"。
    // 与成功词条同密度朴素直译（B1 成败视觉）；公开静态：供会话页兜底气泡复用
    static QString toolFailText(const QString &toolName);

    // 头部展示"哪个工具 + 关键参数"（bash 的 summary 即命令行，其余为 path/pattern），
    // 展开区展示输出原文。若此前 startLive 进入过 live 态，本次调用自动收终态。
    // ok = AgentLoop::isToolFailure 取反的成败判定（B1 单源，UI 不嗅探输出）：
    // 驱动标题词条选型（完成/失败）与头部 ✓/✕ 字形位着色
    void setToolExecution(const QString &toolName, const QString &summary, const QString &output, bool ok);

    // live 进行态（记忆沉淀阶段专用）：标题为 liveTitle + 圆点轮播（同 ThinkingBlock），
    // 隐藏 $/工具名标签（工具身份此刻未知）；setToolExecution 到达自动收终态
    void startLive(const QString &liveTitle);

    // ---- B3 常规工具事前 live 卡（toolStarted → 建卡轮播，toolOutputReady →
    // setToolExecution 就地收口带成败字形；stop/error 终局 → finishToolLiveAborted 兜底）----

    // 常规工具进行态：身份可见形态照终态卡（bash 显 $、其余显等宽工具名标签 +
    // 类别着色），标题「执行中」圆点轮播，关键参数位即 summary；保持默认折叠
    // 一行头部（不自动展开——区别 task 卡，长命令高频执行时防内容区闪跳）
    void startToolLive(const QString &toolName, const QString &summary);

    // 中断终局（stop/error 收口时该工具不再经 toolOutputReady 回流）：停轮播、
    // 标题切「已停止」、字形位置灰、折叠；输出区留空属预期（结果从未产生）
    void finishToolLiveAborted();

    // ---- task 子代理 live 进度三件套（subagentProgress → 首行建卡，toolOutputReady
    // ("task") → setToolExecution 收口；stop/error 终局 → finishTaskLiveAborted 兜底）----

    // task 进行态：保留工具名标签（task，delegate 类别着色）+「子代理执行中」轮播标题，
    // 自动展开供进度行逐条可见（ThinkingBlock 流式展开同款；用户手动折叠后不再跟高）
    void startTaskLive();

    // 追加一条子代理内部活动行「第 N 轮 · 工具名  关键参数」：内容区整体重组（60 行
    // 量级成本可忽略），头部关键参数区同步为最新行；条数超 kSubagentProgressMaxLines
    // 丢弃最旧行并在顶部标注省略数。summary 可为空串（只显轮次与工具名）
    void appendSubagentProgress(int turnNo, const QString &toolName, const QString &summary);

    // 中断终态（stop/error 收口时 task 结果不再经 setToolExecution 回流）：标题切
    // 「已中断」并折叠，已积累的进度行保留可展开回看
    void finishTaskLiveAborted();

    // setExpanded / isExpanded / contentHeight / setContentHeight 继承自基类（API 冻结）

protected:
    // ---- CollapsibleBlock 钩子 ----
    void refreshIcons() override;    // 仅箭头（$ 与工具名标签着色由 QSS 控制）
    QString liveText() const override;
    void onGeometryApplied() override;  // 关键参数省略宽度随块宽变化

private:
    void refreshSummaryLabel();       // 按当前宽度对关键参数做中部省略
    QString singleLineSummary() const;
    bool usesPromptGlyph() const { return m_toolName == QLatin1String("bash"); }
    void renderSubagentLog();         // 进度窗口（含省略提示）整体写入内容区
    QStringList displayedSubagentLines() const;  // 原始行 + 顶部「更早 N 条已省略」提示行
    // 成败字形位统一打点（B1）：设属性喂 QSS 选择器 + 换字形 + unpolish/polish 重着色
    // （照 PermissionCard::applyTrace 模式，零新图标资源）；state 取 "ok"/"fail"/"stopped"
    void applyOutcome(const char *state, const QString &glyph);

    QLabel *m_iconLabel = nullptr;    // bash 专用 "$" 提示符（QSS 着色，等宽字体）
    QLabel *m_tagLabel = nullptr;     // 非 bash 工具：等宽工具名标签（read_file 等）
    QLabel *m_outcomeLabel = nullptr; // 成败字形位（✓/✕/■，QSS 按 outcome 属性着色；live 期隐藏）
    QLabel *m_summaryLabel = nullptr; // 关键参数（等宽观感，单行中部省略）

    QString m_toolName;               // 工具名（bash/read_file/write_file/edit_file/glob/...）
    QString m_summary;                // 关键参数原文（省略显示 + tooltip 全文）
    QString m_liveTitle;              // live 态标题文本（如"记忆整理中"）
    QStringList m_subagentLines;      // task 子代理进度行窗口（终态重组时前置到结果之前）
    int m_subagentDropped = 0;        // 超上限被裁剪的最旧进度条数
    bool m_taskLive = false;          // 处于 task live 进行态（轮播/中断终态标记）
};
