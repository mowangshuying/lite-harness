#include "MessageBubbleWidget.h"
#include "ThinkingBlock.h"
#include "ToolBlock.h"
#include <QFrame>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLayout>
#include <QMargins>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStringList>
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

    m_content = makeTextView();

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

    // 单视图（无工具块）阶段流式增量直接写入主视图
    m_liveView = m_content;

    // 流式期间测量节流：增量追加时最多每 50ms 触发一次 updateSize，保证滚动跟随
    m_streamResizeTimer = new QTimer(this);
    m_streamResizeTimer->setInterval(50);
    m_streamResizeTimer->setSingleShot(true);
    connect(m_streamResizeTimer, &QTimer::timeout, this, &MessageBubbleWidget::updateSize);

    setRole(role);
}

QTextBrowser *MessageBubbleWidget::makeTextView()
{
    auto *view = new QTextBrowser(this);
    // objectName 保持 "msgBrowser"：主题 QSS（ChatSessionPage.qss）按该名称
    // 及 role 动态属性着色，时间线新段与主视图共用同一套样式
    view->setObjectName("msgBrowser");
    view->setFrameShape(QFrame::NoFrame);
    view->setOpenExternalLinks(true);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setContextMenuPolicy(Qt::CustomContextMenu);

    // Prevent cursor navigation from exposing a hidden horizontal range.
    view->setLineWrapMode(QTextEdit::WidgetWidth);

    if (QScrollBar *hbar = view->horizontalScrollBar())
    {
        connect(hbar, &QScrollBar::valueChanged, this, [hbar](int value) {
            if (value != 0)
                hbar->setValue(0);
        });
    }

    connect(view, &QTextBrowser::customContextMenuRequested, this, [this, view](const QPoint &pos) {
        auto menu = new FluPMenu(this);

        auto addAction = [menu, view](FluAwesomeType icon, const QString &text, const QKeySequence &shortcut,
                                      bool enabled, const std::function<void()> &handler) {
            auto action = new FluAction(icon, text, menu);
            action->setShortcut(shortcut);
            action->setEnabled(enabled);
            menu->addAction(action);
            QObject::connect(action, &QAction::triggered, menu, handler);
        };

        addAction(FluAwesomeType::Copy, tr("Copy"), QKeySequence::Copy,
                  view->textCursor().hasSelection(), [view]() { view->copy(); });
        addAction(FluAwesomeType::SelectAll, tr("Select All"), QKeySequence::SelectAll,
                  !view->toPlainText().isEmpty(), [view]() { view->selectAll(); });

        menu->exec(view->mapToGlobal(pos));
        menu->deleteLater();
    });

    // Only listen to contentsChanged; defer to let document layout settle.
    // documentLayoutChanged is NOT connected — it fires on every setTextWidth
    // and causes recursion with QTextBrowser's internal viewport resizing.
    connect(view->document(), &QTextDocument::contentsChanged, this, [this]() {
        QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
    });

    m_textViews.append(view);
    return view;
}

void MessageBubbleWidget::rebuildAsTimeline()
{
    if (m_timeline)
        return;

    QLayout *oldLayout = layout();
    if (oldLayout)
    {
        oldLayout->removeWidget(m_content);
        delete oldLayout;
    }

    m_timeline = new QVBoxLayout(this);
    m_timeline->setContentsMargins(0, 0, 0, 0);
    m_timeline->setSpacing(8);
    m_timeline->addWidget(m_content);
    setLayout(m_timeline);
}

