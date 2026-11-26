#include "PlainPasteTextEdit.h"
#include "PasteText.h"

#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QUrl>

#include <utility> // std::move

PlainPasteTextEdit::PlainPasteTextEdit(QWidget *parent) : QTextEdit(parent)
{
    // 兜底：覆写之外还有 Qt 内部走富文本的入口（如程序化 paste 的 HTML 分支判定），
    // 关掉该开关让这些路径自己退化成纯文本，与本类的判据同向而非互相打架
    setAcceptRichText(false);
}

void PlainPasteTextEdit::setImageSink(ImageSink sink)
{
    m_imageSink = std::move(sink);
}

bool PlainPasteTextEdit::canAcceptMimeData(const QMimeData *source) const
{
    return canInsertFromMimeData(source);
}

bool PlainPasteTextEdit::looksLikeImages(const QMimeData *source) const
{
    if (!source)
        return false;

    const QStringList formats = ImageAttach::supportedMimes();
    for (const QString &format : formats) {
        if (source->hasFormat(format))
            return true;
    }
    // Windows 剪贴板的位图常以 CF_DIB/CF_BITMAP 形态存在，Qt 会把它合成 hasImage()，
    // 但未必逐个登记 image/* 格式名——只看 hasFormat 会漏掉真正的截屏粘贴。
    if (source->hasImage())
        return true;

    for (const QUrl &url : source->urls()) {
        if (url.isLocalFile() && ImageAttach::isSupportedFile(url.toLocalFile()))
            return true;
    }
    return false;
}

QList<ImageAttach::Pending>
PlainPasteTextEdit::collectImages(const QMimeData *source) const
{
    QList<ImageAttach::Pending> collected;
    if (!source)
        return collected;

    // ① 剪贴板原始字节优先：截屏工具与「复制图片」给出的就是原图，
    //    直接取用可以零重编码。命中一种即返回——同一张图常同时挂 png/bmp 两份形态，
    //    全收等于让用户粘一次图附两张。
    const QStringList formats = ImageAttach::supportedMimes();
    for (const QString &format : formats) {
        if (!source->hasFormat(format))
            continue;
        const ImageAttach::Pending pending = ImageAttach::pendingFromBytes(source->data(format));
        if (pending.isValid()) {
            collected.append(pending);
            return collected;
        }
    }

    // ② 只有位图句柄形态时走 QImage：重编码为 PNG（无损，不引入 JPEG 那种有损代次损失）。
    //    这一步确实改写了字节，故内容哈希命名会与原文件不同——可接受，附件名只作寻址用。
    if (source->hasImage() && source->imageData().canConvert<QImage>()) {
        const QImage image = source->imageData().value<QImage>();
        if (!image.isNull()) {
            QByteArray buffer;
            QBuffer device(&buffer);
            if (device.open(QIODevice::WriteOnly) && image.save(&device, "PNG")) {
                const ImageAttach::Pending pending = ImageAttach::pendingFromBytes(buffer);
                if (pending.isValid())
                    collected.append(pending);
            }
        }
        if (!collected.isEmpty())
            return collected;
    }

    // ③ 本地文件（资源管理器里 Ctrl+C 一张 png 再粘进输入框，或直接拖进来）。
    //    限长读 kImageMaxBytes+1：一个 2GB 的 .png 路径不该被 readAll 拖进内存，
    //    而多读的那 1 字节正好让宿主判出「超限」而不是「什么都没发生」。
    //    收集上限取 张数闸门 + 1：多带一张过去，宿主据此给出一条「最多 N 张」的可见提示，
    //    再多就是纯浪费（一次拖 50 张的场景，第 6 张之后与第 5 张的提示完全一样）。
    for (const QUrl &url : source->urls()) {
        if (collected.size() > AgentConst::kImageMaxPerMessage)
            break;
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.isFile() || !ImageAttach::isSupportedFile(path))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const ImageAttach::Pending pending =
            ImageAttach::pendingFromBytes(file.read(AgentConst::kImageMaxBytes + 1));
        if (pending.isValid())
            collected.append(pending);
    }
    return collected;
}

bool PlainPasteTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    // 文本或图片，两者之一可粘。此前只认 hasText()：纯图片载荷一律拒粘，
    // 理由是「发送口承载不了图，宁可拒也不要让用户以为发出去了」。
    // s18.6 起发送口能承载了，判据随之放宽——但图**不进文档**（进文档只会得到一串占位符），
    // 而是走下面的交接口给宿主入队。
    return source->hasText() || looksLikeImages(source);
}

void PlainPasteTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (!canInsertFromMimeData(source))
        return;

    // 文本优先，且此时**不**再取图：从浏览器 / Word / IDE 复制富文本时，剪贴板常常同时
    // 挂一份选区的位图快照（CF_DIB），一并入队等于用户粘一段文字却附了一张意外的图。
    // 纯图载荷（截屏工具、复制图片文件）才走交接口——那才是「我要给你看这张图」的意图。
    if (source->hasText()) {
        // insertPlainText 而非 setHtml/setText：落在光标处、替换当前选区、保住已打的草稿，
        // 且 Qt 内部按原样插入（不解释 TAB、不做二次排版），整次粘贴仍是一步撤销
        insertPlainText(PasteText::normalize(source->text()));
        return;
    }
    if (m_imageSink)
        m_imageSink(collectImages(source));
}

void PlainPasteTextEdit::dragEnterEvent(QDragEnterEvent *event)
{
    // 图片形态必须自己收下：QTextEdit 默认对不可插入的 mime 直接 ignore，
    // 于是拖一个 .png 进来连 drop 机会都没有。文本拖拽仍交回基类（保持既有行为）。
    if (looksLikeImages(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dragEnterEvent(event);
}

void PlainPasteTextEdit::dropEvent(QDropEvent *event)
{
    if (looksLikeImages(event->mimeData())) {
        if (m_imageSink)
            m_imageSink(collectImages(event->mimeData()));
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dropEvent(event);
}
