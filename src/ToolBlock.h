#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QLabel;
class QPropertyAnimation;
class QEvent;
class QResizeEvent;

// 工具执行折叠块（对齐 ThinkingBlock 的视觉与交互语言）：
//   标题栏（$ 提示符 + "已执行" + 单行省略的命令）+ 可展开/折叠的输出内容区。
// 数据来源为 AgentLoop::toolOutputReady（工具执行完成后发射，因此头部恒为完成态）。
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

    // 命令与输出：头部展示单行省略的命令，展开区展示输出原文
    void setToolExecution(const QString &command, const QString &output);

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
    void refreshCommandLabel();      // 按当前宽度对命令做中部省略
    QString singleLineCommand() const;
    void scheduleMeasure();
    void measureContent();
    int scrollbarExtentWidth() const;
    void startExpandAnimation();

private:
    QWidget *m_header = nullptr;
    QLabel *m_iconLabel = nullptr;    // "$" 提示符（QSS 着色，等宽字体）
    QLabel *m_titleLabel = nullptr;   // "已执行"
    QLabel *m_commandLabel = nullptr; // 命令（等宽观感，单行中部省略）
    QLabel *m_arrowLabel = nullptr;
    QTextBrowser *m_content = nullptr; // 输出区：尺寸固定为 min(自然高度, 上限)

    QString m_command;                // 命令原文（省略显示 + tooltip 全文）
    int m_fullContentHeight = 0;      // 展开时输出区完整高度（min(自然高度, 上限)，随宽度重测）
    bool m_expanded = false;
    bool m_animating = false;         // 动画进行中：禁止 resizeEvent 重新测量
    int m_contentHeight = 0;          // 当前内容可见高度（0=完全折叠），动画驱动属性
    QPropertyAnimation *m_anim = nullptr;
};
