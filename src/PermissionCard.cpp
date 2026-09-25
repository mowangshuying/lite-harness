#include "PermissionCard.h"
#include "ThemeAware.h"
#include "ToolTagKind.h"
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

QString PermissionCard::translateReason(const QString &reason)
{
    // 后端契约为固定英文短句；未知 reason 原样展示，避免信息丢失
    if (reason == QLatin1String("Writing outside workspace"))
        return tr("正在尝试访问工作区之外的路径");
    if (reason == QLatin1String("Potentially destructive command"))
        return tr("疑似破坏性命令");
    return reason;
}

QString PermissionCard::toolPendingText(const QString &toolName)
{
    // 与 ToolBlock::toolTitleText 的完成词条同一动词根：已执行 <-> 请求执行
    if (toolName == QLatin1String("bash"))
        return tr("请求执行");
    if (toolName == QLatin1String("read_file"))
        return tr("请求读取");
    if (toolName == QLatin1String("write_file"))
        return tr("请求写入");
    if (toolName == QLatin1String("edit_file"))
        return tr("请求编辑");
    if (toolName == QLatin1String("glob"))
        return tr("请求查找");
    return tr("请求执行");
}

PermissionCard::PermissionCard(QWidget *parent) : FluWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // —— 待决态主体 ——
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("pcBody"));
    auto bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(12, 10, 12, 10);
    bodyLayout->setSpacing(6);

    // 行 1：警示徽章 + 标题
    auto row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(8);
    m_badgeLabel = new QLabel(QStringLiteral("!"), m_body);
    m_badgeLabel->setObjectName(QStringLiteral("pcBadge"));
    m_badgeLabel->setFixedSize(18, 18);
    m_badgeLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel = new QLabel(tr("需要权限确认"), m_body);
    m_titleLabel->setObjectName(QStringLiteral("pcTitle"));
    row1->addWidget(m_badgeLabel, 0, Qt::AlignVCenter);
    row1->addWidget(m_titleLabel, 0, Qt::AlignVCenter);
    row1->addStretch(1);

    // 行 2：工具名标签 + 待决词条 + 关键参数（缩进 26 = 徽章宽 18 + 间距 8，与标题对齐）
    auto row2 = new QHBoxLayout();
    row2->setContentsMargins(26, 0, 0, 0);
    row2->setSpacing(8);
    m_tagLabel = new QLabel(m_body);
    m_tagLabel->setObjectName(QStringLiteral("pcTag"));
    m_verbLabel = new QLabel(m_body);
    m_verbLabel->setObjectName(QStringLiteral("pcVerb"));
    m_summaryLabel = new QLabel(m_body);
    m_summaryLabel->setObjectName(QStringLiteral("pcSummary"));
    m_summaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row2->addWidget(m_tagLabel, 0, Qt::AlignVCenter);
    row2->addWidget(m_verbLabel, 0, Qt::AlignVCenter);
    row2->addWidget(m_summaryLabel, 1, Qt::AlignVCenter);

    // 行 3：风险原因 + 操作按钮（拒绝在左、允许在右；拒绝为默认焦点）
    auto row3 = new QHBoxLayout();
    row3->setContentsMargins(26, 0, 0, 0);
    row3->setSpacing(8);
    m_reasonLabel = new QLabel(m_body);
    m_reasonLabel->setObjectName(QStringLiteral("pcReason"));
    m_denyButton = new QPushButton(tr("拒绝"), m_body);
    m_denyButton->setObjectName(QStringLiteral("pcDenyButton"));
    m_allowButton = new QPushButton(tr("允许"), m_body);
    m_allowButton->setObjectName(QStringLiteral("pcAllowButton"));
    for (auto *button : {m_denyButton, m_allowButton})
    {
        button->setFixedHeight(28);
        button->setMinimumWidth(64);
        button->setCursor(Qt::PointingHandCursor);
    }
    row3->addWidget(m_reasonLabel, 0, Qt::AlignVCenter);
    row3->addStretch(1);
    row3->addWidget(m_denyButton, 0, Qt::AlignVCenter);
    row3->addWidget(m_allowButton, 0, Qt::AlignVCenter);

    bodyLayout->addLayout(row1);
    bodyLayout->addLayout(row2);
    bodyLayout->addLayout(row3);

    // —— 裁决留痕（单行，弱化呈现，保留在时间线中）——
    m_traceWidget = new QWidget(this);
    m_traceWidget->setObjectName(QStringLiteral("pcTrace"));
    auto traceLayout = new QHBoxLayout(m_traceWidget);
    traceLayout->setContentsMargins(4, 2, 4, 2);
    traceLayout->setSpacing(6);
    m_traceIcon = new QLabel(m_traceWidget);
    m_traceIcon->setObjectName(QStringLiteral("pcTraceIcon"));
    m_traceIcon->setFixedWidth(16);
    m_traceIcon->setAlignment(Qt::AlignCenter);
    m_traceLabel = new QLabel(m_traceWidget);
    m_traceLabel->setObjectName(QStringLiteral("pcTraceLabel"));
    m_traceLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    traceLayout->addWidget(m_traceIcon, 0, Qt::AlignVCenter);
    traceLayout->addWidget(m_traceLabel, 1, Qt::AlignVCenter);
    m_traceWidget->setVisible(false);

    rootLayout->addWidget(m_body);
    rootLayout->addWidget(m_traceWidget);

    connect(m_denyButton, &QPushButton::clicked, this, [this]() { handleDecision(false); });
    connect(m_allowButton, &QPushButton::clicked, this, [this]() { handleDecision(true); });

    // QSS 主题加载与联动（与 ToolBlock / ThinkingBlock 同一套约定）：收敛到 ThemeAware::bind
    ThemeAware::bind("PermissionCard.qss", this);
}