QTextBrowser *MessageBubbleWidget::ensureLiveView()
{
    if (m_liveView)
        return m_liveView;

    // 冻结发生在时间线模式下；非流式防御路径下可能仍是横向布局，先重建
    if (!m_timeline)
        rebuildAsTimeline();

    auto *view = makeTextView();
    view->setProperty("role", "Assistant");
    view->setMaximumWidth(QWIDGETSIZE_MAX);
    view->document()->setDocumentMargin(4);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    view->style()->polish(view);
    m_timeline->addWidget(view);

    m_liveView = view;
    m_liveText.clear();
    return m_liveView;
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

QString MessageBubbleWidget::content() const
{
    // 时间线模式：正文按段顺序合并；单视图模式：直接取主视图 markdown
    if (m_timeline || !m_textRuns.isEmpty())
    {
        QStringList parts;
        for (const TextRun &run : m_textRuns)
        {
            if (!run.markdown.trimmed().isEmpty())
                parts << run.markdown;
        }
        if (!m_liveText.trimmed().isEmpty())
            parts << m_liveText;
        return parts.join(QStringLiteral("\n\n"));
    }
    return m_content->toMarkdown();
}

void MessageBubbleWidget::refreshSize()
{
    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::startStreaming(const QString &placeholder)
{
    m_streaming = true;
    m_liveText.clear();
    m_thinkingRunning = false;
    m_liveThinking = nullptr;
    m_content->setPlainText(placeholder);
    m_liveView = m_content;
    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::appendThinkingText(const QString &delta)
{
    if (!m_streaming || delta.isEmpty())
        return;

    // 新思考区间开始（首轮，或工具/正文打断后重新开始的下一轮）：本轮独立计时
    if (!m_thinkingRunning)
    {
        m_thinkingTimer.start();
        m_thinkingRunning = true;

        rebuildAsTimeline();

        // 冻结上一正文段（与 appendToolExecution 同一套约定）：
        // 后续正文经 ensureLiveView 在思考块之后另起新段，保证时间线因果顺序
        if (m_liveView)
        {
            if (m_liveView->document()->isEmpty())
                m_liveView->hide();
            else
                m_textRuns.append({m_liveText, m_liveView});
            m_liveView = nullptr;
            m_liveText.clear();
        }

        // 每轮思考独立建块并按到达顺序插入时间线：
        // 首轮置顶（此时尚无正文/工具，空主视图收起不占位）；
        // 后续轮次追加到当前位置（上一 ToolBlock/正文段之后）
        auto *block = new ThinkingBlock(this);
        if (!m_liveThinking)
            m_timeline->insertWidget(0, block);
        else
            m_timeline->addWidget(block);
        m_liveThinking = block;
        m_liveThinking->startLive();   // 头部切「思考中」并自动展开，增量在下拉区滚动可见
    }

    m_liveThinking->appendLiveText(delta);

    scheduleStreamResize();
}

void MessageBubbleWidget::appendText(const QString &delta)
{
    if (!m_streaming || delta.isEmpty())
        return;

    // 思考阶段结束（正文开始），暂停计时
    stopThinkingInterval();

    m_liveText += delta;

    QTextBrowser *view = ensureLiveView();

    // 默认格式追加，继承主题文本色
    QTextCursor cur = view->textCursor();
    cur.movePosition(QTextCursor::End);
    cur.insertText(delta);

    scheduleStreamResize();
}

void MessageBubbleWidget::appendToolExecution(const QString &toolName, const QString &summary, const QString &output)
{
    // 仅助手气泡承载工具时间线；用户气泡防御性忽略
    if (m_role != Assistant)
        return;

    // 等待工具结果的这段时间不计入思考耗时
    stopThinkingInterval();

    rebuildAsTimeline();

    // 冻结当前流式段：有可见内容（思考或正文）则归档，空段直接收起，
    // 后续增量经 ensureLiveView 在工具块之后另起新段，保证时间线顺序
    if (m_liveView)
    {
        if (m_liveView->document()->isEmpty())
            m_liveView->hide();
        else
            m_textRuns.append({m_liveText, m_liveView});
        m_liveView = nullptr;
        m_liveText.clear();
    }

    auto *block = new ToolBlock(this);
    block->setToolExecution(toolName, summary, output);
    block->setExpanded(false);
    m_timeline->addWidget(block);

    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::finishStreaming()
{
    m_streaming = false;
    if (m_streamResizeTimer)
        m_streamResizeTimer->stop();
    // 思考收尾：若仍处思考区间则结算本轮耗时，思考块切终态标题并折叠
    stopThinkingInterval();

    if (!m_timeline)
    {
        // 无思考、无工具块：直接渲染正文 markdown（保持原横向布局）
        m_content->setMarkdown(m_liveText);
        QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
        return;
    }

    // 收尾未冻结的流式段：归档后统一按 markdown 重建
    if (m_liveView)
    {
        m_textRuns.append({m_liveText, m_liveView});
        m_liveView = nullptr;
        m_liveText.clear();
    }

    for (const TextRun &run : m_textRuns)
    {
        if (run.markdown.trimmed().isEmpty())
            run.view->hide();   // 空文本段（思考不再经过正文视图，此处只会是占位残留）：收起不占位
        else
            run.view->setMarkdown(run.markdown);
    }

    // 思考块已在流式期间就地创建并收终态，不再重建（避免重复渲染与闪烁）

    QTimer::singleShot(0, this, &MessageBubbleWidget::updateSize);
}

void MessageBubbleWidget::stopThinkingInterval()
{
    if (!m_thinkingRunning)
        return;
    const int elapsedMs = int(m_thinkingTimer.elapsed());
    m_thinkingRunning = false;

    // 本轮思考区间结束（首个正文增量 / 工具到达 / 流收尾三种判定时机）：
    // 本轮思考块标题切「思考了 N 秒」（N 为本轮耗时），未被打扰则自动折叠
    if (m_liveThinking)
        m_liveThinking->stopLive(elapsedMs / 1000);
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
    // 宽度未变（ThinkingBlock/ToolBlock 动画期父链同步 resize 仅改高度）跳过正文重测，
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

        // 逐段测量所有可见的文段视图（工具块会把正文切成多段；
        // 思考块不在 m_textViews 中，其高度由自身测量/动画机制管理）
        for (QTextBrowser *view : m_textViews)
        {
            if (!view || view->isHidden())
                continue;

            QTextDocument *viewDoc = view->document();
            if (!viewDoc)
                continue;

            {
                QSignalBlocker blocker(viewDoc);
                viewDoc->setTextWidth(availW);
            }
            QSizeF docSize = viewDoc->size();
            view->setFixedSize(availW, qCeil(docSize.height()));
        }
    }

    m_updatingSize = false;
}
