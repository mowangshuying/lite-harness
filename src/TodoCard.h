#pragma once

#include <FluWidget.h>
#include <QJsonArray>
#include <QVector>

class QLabel;
class QScrollArea;
class QVBoxLayout;
class QPropertyAnimation;
class QEvent;
class QResizeEvent;

// 会话任务清单卡片（对齐 lcc s05 的 "Current Tasks" 面板，GUI 常驻形态）：
//   会话流中的持久状态面板 —— 首次 todoUpdated 时由会话页挂载到流末尾，
//   此后每次更新仅就地刷新列表内容（todo 全量替换语义），不随回合增殖。
//   头部 = 标题"任务清单" + 完成计数 "done/total" + 折叠箭头；
//   条目三态：pending 空心圆 / in_progress 琥珀实心+淡染底行 / completed 绿勾+暗淡文字。
// 折叠动画与 ThinkingBlock / ToolBlock 同族机制：QPropertyAnimation 驱动
//   "contentHeight"，setContentHeight 内同步向上遍历父链逐帧 resize；头部以
//   stackUnder 叠放在内容之上，内容从头部背后滑出/收回（头部不透明底色负责遮挡）。
// 数据来源为 AgentLoop::todoUpdated：元素 {content: string,
//   status: "pending"|"in_progress"|"completed"}；空列表时整卡隐藏（保留挂载位，待再现）。
class TodoCard : public FluWidget
{
    Q_OBJECT
    Q_PROPERTY(int contentHeight READ contentHeight WRITE setContentHeight)

public:
    // 列表可见区最大高度（px）：超限后列表区内部滚动，防止超长任务清单撑爆会话流
    static constexpr int kMaxListHeight = 260;

    explicit TodoCard(QWidget *parent = nullptr);

    // 全量替换任务列表并就地刷新（计数、行、高度）；空列表 => 隐藏整卡
    void setTodos(const QJsonArray &todos);

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    int contentHeight() const { return m_contentHeight; }
    void setContentHeight(int h);

signals:
    void expandedChanged(bool expanded);
    // 内容区高度变化（动画/更新），供外部跟随刷新布局
    void sizeChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Row
    {
        QWidget *widget = nullptr;
        QLabel *textLabel = nullptr;
        QString fullText; // 任务原文（单行中部省略 + tooltip 全文）
    };

    void rebuildRows(const QJsonArray &todos);
    void clearRows();
    void refreshRowTexts();           // 按视口宽度对各条任务文本做省略
    QString elidedFor(const Row &row, int avail) const;
    void updateThemeIcons();
    void syncHeight();                 // 依据展开态把内容高度平滑跟到新测量值
    void measureContent();             // 列表自然高度 = 行数 * 行高 + 间距 + 上下边距（封顶）
    void scheduleMeasure();

private:
    QWidget *m_header = nullptr;
    QLabel *m_titleLabel = nullptr;    // "任务清单"
    QLabel *m_countLabel = nullptr;    // "done/total" 完成计数
    QLabel *m_arrowLabel = nullptr;
    QScrollArea *m_scroll = nullptr;   // 列表容器：封顶后内部滚动（手动几何，随动画平移）
    QWidget *m_list = nullptr;         // 行宿主，QVBoxLayout 纵排
    QVBoxLayout *m_listLayout = nullptr;

    QVector<Row> m_rows;
    int m_totalCount = 0;
    int m_doneCount = 0;

    int m_fullContentHeight = 0;       // 展开时列表完整高度（min(自然高度, 上限)，随宽度/行数重测）
    bool m_expanded = true;            // 状态面板默认展开：保留全貌是 lcc 面板的核心信息
    bool m_animating = false;          // 动画进行中：禁止 resizeEvent 重新测量
    int m_contentHeight = 0;           // 当前内容可见高度（0=完全折叠），动画驱动属性
    QPropertyAnimation *m_anim = nullptr;
};