void PermissionCard::setPermissionRequest(const QString &toolName, const QString &summary,
                                          const QString &reason)
{
    m_toolName = toolName;
    m_summary = summary;

    m_tagLabel->setText(toolName);
    // 工具名标签与 ToolBlock 共用同一套语义类别着色，保持两处视图的视觉语言一致。
    ToolTagKind::applyTo(m_tagLabel, toolName);
    m_verbLabel->setText(toolPendingText(toolName));
    m_reasonLabel->setText(translateReason(reason));
    refreshTexts();

    // 拒绝按钮持有默认焦点（安全优先）；延迟到事件循环，确保卡片已入布局可见后生效
    QTimer::singleShot(0, m_denyButton, [this]() {
        if (!m_resolved)
            m_denyButton->setFocus();
    });
}

void PermissionCard::handleDecision(bool allow)
{
    if (m_resolved)
        return; // 防双击/竞态：首次点击即锁定裁决
    m_resolved = true;
    m_denyButton->setEnabled(false);
    m_allowButton->setEnabled(false);
    applyTrace(allow);
    emit userResolved(allow);
}

void PermissionCard::resolveDenySilently()
{
    if (m_resolved)
        return;
    m_resolved = true;
    applyTrace(false); // 外部收口（用户 stop 等）：与后端自动拒绝一致，不发信号
}

void PermissionCard::applyTrace(bool allow)
{
    m_body->setVisible(false);
    m_traceWidget->setVisible(true);

    m_traceIcon->setText(allow ? QStringLiteral("✓") : QStringLiteral("✕"));
    m_traceIcon->setProperty("outcome", allow ? QStringLiteral("allowed")
                                              : QStringLiteral("denied"));
    m_traceIcon->style()->unpolish(m_traceIcon);
    m_traceIcon->style()->polish(m_traceIcon);

    const QString prefix = allow ? tr("已允许") : tr("已拒绝");
    const QString line = prefix
                         + QStringLiteral(" · ") + toolPendingText(m_toolName)
                         + QStringLiteral(" · ") + singleLineText(m_summary);
    m_traceLabel->setText(line);
    m_traceLabel->setToolTip(line);
    refreshTexts();
    updateGeometry();
}

QString PermissionCard::singleLineText(const QString &text)
{
    QString line = text;
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    line.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return line.simplified();
}

void PermissionCard::refreshTexts()
{
    if (m_summary.isEmpty())
    {
        m_summaryLabel->setText(QString());
        m_summaryLabel->setToolTip(QString());
        return;
    }

    const QString text = singleLineText(m_summary);

    // 行 2 可用宽度 = 卡片宽 - 左右边距(12+12) - 边框(1+1) - 缩进 26 - 标签/词条宽 - 间距(8*2)
    const int avail = qMax(40, width() - 12 - 12 - 2 - 26
                                  - m_tagLabel->sizeHint().width()
                                  - m_verbLabel->sizeHint().width() - 8 * 2);
    QFontMetrics fm(m_summaryLabel->font());
    m_summaryLabel->setText(fm.elidedText(text, Qt::ElideMiddle, avail));
    m_summaryLabel->setToolTip(text);

    // 留痕行同样按宽度省略
    QFontMetrics tfm(m_traceLabel->font());
    const int traceAvail = qMax(60, width() - 4 - 4 - 16 - 6 - 2);
    m_traceLabel->setText(tfm.elidedText(m_traceLabel->toolTip(), Qt::ElideMiddle, traceAvail));
}

void PermissionCard::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);
    refreshTexts();
}
