#include "ToolOutputDialog.h"
#include "ThemeAware.h"
#include "AgentConstants.h" // kOutputCharLimit：上游截断判据单源，meta 行据此如实提示

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTextEdit>
#include <QVBoxLayout>

#include <FluPushButton.h>
#include <FluStyleButton.h>
#include <FluVSplitLine.h>

void ToolOutputDialog::showText(QWidget *parent, const QString &title, const QString &text)
{
    ToolOutputDialog dialog(parent);
    dialog.setTitle(title);
    dialog.setText(text);
    dialog.exec(); // 模态：查看器读的是历史输出，不需要同屏对照，模态换来零布局干扰
}

ToolOutputDialog::ToolOutputDialog(QWidget *parent) : QDialog(parent), m_parentWidget(parent)
{
    // —— 窗口骨架：与 FluentInputDialog / FluMessageBox 同款三层结构 ——
    auto boxLayout = new QHBoxLayout(this);

    // 遮罩刻意**不进布局**（纪律原样继承 FluentInputDialog.cpp 的论证）：它是纯子控件，
    // 几何由构造 / resizeEvent / eventFilter 三处手动铺满。一旦 addWidget 进 QHBoxLayout，
    // 布局即接管它的几何——空 QWidget 无 sizeHint、卡片又 fixedSize + stretch 1 吃掉全部
    // 富余，遮罩会被压成零宽且被 contentsMargins 内缩，等于没有遮罩。勿「顺手」加回来。
    m_windowMask = new QWidget(this);
    m_windowMask->setObjectName(QStringLiteral("windowMask"));

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("centerWidget"));
    boxLayout->addWidget(m_card, 1, Qt::AlignCenter);

    auto cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    cardLayout->setAlignment(Qt::AlignTop);

    // —— 头部：标题 + meta 行 ——
    auto headLayout = new QVBoxLayout();
    headLayout->setContentsMargins(24, 20, 24, 12);
    headLayout->setSpacing(4);

    m_titleLabel = new QLabel(m_card);
    m_titleLabel->setObjectName(QStringLiteral("titleLabel"));
    m_titleLabel->setWordWrap(true);

    m_metaLabel = new QLabel(m_card);
    m_metaLabel->setObjectName(QStringLiteral("metaLabel"));
    m_metaLabel->setWordWrap(true);

    headLayout->addWidget(m_titleLabel);
    headLayout->addWidget(m_metaLabel);
    cardLayout->addLayout(headLayout);

    // —— 内容视图：等宽 + NoWrap + 横滚按需（取舍理由见头文件注释）——
    m_view = new QTextBrowser(m_card);
    m_view->setObjectName(QStringLiteral("outputBody"));
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setLineWrapMode(QTextEdit::NoWrap); // 长行不折行，交横向滚动条承接
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 只读可选中：选中即可 Ctrl+C，与「复制全部」按钮并存——前者取片段，后者取整块
    m_view->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_view->document()->setDocumentMargin(8);
    cardLayout->addWidget(m_view, 1);

    cardLayout->addWidget(new FluVSplitLine(m_card));

    // —— 按钮行：复制全部（强调样式）+ 关闭，排布与宽度同 FluentInputDialog ——
    m_btnWidget = new QWidget(m_card);
    m_btnWidget->setObjectName(QStringLiteral("btnWidget"));
    m_btnWidget->setFixedHeight(64);

    auto btnLayout = new QHBoxLayout(m_btnWidget);
    btnLayout->setContentsMargins(24, 0, 24, 0);
    btnLayout->setSpacing(10);

    m_copyButton = new FluStyleButton(m_btnWidget);
    m_copyButton->setText(tr("复制全部"));
    m_copyButton->setFixedWidth(130);

    m_closeButton = new FluPushButton(m_btnWidget);
    m_closeButton->setText(tr("关闭"));
    m_closeButton->setFixedWidth(130);

    btnLayout->addWidget(m_copyButton);
    btnLayout->addWidget(m_closeButton);
    btnLayout->addStretch(1);
    cardLayout->addWidget(m_btnWidget);

    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    connect(m_copyButton, &FluStyleButton::clicked, this, [this] {
        // 整块写剪贴板（仓内此前无 clipboard()->setText 先例：气泡右键菜单走的是
        // QTextEdit::copy() 选区通路，取不到「整块」语义，故此处新建而非复用）
        QApplication::clipboard()->setText(m_view->toPlainText());
        m_copyButton->setText(tr("已复制"));
    });
    connect(m_closeButton, &FluPushButton::clicked, this, &QDialog::reject);
    // Esc=reject 为 QDialog 原生行为，无需接线

    if (m_parentWidget)
    {
        m_parentWidget->installEventFilter(this);
    }
    else
    {
        m_windowMask->hide(); // 无父窗退化为无遮罩小窗
    }

    applyCardSize();

    // QDialog 无 FluWidget 的主题自动联动，bind 即本对话框唯一的 QSS 首刷 + themeChanged 订阅
    ThemeAware::bind("ToolOutputDialog.qss", this);
}

void ToolOutputDialog::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void ToolOutputDialog::setText(const QString &text)
{
    m_view->setPlainText(text);
    m_copyButton->setText(tr("复制全部")); // 换内容即复位上一次「已复制」反馈

    // meta 行如实声明规模与上游截断：本查看器能给的「完整」只到 kOutputCharLimit 为止，
    // 再往上的原文在上游已被丢弃、此处无从补出——不写这行，用户会把截断版误读成全文。
    const int lines = text.isEmpty() ? 0 : int(text.count(QLatin1Char('\n'))) + 1;
    QString meta = tr("共 %1 字符 · %2 行").arg(text.length()).arg(lines);
    if (text.length() >= int(AgentConst::kOutputCharLimit))
        meta += tr("（已达上游输出上限 %1 字符，更长部分在数据源头即被丢弃，非本视图遮挡）")
                    .arg(AgentConst::kOutputCharLimit);
    m_metaLabel->setText(meta);
}

void ToolOutputDialog::applyCardSize()
{
    // 父窗可用区钳制：小窗不溢出、大窗固定上限（铺满整窗对「读一段输出」无收益，
    // 反而让视线横跨全屏找内容）
    const QSize bound = m_parentWidget ? m_parentWidget->size() : QSize(kCardWidth, kCardHeight);
    const int w = qBound(420, qMin(kCardWidth, bound.width() - 48), qMax(420, bound.width() - 24));
    const int h = qBound(300, qMin(kCardHeight, bound.height() - 48), qMax(300, bound.height() - 24));
    m_card->setFixedSize(w, h);
}

void ToolOutputDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    applyCardSize(); // 父窗尺寸在 exec 前才最终确定，此处按实值再钳一次
    m_view->setFocus(); // 打开即可 Ctrl+A / Ctrl+C，无需先点一下
}

void ToolOutputDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_parentWidget)
    {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
    }
}

bool ToolOutputDialog::eventFilter(QObject *obj, QEvent *event)
{
    // 父窗尺寸变化时同步遮罩与窗口，并让卡片保持居中（同 FluentInputDialog）
    if (obj == m_parentWidget && event->type() == QEvent::Resize)
    {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
        applyCardSize();
    }
    return QDialog::eventFilter(obj, event);
}
