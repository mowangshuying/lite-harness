#pragma once

#include <FluWidget.h>

class QLabel;
class QPushButton;

// 权限确认卡片（s03：危险操作审批）：
//   AgentLoop::permissionRequired 到达时在会话流底部挂载，包含
//   警示徽章 + "需要权限确认" + 等宽工具名标签 + 请求词条 + 关键参数（中部省略 + tooltip）
//   + reason 中文转译（琥珀点缀）+ 「拒绝 / 允许」按钮（拒绝持有默认焦点，安全优先）。
//   用户裁决后整卡收起，仅留一行弱化的"已允许/已拒绝"留痕在时间线中。
// 不使用系统模态对话框：AgentLoop 为事件驱动异步，卡片内嵌于消息流不打断阅读。
class PermissionCard : public FluWidget
{
    Q_OBJECT

public:
    explicit PermissionCard(QWidget *parent = nullptr);

    // toolName: bash/read_file/write_file/edit_file/glob/...
    // summary:  命令或路径摘要（等宽单行展示，超长中部省略）
    // reason:   后端英文短句，UI 内映射为中文（未知原样展示）
    void setPermissionRequest(const QString &toolName, const QString &summary,
                              const QString &reason);

    bool isResolved() const { return m_resolved; }

    // 外部强制收口为"已拒绝"留痕（不发信号）：用于用户 stop/回合异常结束时，
    // 与后端 stop() 的自动拒绝回填保持一致，避免重复裁决。
    void resolveDenySilently();

signals:
    // 用户点击「允许/拒绝」时发射一次（点击即置 m_resolved，杜绝双击竞态）；
    // 由会话页转呼 AgentLoop::resolvePermission(allow)
    void userResolved(bool allow);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // reason 英文短句 -> 中文转译（未知原样返回）
    static QString translateReason(const QString &reason);
    // 工具名 -> 中文待决词条（请求执行/请求读取/...），未知回退"请求执行"。
    // 与 ToolBlock::toolTitleText 的完成词条（已执行/已读取）同一套动词根
    static QString toolPendingText(const QString &toolName);

    void handleDecision(bool allow);       // 按钮点击统一入口（含防重入）
    void applyTrace(bool allow);           // body -> 单行留痕
    void refreshTexts();                   // 按当前宽度重算摘要/留痕省略
    static QString singleLineText(const QString &text); // 换行/制表压平 + 折叠空白

private:
    // —— 待决态主体 ——
    QWidget *m_body = nullptr;
    QLabel *m_badgeLabel = nullptr;        // 圆形 "!" 警示徽章（QSS 着色）
    QLabel *m_titleLabel = nullptr;        // "需要权限确认"
    QLabel *m_tagLabel = nullptr;          // 等宽工具名标签（bash/write_file/...）
    QLabel *m_verbLabel = nullptr;         // 中文待决词条（"请求写入"等）
    QLabel *m_summaryLabel = nullptr;      // 关键参数（等宽，中部省略，tooltip 全文）
    QLabel *m_reasonLabel = nullptr;       // 转译后的风险原因（琥珀色）
    QPushButton *m_denyButton = nullptr;   // 拒绝（默认焦点）
    QPushButton *m_allowButton = nullptr;  // 允许（琥珀描边，弱于拒绝一档）
    // —— 裁决留痕 ——
    QWidget *m_traceWidget = nullptr;
    QLabel *m_traceIcon = nullptr;         // ✓ / ✕
    QLabel *m_traceLabel = nullptr;        // "已允许 · 请求写入 · <摘要>"

    QString m_toolName;
    QString m_summary;                     // 摘要原文（省略显示 + tooltip 全文）
    bool m_resolved = false;
};
