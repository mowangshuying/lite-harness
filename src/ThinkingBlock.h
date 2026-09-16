#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QLabel;
class QPropertyAnimation;
class QTimer;
class QEvent;
class QResizeEvent;

// 思考过程折叠块（Ollama 风格）：
//   标题栏（图标 + "思考了 N 秒" + 折叠箭头）+ 可展开/折叠的思考内容区。
// 动画机制与 FluExpander 同源（用户实测无抖动）：QPropertyAnimation 驱动
// "contentHeight"（像素高度）属性，setContentHeight 内同步向上遍历父链
// 逐帧 resize（直到 window 或滚动区 viewport 为止）；头部以 stackUnder 叠放
// 在内容之上，内容从头部背后滑出/收回（纯手动几何，无外层布局）。
class ThinkingBlock : public FluWidget
{
    Q_OBJECT
    Q_PROPERTY(int contentHeight READ contentHeight WRITE setContentHeight)

public:
    // 思考内容可见区最大高度（px）：ThinkingBlock 展开高度与流式思考气泡共用
    static constexpr int kMaxThinkingHeight = 150;

    explicit ThinkingBlock(QWidget *parent = nullptr);

    void setThinkingContent(const QString &thinkingText);
    void setThinkingDuration(int seconds);

    // ---- 流式进行态（思考生成期间占位展示）----
    // startLive：头部切换为「思考中」（圆点轮播），未被打扰时自动展开；
    // stopLive：恢复终态「思考了 N 秒」，未被打扰时自动折叠；
    // appendLiveText：增量纯文本追加，限高内滚动并跟随最新内容（钉底）。
    // 用户手动点过头部后，进行/终态切换不再自动改变展开状态（尊重用户操作）。
    void startLive();
    void stopLive(int seconds);
    void appendLiveText(const QString &delta);
    bool isLive() const { return m_live; }

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
    QString durationText() const;
    QString liveText() const;
    void scheduleMeasure();
    void measureContent();
    int scrollbarExtentWidth() const;
    void startExpandAnimation();

private:
    QWidget *m_header = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_arrowLabel = nullptr;
    QTextBrowser *m_content = nullptr;  // 内容区：尺寸固定为 min(自然高度, 上限)，动画期间仅靠 move 从头部背后滑出

    int m_durationSeconds = 0;      // 思考耗时（秒）
    int m_fullContentHeight = 0;    // 展开时内容区完整高度（由文档测量得到，随内容流式增长实时更新）
    bool m_expanded = false;
    bool m_animating = false;       // 动画进行中：禁止 resizeEvent 重新测量
    int m_contentHeight = 0;        // 当前内容可见高度（0=完全折叠），动画驱动属性
    QPropertyAnimation *m_anim = nullptr;

    // 流式进行态：头部「思考中」圆点轮播动画 + 用户手动操作记忆
    bool m_live = false;
    bool m_userInteracted = false;  // 用户手动展开/折叠过：进行/终态切换不再自动改展开状态
    QTimer *m_liveTimer = nullptr;
    int m_liveDots = 0;
};
