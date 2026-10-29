#pragma once

#include <QDialog>
#include <QString>

class FluPushButton;
class FluStyleButton;
class QFrame;
class QLabel;
class QResizeEvent;
class QShowEvent;
class QTextBrowser;
class QWidget;

// 工具输出完整查看器：把 ToolBlock 里被 160px 限高 + 折叠遮挡的输出，
// 放进一个大尺寸、横向不折行、可整体复制的独立视图里读。
//
// 窗口骨架复用 FluentInputDialog / FluMessageBox 同款三层结构（整窗半透明遮罩 +
// 居中圆角卡片 + 分隔线 + 底部按钮行），不另起一套模态实现——遮罩「不进布局、由
// resizeEvent 手动铺满」这条关键纪律在 FluentInputDialog.cpp 里有完整论证与踩坑记录，
// 重造一遍等于重踩一遍。
//
// 为什么内容视图用 NoWrap + 横向滚动（与 ToolBlock 内容区的 WidgetWidth 折行相反）：
// 本对话框的存在理由就是读「长行」——命令行输出、日志、代码片段在 160px 窄窗里被折行
// 后几乎不可读。这里宽度是气泡的数倍且可横向拖动滚动条，不折行才是正确取舍；
// 正文气泡那边继续折行（窄窗内不折行会撑破布局），两者按各自窗宽分别取最优，不强行统一。
//
// 数据源边界（重要，勿当成「查看原始未截断输出」）：本类展示的文本来自 ToolBlock 已持有
// 的输出串，而该串在更上游已被 BashRunner::truncateOutput 按 AgentConst::kOutputCharLimit
// 硬截断且不可逆（原文既不落盘也不在内存留存）。因此本查看器解决的是「被视口挡住」，
// 不解决「被截断丢弃」——后者需要改后端截断策略，属独立议题，此处不做过度承诺。
class ToolOutputDialog : public QDialog
{
    Q_OBJECT

public:
    // 便捷入口：栈上构造 + exec。title 为窗口标题（工具名 + 摘要），text 为待展示全文。
    // parent 传主窗/页面（遮罩铺满并随父窗尺寸联动）；为空则退化为无遮罩小窗。
    static void showText(QWidget *parent, const QString &title, const QString &text);

    explicit ToolOutputDialog(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    // 写入全文并据此自动刷新 meta 行（字符数/行数 + 上游截断提示）。
    // meta 不在外部拼：截断判据与 kOutputCharLimit 同源，调用方各写一份必然漂移。
    void setText(const QString &text);

protected:
    void showEvent(QShowEvent *event) override;     // 首次显示：把焦点落到内容视图（可直接 Ctrl+A/C）
    void resizeEvent(QResizeEvent *event) override; // 遮罩与卡片随父窗尺寸联动
    // 父窗尺寸变化时同步遮罩与窗口尺寸（构造里对 m_parentWidget installEventFilter）：
    // 遮罩不进布局、纯手动铺满，故父窗每次 resize 都得由这里补一次尺寸（同 FluentInputDialog）
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    // 卡片尺寸按父窗可用区钳制：小窗下不溢出，大窗下固定上限（避免无意义地铺满）
    void applyCardSize();

    QWidget *m_parentWidget = nullptr;
    QWidget *m_windowMask = nullptr;  // objectName windowMask，半透明遮罩（不进布局）
    QFrame *m_card = nullptr;         // objectName centerWidget，圆角卡片
    QLabel *m_titleLabel = nullptr;   // objectName titleLabel
    QLabel *m_metaLabel = nullptr;    // objectName metaLabel（字符数提示，可隐藏）
    QTextBrowser *m_view = nullptr;   // objectName outputBody，等宽 + NoWrap + 横滚按需
    QWidget *m_btnWidget = nullptr;   // objectName btnWidget，底部按钮行
    FluStyleButton *m_copyButton = nullptr;
    FluPushButton *m_closeButton = nullptr;

    static constexpr int kCardWidth = 860;   // 卡片理想宽
    static constexpr int kCardHeight = 560;  // 卡片理想高
};
