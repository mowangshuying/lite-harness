#include "MessageBubbleWidget.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QStyle>
#include <QTextDocument>
#include <QtMath>

MessageBubbleWidget::MessageBubbleWidget(Role role, QWidget *parent) : FluWidget(parent)
{
    m_content = new QTextBrowser(this);
    m_content->setObjectName("msgBrowser");
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(true);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);

    auto hLayout = new QHBoxLayout(this);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    if (role == User)
    {
        // bubble pinned to the right, capped width
        m_content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        hLayout->addStretch(1);
        hLayout->addWidget(m_content);
    }
    else
    {
        // assistant content spans the full row, left aligned
        m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        hLayout->addWidget(m_content);
        hLayout->addStretch(1);
    }
    setLayout(hLayout);

    connect(m_content->document(), &QTextDocument::contentsChanged, this, &MessageBubbleWidget::updateSize);
    connect(m_content->document(), &QTextDocument::documentLayoutChanged, this, &MessageBubbleWidget::updateSize);

    setRole(role);
}

void MessageBubbleWidget::setRole(Role role)
{
    m_role = role;

    const char *roleName = (m_role == User) ? "User" : "Assistant";
    m_content->setProperty("role", roleName);
    m_content->style()->unpolish(m_content);
    m_content->style()->polish(m_content);

    if (m_role == User)
    {
        m_content->setMaximumWidth(520);
        m_content->document()->setDocumentMargin(10);
    }
    else
    {
        m_content->setMaximumWidth(QWIDGETSIZE_MAX);
        m_content->document()->setDocumentMargin(4);
    }

    updateSize();
}

void MessageBubbleWidget::setContent(const QString &markdown)
{
    m_content->setMarkdown(markdown);
    updateSize();
}

void MessageBubbleWidget::resizeEvent(QResizeEvent *event)
{
    if (m_content && m_content->document())
    {
        m_content->document()->setTextWidth(m_content->viewport()->width());
        updateSize();
    }
    FluWidget::resizeEvent(event);
}

void MessageBubbleWidget::updateSize()
{
    QTextDocument *doc = m_content->document();
    if (!doc)
        return;

    if (m_role == User)
    {
        qreal margin = doc->documentMargin();
        qreal maxW = (qreal)m_content->maximumWidth() - 2.0 * margin;
        qreal w = qBound((qreal)40, (qreal)doc->idealWidth(), maxW);
        doc->setTextWidth(w);
        doc->adjustSize();
        QSizeF docSize = doc->size();
        m_content->setFixedSize(qMax(40, qCeil(docSize.width())),
                                qMax(40, qCeil(docSize.height())));
    }
    else
    {
        doc->setTextWidth(m_content->viewport()->width());
        doc->adjustSize();
        QSizeF docSize = doc->size();
        m_content->setFixedHeight(qMax(25, qCeil(docSize.height())));
    }
}