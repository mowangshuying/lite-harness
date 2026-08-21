#include "MessageBubbleWidget.h"
#include <QFrame>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLayout>
#include <QMargins>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QtMath>
// #include <FluScrollDelegate.h>

// Actual row width available to a bubble: parent widget (the scroll area's
// context widget, kept in sync with the viewport width by setWidgetResizable)
// minus its layout's left/right contents margins. Returns 0 when the bubble
// has no parent yet or the parent has no width.
static int availableContentWidth(const QWidget *bubble)
{
    const QWidget *pw = bubble->parentWidget();
    if (!pw)
        return 0;
    int w = pw->width();
    if (w <= 0)
        return 0;
    if (const QLayout *pl = pw->layout())
    {
        const QMargins m = pl->contentsMargins();
        w -= (m.left() + m.right());
    }
    return w;
}

MessageBubbleWidget::MessageBubbleWidget(Role role, QWidget *parent) : FluWidget(parent)
{
    // Row fills the available width; height is driven by content.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_content = new QTextBrowser(this);
    // auto delegate = new FluScrollDelegate(m_content);
    m_content->setObjectName("msgBrowser");
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(true);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Prevent cursor navigation from exposing a hidden horizontal range.
    m_content->setLineWrapMode(QTextEdit::WidgetWidth);

    if (QScrollBar *hbar = m_content->horizontalScrollBar())
    {
        connect(hbar, &QScrollBar::valueChanged, this, [hbar](int value) {
            if (value != 0)
                hbar->setValue(0);
        });
    }

    auto hLayout = new QHBoxLayout(this);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    if (role == User)
    {
        // Bubble wraps the text content and is right-aligned in the row.
        // Width is measured from the document and capped at a fraction of
        // the available row width so the bubble grows with the text but
        // reflows when the window narrows.
        m_content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        hLayout->addWidget(m_content, 0, Qt::AlignRight);
    }
    else
    {
        // assistant content spans the full row, left aligned
        m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        hLayout->addWidget(m_content, 0, Qt::AlignLeft);
    }
    setLayout(hLayout);

    // Only listen to contentsChanged; defer to let document layout settle.
    // documentLayoutChanged is NOT connected — it fires on every setTextWidth
    // and causes recursion with QTextBrowser's internal viewport resizing.
    connect(m_content->document(), &QTextDocument::contentsChanged, this, [this]() {
        QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
    });

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
        m_content->setMaximumWidth(QWIDGETSIZE_MAX);
        m_content->document()->setDocumentMargin(10);
    }
    else
    {
        m_content->setMaximumWidth(QWIDGETSIZE_MAX);
        m_content->document()->setDocumentMargin(4);
    }

    updateSize();
}

void MessageBubbleWidget::setContent(const QString &content)
{
    if (m_role == User)
        m_content->setPlainText(content);
    else
        m_content->setMarkdown(content);

    // Defer measurement so the document finishes its internal layout pass
    // before we measure idealWidth / document size.
    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::refreshSize()
{
    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

bool MessageBubbleWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        refreshSize();

    return FluWidget::eventFilter(watched, event);
}

void MessageBubbleWidget::resizeEvent(QResizeEvent *event)
{
    FluWidget::resizeEvent(event);
    if (m_content && m_content->document())
    {
        updateSize();
    }
}

void MessageBubbleWidget::updateSize()
{
    if (m_updatingSize)
        return;
    m_updatingSize = true;

    QTextDocument *doc = m_content->document();
    if (!doc)
    {
        m_updatingSize = false;
        return;
    }

    if (m_role == User)
    {
        if (parentWidget())
            parentWidget()->installEventFilter(this);

        int availW = availableContentWidth(this);
        availW *= 0.75;
        if (availW <= 0)
        {
            m_updatingSize = false;
            QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
            return;
        }
        qreal margin = doc->documentMargin();

        QFontMetrics fm(m_content->font());
        qreal allTextWidth = 0;
        const QStringList lines = doc->toPlainText().split('\n');
        for (const QString &line : lines)
            allTextWidth = qMax(allTextWidth, qreal(fm.horizontalAdvance(line)));
        qreal allBubbleWidth = allTextWidth + 2.0 * margin;
        if (allBubbleWidth > availW)
        {
            QSignalBlocker blocker(doc);
            doc->setTextWidth(availW);
            QSizeF docSize = doc->size();
            m_content->setFixedSize(availW, qCeil(docSize.height()));
        }
        else
        {
            QSignalBlocker blocker(doc);
            doc->setTextWidth(-1);
            doc->adjustSize();
            qreal naturalTextWidth = doc->idealWidth();
            qreal naturalBubbleWidth = naturalTextWidth + 2.0 * margin;

            naturalBubbleWidth = qMin(qCeil(allBubbleWidth), qCeil(naturalBubbleWidth));
            doc->setTextWidth(naturalBubbleWidth);
            QSizeF docSize = doc->size();
            m_content->setFixedSize(naturalBubbleWidth, qCeil(docSize.height()));

        }
    }
    else
    {
        int vpWidth = m_content->viewport()->width();
        if (vpWidth <= 0)
        {
            // Viewport not yet realized — retry on next event loop tick
            m_updatingSize = false;
            QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
            return;
        }
        {
            QSignalBlocker blocker(doc);
            doc->setTextWidth(vpWidth);
            doc->adjustSize();
        }
        QSizeF docSize = doc->size();
        m_content->setFixedHeight(qMax(25, qCeil(docSize.height())));
    }

    m_updatingSize = false;
}
