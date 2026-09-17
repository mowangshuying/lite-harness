#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QLabel;
class QPropertyAnimation;
class QEvent;
class QResizeEvent;
class QTimer;

// 工具执行折叠块（对齐 ThinkingBlock 的视觉与交互语言）：
//   标题栏（bash：$ 提示符；其余工具：等宽工具名标签 + 中文完成词条 + 单行省略的关键参数）
//   + 可展开/折叠的输出内容区。
// 数据来源为 AgentLoop::toolOutputReady（工具执行完成后发射，因此头部恒为完成态）；
// 例外是记忆沉淀阶段：startLive 先挂"进行中"圆点轮播标题，setToolExecution 到达后收终态。
// 动画机制与 ThinkingBlock / FluExpander 同源：QPropertyAnimation 驱动
// "contentHeight"，setContentHeight 内同步向上遍历父链逐帧 resize；头部以
// stackUnder 叠放在内容之上，内容从头部背后滑出/收回（纯手动几何，无外层布局）。
class ToolBlock : public FluWidget
{
    Q_OBJECT
    Q_PROPERTY(int contentHeight READ contentHeight WRITE setContentHeight)

public:
    // 输出可见区最大高度（px）：超限后内容区内部滚动，防止撑爆气泡
    static constexpr int kMaxOutputHeight = 160;

    explicit ToolBlock(QWidget *parent = nullptr);

    // 工具名 -> 中文完成词条（已执行/已读取/...），未知工具回退"已执行"。
    // 公开静态：供会话页兜底气泡复用同一套文案
    static QString toolTitleText(const QString &toolName);

    // 头部展示"哪个工具 + 关键参数"（bash 的 summary 即命令行，其余为 path/pattern），
    // 展开区展示输出原文。若此前 startLive 进入过 live 态，本次调用自动收终态
    void setToolExecution(const QString &toolName, const QString &summary, const QString &output);

    // live 进行态（记忆沉淀阶段专用）：标题为 liveTitle + 圆点轮播（同 ThinkingBlock），
    // 隐藏 $/工具名标签（工具身份此刻未知）；setToolExecution 到达自动收终态
    void startLive(const QString &liveTitle);

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    int contentHeight() const { return m_contentHeight; }
    void setContentHeight(int h);

signals:
    void expandedChanged(bool expanded);
    // 内容区高度随动画进度变化，供外部（气泡/会话页）跟随刷新布局
    void sizeChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void toggleExpanded();
    void updateThemeIcons();
    void refreshSummaryLabel();       // 按当前宽度对关键参数做中部省略
    QString singleLineSummary() const;
    QString liveText() const;         // live 态标题：m_liveTitle + 轮播圆点
    bool usesPromptGlyph() const { return m_toolName == QLatin1String("bash"); }
    void scheduleMeasure();
    void measureContent();
    int scrollbarExtentWidth() const;
    void startExpandAnimation();

private:
    QWidget *m_header = nullptr;
    QLabel *m_iconLabel = nullptr;    // bash 专用 "$" 提示符（QSS 着色，等宽字体）
    QLabel *m_tagLabel = nullptr;     // 非 bash 工具：等宽工具名标签（read_file 等）
    QLabel *m_titleLabel = nullptr;   // 中文完成词条（"已执行"等）
    QLabel *m_summaryLabel = nullptr; // 关键参数（等宽观感，单行中部省略）
    QLabel *m_arrowLabel = nullptr;
    QTextBrowser *m_content = nullptr; // 输出区：尺寸固定为 min(自然高度, 上限)

    QString m_toolName;               // 工具名（bash/read_file/write_file/edit_file/glob/...）
    QString m_summary;                // 关键参数原文（省略显示 + tooltip 全文）
    bool m_live = false;              // live 进行态（startLive 置位，setToolExecution 收口）
    QString m_liveTitle;              // live 态标题文本（如"记忆整理中"）
    int m_liveDots = 0;               // 圆点轮播相位 0..3
    QTimer *m_liveTimer = nullptr;    // 懒建 400ms 轮播定时器
    int m_fullContentHeight = 0;      // 展开时输出区完整高度（min(自然高度, 上限)，随宽度重测）
    bool m_expanded = false;
    bool m_animating = false;         // 动画进行中：禁止 resizeEvent 重新测量
    int m_contentHeight = 0;          // 当前内容可见高度（0=完全折叠），动画驱动属性
    QPropertyAnimation *m_anim = nullptr;
};
