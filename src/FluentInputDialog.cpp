#include "FluentInputDialog.h"

#include "ThemeAware.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include <FluLineEdit.h>
#include <FluPushButton.h>
#include <FluStyleButton.h>
#include <FluVSplitLine.h>

// 静态便捷入口：栈上构造 + exec，语义对齐 QInputDialog::getText
QPair<QString, bool> FluentInputDialog::getInputText(QWidget *parent,
                                                     const QString &title,
                                                     const QString &label,
                                                     const QString &text,
                                                     QLineEdit::EchoMode echoMode)
{
    FluentInputDialog dialog(parent);
    dialog.setTitle(title);
    dialog.setLabel(label);
    dialog.setText(text);
    dialog.setEchoMode(echoMode);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    return {dialog.text(), accepted};
}

FluentInputDialog::FluentInputDialog(QWidget *parent) : QDialog(parent), m_parentWidget(parent)
{
    // —— 窗口骨架：与 FluMessageBox 同款（遮罩 + 居中卡片），卡片稍高以容纳输入行 ——
    auto boxLayout = new QHBoxLayout(this);

    // 遮罩刻意**不进布局**：它是纯子控件，几何由构造/resizeEvent/eventFilter 里那三处
    // m_windowMask->resize(m_parentWidget->size()) 手动铺满（同 FluMessageBox 原版——其
    // m_boxLayout 全文只 addWidget 卡片一处）。一旦 addWidget 进 QHBoxLayout，布局即接管
    // 它的几何：空 QWidget 无 sizeHint、minimumSize 为 0，卡片又 fixedSize + stretch 1 吃掉
    // 全部富余，遮罩实测被压成 **0x782** 且被 contentsMargins 内缩到起点 (9,9)，等于完全没有
    // 遮罩；同时那三处手动 resize 全部沦为无效代码（被下一次布局激活覆盖）。改 stretch 也救不
    // 了——横向布局里一个子件无法跨越另一个子件所占的那一格（实测 stretch 1 只得 597 宽）。
    // 勿「顺手」把这行加回来。
    m_windowMask = new QWidget(this);
    m_windowMask->setObjectName("windowMask"); // 配色见 stylesheet/<theme>/FluentInputDialog.qss

    m_card = new QFrame(this);
    m_card->setObjectName("centerWidget");
    m_card->setFixedSize(360, 232);
    boxLayout->addWidget(m_card, 1, Qt::AlignCenter);

    auto cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    cardLayout->setAlignment(Qt::AlignTop);

    // 内容区：标题 → 提示（可隐藏）→ 输入框，底部拉伸使其贴顶成组
    auto contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(24, 32, 24, 0);
    contentLayout->setSpacing(0);

    m_titleLabel = new QLabel(m_card);
    m_titleLabel->setObjectName("titleLabel");
    m_titleLabel->setWordWrap(true);

    m_infoLabel = new QLabel(m_card);
    m_infoLabel->setObjectName("infoLabel");
    m_infoLabel->setWordWrap(true);

    m_lineEdit = new FluLineEdit(m_card);

    contentLayout->addWidget(m_titleLabel);
    contentLayout->addSpacing(6);
    contentLayout->addWidget(m_infoLabel);
    contentLayout->addSpacing(12);
    contentLayout->addWidget(m_lineEdit);
    contentLayout->addStretch(1);
    cardLayout->addLayout(contentLayout);

    cardLayout->addWidget(new FluVSplitLine(m_card));

    // 按钮行：确定（FluStyleButton 强调样式）在前、取消在后，同 FluMessageBox 排布与宽度
    m_btnWidget = new QWidget(m_card);
    m_btnWidget->setObjectName("btnWidget");
    m_btnWidget->setFixedHeight(80);

    auto btnLayout = new QHBoxLayout(m_btnWidget);
    btnLayout->setContentsMargins(24, 0, 24, 0);
    btnLayout->setSpacing(10);

    m_okButton = new FluStyleButton(m_btnWidget);
    m_okButton->setText(tr("确定"));
    m_okButton->setFixedWidth(130);
    m_okButton->setDefault(true); // Enter 兜底（FluLineEdit 自身回车走下方 returnPressed 通路）

    m_cancelButton = new FluPushButton(m_btnWidget);
    m_cancelButton->setText(tr("取消"));
    m_cancelButton->setFixedWidth(130);

    btnLayout->addWidget(m_okButton);
    btnLayout->addWidget(m_cancelButton);
    btnLayout->addStretch(1);
    cardLayout->addWidget(m_btnWidget);

    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    connect(m_lineEdit, &FluLineEdit::returnPressed, m_okButton, &FluStyleButton::click); // 回车=确定
    connect(m_okButton, &FluStyleButton::clicked, this, &QDialog::accept);
    connect(m_cancelButton, &FluPushButton::clicked, this, &QDialog::reject);
    // Esc=reject 为 QDialog 原生行为，无需接线

    if (m_parentWidget)
    {
        // 同 FluMessageBox：铺满父窗做遮罩，并跟随父窗尺寸变化
        setGeometry(0, 0, m_parentWidget->width(), m_parentWidget->height());
        m_windowMask->resize(m_parentWidget->size());
        m_parentWidget->installEventFilter(this);
    }
    else
    {
        m_windowMask->hide(); // 无父窗退化为无遮罩小窗，卡片由布局撑出窗口尺寸
    }

    // QDialog 无 FluWidget 的主题自动联动，bind 即本对话框唯一的 QSS 首刷 + themeChanged 订阅
    ThemeAware::bind("FluentInputDialog.qss", this);
}

void FluentInputDialog::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void FluentInputDialog::setLabel(const QString &label)
{
    m_infoLabel->setVisible(!label.isEmpty());
    m_infoLabel->setText(label);
}

void FluentInputDialog::setText(const QString &text)
{
    m_lineEdit->setText(text);
}

void FluentInputDialog::setEchoMode(QLineEdit::EchoMode mode)
{
    m_lineEdit->setEchoMode(mode);
}

QString FluentInputDialog::text() const
{
    return m_lineEdit->text();
}

void FluentInputDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    m_lineEdit->setFocus();  // 打开即聚焦，无需点击
    m_lineEdit->selectAll(); // 预填内容全选，直接输入即整体替换
}

void FluentInputDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_parentWidget)
    {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
    }
}

bool FluentInputDialog::eventFilter(QObject *obj, QEvent *event)
{
    // 父窗尺寸变化时同步遮罩与窗口（同 FluMessageBox），并让卡片保持居中
    if (obj == m_parentWidget && event->type() == QEvent::Resize)
    {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
    }
    return QDialog::eventFilter(obj, event);
}
