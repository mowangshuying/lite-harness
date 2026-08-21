#include "ChatSessionPage.h"
#include <FluUtils.h>
#include <FluThemeUtils.h>
#include <FluVScrollView.h>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include "ChatMsgEdit.h"

namespace {

// Escape characters that carry special meaning in Markdown so that
// arbitrary user text can be safely embedded inside a blockquote.
QString escapeMarkdownSpecialChars(const QString &text)
{
    QString result = text;
    // Backslash must be escaped first to avoid double-escaping.
    result.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    result.replace(QLatin1String("*"), QLatin1String("\\*"));
    result.replace(QLatin1String("_"), QLatin1String("\\_"));
    result.replace(QLatin1String("`"), QLatin1String("\\`"));
    result.replace(QLatin1String("["), QLatin1String("\\["));
    result.replace(QLatin1String("]"), QLatin1String("\\]"));
    result.replace(QLatin1String("#"), QLatin1String("\\#"));
    result.replace(QLatin1String("+"), QLatin1String("\\+"));
    result.replace(QLatin1String("-"), QLatin1String("\\-"));
    result.replace(QLatin1String("."), QLatin1String("\\."));
    result.replace(QLatin1String("!"), QLatin1String("\\!"));
    result.replace(QLatin1String("|"), QLatin1String("\\|"));
    return result;
}

// Convert each line of user text into a Markdown blockquote line
// ("> ...") with special characters escaped, so the original content
// cannot break out of the quote or interfere with surrounding Markdown.
QString toSafeBlockquote(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace('\r', '\n');
    const QStringList lines = normalized.split('\n');
    QStringList quoted;
    quoted.reserve(lines.size());
    for (const auto &line : lines)
        quoted << QStringLiteral("> ") + escapeMarkdownSpecialChars(line);
    return quoted.join(QLatin1Char('\n'));
}

// Return a fenced-code-block delimiter (``` or longer) that does not
// appear anywhere in the given text, preventing the user's content
// from prematurely closing the fence.
QString safeFenceDelimiter(const QString &text)
{
    QString fence = QStringLiteral("```");
    while (text.contains(fence))
        fence += QLatin1Char('`');
    return fence;
}

// Build a deterministic local-mock Assistant reply that exercises
// several Markdown constructs: italic, bold, blockquote, list,
// inline code, and a fenced code block.
QString generateMockReply(const QString &userText)
{
    const QString quoted = toSafeBlockquote(userText);
    const QString fence = safeFenceDelimiter(userText);
    const int charCount = userText.length();

    QString reply;
    reply += QStringLiteral("*[本地模拟回复 — 远端接口尚未接入]*\n\n");
    reply += QStringLiteral("你发送了：\n\n");
    reply += quoted;
    reply += QStringLiteral("\n\n**要点整理：**\n\n");
    reply += QStringLiteral("- 收到你的消息，共 **%1** 个字符\n").arg(charCount);
    reply += QStringLiteral("- 当前为 `本地模拟模式`，所有回复均由客户端生成\n");
    reply += QStringLiteral("- 远端 API 接入后，此处将替换为真实模型响应\n\n");
    reply += QStringLiteral("**示例代码：**\n\n");
    reply += fence;
    reply += QStringLiteral("cpp\n");
    reply += QStringLiteral("// 模拟响应处理\n");
    reply += QStringLiteral("void onReply(const QString &msg) {\n");
    reply += QStringLiteral("    qDebug() << \"received:\" << msg;\n");
    reply += QStringLiteral("}\n");
    reply += fence;
    reply += QStringLiteral("\n\n如需进一步测试，请继续发送消息。");
    return reply;
}

} // namespace

ChatSessionPage::ChatSessionPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(35, 35, 35, 35);
    vMainLayout->setSpacing(15);
    setLayout(vMainLayout);

    m_scrollView = new FluVScrollView(this);
    m_scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    m_scrollView->getMainLayout()->setContentsMargins(15, 15, 15, 15);
    m_scrollView->getMainLayout()->setSpacing(15);
    vMainLayout->addWidget(m_scrollView, 1);

    auto hLayout = new QHBoxLayout();
    m_inputEdit = new ChatMsgEdit(this);
    // vMainLayout->addWidget(m_inputEdit, 0, Qt::AlignHCenter);
    hLayout->addWidget(m_inputEdit, 1);

    connect(m_inputEdit, &ChatMsgEdit::sendMessage, this, [this](const QString &text) {
        addMessage(MessageBubbleWidget::Role::User, text);

        // Schedule a local mock Assistant reply after a short delay.
        // Using 'this' as context ensures the callback is automatically
        // cancelled if the page is destroyed before the timer fires.
        QTimer::singleShot(600, this, [this, text]() {
            addMessage(MessageBubbleWidget::Role::Assistant, generateMockReply(text));
        });
    });

    vMainLayout->addLayout(hLayout);

    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, &ChatSessionPage::onThemeChanged);
    onThemeChanged();
}

void ChatSessionPage::addMessage(MessageBubbleWidget::Role role, const QString &content)
{
    auto bubble = new MessageBubbleWidget(role, this);
    bubble->setContent(content);
    m_scrollView->getMainLayout()->addWidget(bubble);
    scrollToBottom();
}

void ChatSessionPage::scrollToBottom()
{
    auto scrollBar = m_scrollView->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void ChatSessionPage::clearMessages()
{
    auto mainLayout = m_scrollView->getMainLayout();
    while (auto item = mainLayout->takeAt(0))
    {
        if (auto widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

void ChatSessionPage::resizeEvent(QResizeEvent *event)
{
    BasePage::resizeEvent(event);

    auto mainLayout = m_scrollView->getMainLayout();
    for (int i = 0; i < mainLayout->count(); ++i)
    {
        auto bubble = qobject_cast<MessageBubbleWidget *>(mainLayout->itemAt(i)->widget());
        if (bubble)
            bubble->refreshSize();
    }

    QTimer::singleShot(0, this, [this]() {
        auto mainLayout = m_scrollView->getMainLayout();
        for (int i = 0; i < mainLayout->count(); ++i)
        {
            auto bubble = qobject_cast<MessageBubbleWidget *>(mainLayout->itemAt(i)->widget());
            if (bubble)
                bubble->refreshSize();
        }
    });
}

void ChatSessionPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("ChatSessionPage.qss", this, FluThemeUtils::getUtils()->getTheme());

    // Re-polish existing bubbles so their role-based QSS picks up the new theme
    auto mainLayout = m_scrollView->getMainLayout();
    for (int i = 0; i < mainLayout->count(); ++i)
    {
        auto widget = mainLayout->itemAt(i)->widget();
        if (widget)
        {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }
}
