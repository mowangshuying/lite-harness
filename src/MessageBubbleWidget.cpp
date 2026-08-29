#include "MessageBubbleWidget.h"
#include "ThinkingBlock.h"
#include <QColor>
#include <QFrame>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLayout>
#include <QMargins>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#include <functional>
#include <FluAction.h>
#include <FluPMenu.h>
// #include <FluScrollDelegate.h>

// Actual row width available to a bubble: parent widget (the scroll area's
// context widget, kept in sync with the viewport width by setWidgetResizable)
// minus its layout's left/right contents margins. Returns 0 when the bubble
// has no parent yet or the parent has no width.
static int availableContentWidth(const QWidget *bubble)
{
    const QWidget* pw = bubble->parentWidget();
    const QWidget *ppw = bubble->parentWidget()->parentWidget();
    if (!pw || !ppw)
        return 0;
    
    int w = ppw->width();
    if (w <= 0)
        return 0;


    //if (const QLayout *pl = pw->layout())
    //{
    //    const QMargins m = pl->contentsMargins();
    //    w -= (m.left() + m.right());
    //}

    if (ppw->layout())
    {
        w -= ppw->layout()->contentsMargins().left() + ppw->layout()->contentsMargins().right();
    }

    if (pw->layout())
    {
        w -= pw->layout()->contentsMargins().left() + pw->layout()->contentsMargins().right();
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
    m_content->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_content, &QTextBrowser::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto menu = new FluPMenu(this);

        auto addAction = [menu](FluAwesomeType icon, const QString &text, const QKeySequence &shortcut,
                                bool enabled, const std::function<void()> &handler) {
            auto action = new FluAction(icon, text, menu);
            action->setShortcut(shortcut);
            action->setEnabled(enabled);
            menu->addAction(action);
            QObject::connect(action, &QAction::triggered, menu, handler);
        };

        addAction(FluAwesomeType::Copy, tr("Copy"), QKeySequence::Copy,
                  m_content->textCursor().hasSelection(), [this]() { m_content->copy(); });
        addAction(FluAwesomeType::SelectAll, tr("Select All"), QKeySequence::SelectAll,
                  !m_content->toPlainText().isEmpty(), [this]() { m_content->selectAll(); });

        menu->exec(m_content->mapToGlobal(pos));
        menu->deleteLater();
    });

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

    // 流式期间测量节流：增量追加时最多每 50ms 触发一次 updateSize，保证滚动跟随
    m_streamResizeTimer = new QTimer(this);
    m_streamResizeTimer->setInterval(50);
    m_streamResizeTimer->setSingleShot(true);
    connect(m_streamResizeTimer, &QTimer::timeout, this, &MessageBubbleWidget::updateSize);

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

void MessageBubbleWidget::startStreaming(const QString &placeholder)
{
    m_streaming = true;
    m_thinkingBuffer.clear();
    m_textBuffer.clear();
    m_thinkingStarted = false;
    m_content->setPlainText(placeholder);
    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::appendThinkingText(const QString &delta)
{
    if (!m_streaming || delta.isEmpty())
        return;

    // 首个 thinkingDelta 到来时启动思考计时
    if (!m_thinkingStarted)
    {
        m_thinkingTimer.start();
        m_thinkingStarted = true;
    }

    m_thinkingBuffer += delta;

    // 灰色斜体追加，纯文本路径速度更快
    QTextCursor cur = m_content->textCursor();
    cur.movePosition(QTextCursor::End);
    QTextCharFormat fmt;
    fmt.setForeground(QColor(128, 128, 128));
    fmt.setFontItalic(true);
    cur.insertText(delta, fmt);

    scheduleStreamResize();
}

void MessageBubbleWidget::appendText(const QString &delta)
{
    if (!m_streaming || delta.isEmpty())
        return;

    m_textBuffer += delta;

    // 默认格式追加，继承主题文本色
    QTextCursor cur = m_content->textCursor();
    cur.movePosition(QTextCursor::End);
    cur.insertText(delta);

    scheduleStreamResize();
}

void MessageBubbleWidget::finishStreaming()
{
    m_streaming = false;
    if (m_streamResizeTimer)
        m_streamResizeTimer->stop();

    const QString thinking = m_thinkingBuffer.trimmed();
    if (thinking.isEmpty())
    {
        // 无思考：直接渲染正文 markdown
        m_content->setMarkdown(m_textBuffer);
        QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
        return;
    }

    // 思考耗时（秒）：从首个 thinkingDelta 到 messageFinished
    const int thinkingMs = m_thinkingStarted ? int(m_thinkingTimer.elapsed()) : 0;
    const int thinkingSeconds = qMax(0, thinkingMs / 1000);

    // 重建为纵向布局：上部 ThinkingBlock（可折叠思考区），下部正文 markdown
    QLayout *oldLayout = layout();
    if (oldLayout)
    {
        oldLayout->removeWidget(m_content);
        delete oldLayout;
    }

    auto *thinkingBlock = new ThinkingBlock(this);
    thinkingBlock->setThinkingContent(thinking);
    thinkingBlock->setThinkingDuration(thinkingSeconds);
    thinkingBlock->setExpanded(false);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(8);
    vLayout->addWidget(thinkingBlock);
    vLayout->addWidget(m_content);
    setLayout(vLayout);

    // 正文仅渲染 markdown（思考区已移入 ThinkingBlock）
    m_content->setMarkdown(m_textBuffer);

    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::scheduleStreamResize()
{
    // 节流：timer 未激活才启动，timeout 时执行 updateSize
    if (m_streamResizeTimer && !m_streamResizeTimer->isActive())
        m_streamResizeTimer->start();
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
    // 宽度未变（ThinkingBlock 动画期父链同步 resize 仅改高度）跳过正文重测，
    // 避免每帧 markdown 文档重排引入布局噪声
    if (m_content && m_content->document() && event->size().width() != event->oldSize().width())
        updateSize();
}

void MessageBubbleWidget::updateSize()
{
    if (m_updatingSize)
        return;
    m_updatingSize = true;

    // 流式期间每段增量都会触发 contentsChanged → updateSize，
    // 节流定时器已挂起时直接跳过，由 timer timeout 统一测量
    if (m_streaming && m_streamResizeTimer && m_streamResizeTimer->isActive())
    {
        m_updatingSize = false;
        return;
    }

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
            // Text fits within available width — use QFontMetrics measurement
            // directly. doc->idealWidth() can return 0 before the document
            // layout has fully settled, so we avoid relying on it here.
            QSignalBlocker blocker(doc);
            qreal finalW = qCeil(allBubbleWidth);
            doc->setTextWidth(finalW);
            QSizeF docSize = doc->size();
            m_content->setFixedSize(finalW, qCeil(docSize.height()));
        }
    }
    else
    {
        // int vpWidth = m_content->viewport()->width();

        int availW = availableContentWidth(this);
        // availW *= 0.75;
        if (availW <= 0)
        {
            // Viewport not yet realized — retry on next event loop tick
            m_updatingSize = false;
            QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
            return;
        }
        {
            QSignalBlocker blocker(doc);
            doc->setTextWidth(availW);
            // doc->adjustSize();
        }
        // QSizeF docSize = doc->size();
        QSizeF docSize = doc->size();
        m_content->setFixedSize(availW, qCeil(docSize.height()));
    }

    m_updatingSize = false;
}
