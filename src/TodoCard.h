#pragma once

#include "CollapsibleBlock.h"
#include <QJsonArray>
#include <QVector>

class QLabel;
class QScrollArea;
class QVBoxLayout;

// 会话任务清单卡片（对齐 lcc s05 的 "Current Tasks" 面板，GUI 常驻形态）：
//   会话流中的持久状态面板 —— 首次 todoUpdated 时由会话页挂载到流末尾，
//   此后每次更新仅就地刷新列表内容（todo 全量替换语义），不随回合增殖。
//   头部 = 标题"任务清单" + 完成计数 "done/total" + 折叠箭头；
//   条目三态：pending 空心圆 / in_progress 琥珀实心+淡染底行 / completed 绿勾+暗淡文字。
// 折叠骨架（头部点击/叠放滑动/contentHeight 动画/几何定位）全部下沉 CollapsibleBlock；
//   本类仅保留列表形态差异：三态行组装、行数公式测高（覆写 measureContent，
//   经基类 startHeightAnimation 平滑跟高），更新节奏覆写 scheduleMeasure（无流式）。
// 数据来源为 AgentLoop::todoUpdated：元素 {content: string,
//   status: "pending"|"in_progress"|"completed"}；空列表时整卡隐藏（保留挂载位，待再现）。
// 对外 API 冻结（前轮承诺）：ctor/setTodos/kMaxListHeight 不变；
//   setExpanded/isExpanded/contentHeight/expandedChanged/sizeChanged 直接继承基类。
class TodoCard : public CollapsibleBlock
{
    Q_OBJECT

public:
    // 列表可见区最大高度（px）：超限后列表区内部滚动，防止超长任务清单撑爆会话流
    static constexpr int kMaxListHeight = 260;

    explicit TodoCard(QWidget *parent = nullptr);

    // 全量替换任务列表并就地刷新（计数、行、高度）；空列表 => 隐藏整卡
    void setTodos(const QJsonArray &todos);

protected:
    // ---- 基类钩子 ----
    void refreshIcons() override;   // 箭头方向（ChevronUp/Down 随主题取色）
    // 无进行态：不覆写 liveText（基类默认空串，轮播定时器永不被启动）

    // 列表自然高度 = 行数 * 行高 + 间距 + 上下边距（封顶），平滑跟到新展开态
    void measureContent() override;
    // 去掉基类的动画期测量守卫：本类测量即重定向动画（理由见 cpp）
    void scheduleMeasure() override;
    // 行文本省略宽随视口变化（行高不随宽度变，几何定位后无需重测）
    void onGeometryApplied() override;

private:
    struct Row
    {
        QWidget *widget = nullptr;
        QLabel *textLabel = nullptr;
        QString fullText; // 任务原文（单行右侧省略 + tooltip 全文）
    };

    void rebuildRows(const QJsonArray &todos);
    void clearRows();
    void refreshRowTexts();
    QString elidedFor(const Row &row, int avail) const;

private:
    QLabel *m_countLabel = nullptr;    // "done/total" 完成计数
    QScrollArea *m_scroll = nullptr;   // 列表容器：封顶后内部滚动（基类 m_contentArea 指向它）
    QWidget *m_list = nullptr;         // 行宿主，QVBoxLayout 纵排
    QVBoxLayout *m_listLayout = nullptr;

    QVector<Row> m_rows;
    int m_totalCount = 0;
    int m_doneCount = 0;
};
