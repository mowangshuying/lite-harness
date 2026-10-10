#pragma once

#include <QDialog>
#include <QLineEdit> // EchoMode 枚举（密钥输入框脱敏）
#include <QPair>
#include <QString>

class FluLineEdit;
class FluPushButton;
class FluStyleButton;
class QLabel;
class QFrame;
class QResizeEvent;
class QShowEvent;

// FluentUI 风格的单行输入对话框：与 FluMessageBox 同款窗口骨架
// （整窗半透明遮罩 + 居中圆角卡片 + 底部分隔线 + 灰底按钮行），
// 内容区为 标题 + 可选提示 label + FluLineEdit（预填、打开即全选聚焦、回车=确定、Esc=取消）。
// 主题跟随：本类配色走仓内 stylesheet/<theme>/FluentInputDialog.qss；
// 输入框与按钮为 FluentUI 控件自带 QSS，天然三主题变色。
// 与 FluMessageBox 相同约定：parent 传主窗口（用于遮罩铺满与随父窗尺寸联动）；parent 为空时退化为无遮罩小窗。
class FluentInputDialog : public QDialog
{
    Q_OBJECT
public:
    // 静态便捷入口，语义对齐 QInputDialog::getText：返回 {输入文本, 是否点确定}。
    // label 为空则不显示提示行；text 为预填内容。
    static QPair<QString, bool> getInputText(QWidget *parent,
                                             const QString &title,
                                             const QString &label = QString(),
                                             const QString &text = QString(),
                                             QLineEdit::EchoMode echoMode = QLineEdit::Normal);

    explicit FluentInputDialog(QWidget *parent = nullptr);

    // 输入框回显模式：默认 Normal。密钥类输入传 QLineEdit::Password，避免明文上屏
    // （肩窥/截屏泄漏）。此前全仓 grep echoMode 零命中，与 SettingsPage 里
    // 「明文绝不上屏」的注释自相矛盾——值区脱敏了，编辑框却把完整 key 预填出来。
    void setEchoMode(QLineEdit::EchoMode mode);

    void setTitle(const QString &title);
    void setLabel(const QString &label); // 空串隐藏提示行
    void setText(const QString &text);   // 预填输入框
    QString text() const;                // 当前输入内容（原始值，未 simplified/trim）

protected:
    void showEvent(QShowEvent *event) override;     // 首次显示：聚焦输入框并全选预填内容
    void resizeEvent(QResizeEvent *event) override; // 遮罩与卡片随父窗尺寸联动
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QWidget *m_parentWidget = nullptr;
    QWidget *m_windowMask = nullptr;  // objectName windowMask，半透明遮罩
    QFrame *m_card = nullptr;         // objectName centerWidget，圆角卡片
    QLabel *m_titleLabel = nullptr;   // objectName titleLabel
    QLabel *m_infoLabel = nullptr;    // objectName infoLabel（提示行，可隐藏）
    FluLineEdit *m_lineEdit = nullptr;
    QWidget *m_btnWidget = nullptr;   // objectName btnWidget，灰底按钮行
    FluPushButton *m_cancelButton = nullptr;
    FluStyleButton *m_okButton = nullptr;
};
