#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QLabel;
class QPropertyAnimation;
class QVBoxLayout;
class QEvent;
class QResizeEvent;

// 思考过程折叠块（Ollama 风格）：
//   标题栏（图标 + "思考了 N 秒" + 折叠箭头）+ 可展开/折叠的思考内容区。
// 流式结束后由 MessageBubbleWidget 创建，默认折叠为高度约 32px 的标题栏；
// 点击标题栏通过 QPropertyAnimation 驱动 expandProgress（0↔100），
// 内容区高度 = 完整内容高度 × progress / 100，动画自管理不涉及外部滚动。
class ThinkingBlock : public FluWidget
{
    Q_OBJECT
    Q_PROPERTY(int expandProgress READ expandProgress WRITE setExpandProgress)

public:
    explicit ThinkingBlock(QWidget *parent = nullptr);

    void setThinkingContent(const QString &thinkingText);
    void setThinkingDuration(int seconds);

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    int expandProgress() const { return m_expandProgress; }
    void setExpandProgress(int progress);

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
    void scheduleMeasure();
    void measureContent();
    void applyProgress();

private:
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_header = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_arrowLabel = nullptr;
    QTextBrowser *m_content = nullptr;

    int m_durationSeconds = 0;      // 思考耗时（秒）
    int m_fullContentHeight = 0;    // 展开时内容区完整高度（由文档测量得到）
    bool m_expanded = false;
    int m_expandProgress = 0;       // 0=完全折叠 100=完全展开
    QPropertyAnimation *m_anim = nullptr;
};