#include "PlainPasteTextEdit.h"
#include "PasteText.h"

#include <QMimeData>

PlainPasteTextEdit::PlainPasteTextEdit(QWidget *parent) : QTextEdit(parent)
{
    // 兜底：覆写之外还有 Qt 内部走富文本的入口（如程序化 paste 的 HTML 分支判定），
    // 关掉该开关让这些路径自己退化成纯文本，与本类的判据同向而非互相打架
    setAcceptRichText(false);
}

bool PlainPasteTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    // 只认 text/plain：纯 HTML（无 plain 兜底）与纯图片一律不可粘贴。
    // 与 ChatMsgEdit 右键菜单「粘贴」项的启用判据同口径（那边同样只看 hasText），
    // 也与发送口能承载的形态一致——全仓没有任何图片附件通路，图粘进文档只会在
    // toPlainText() 处静默蒸发，用户却以为已经发出去了。
    return source->hasText();
}

void PlainPasteTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (!canInsertFromMimeData(source))
        return;
    // insertPlainText 而非 setHtml/setText：落在光标处、替换当前选区、保住已打的草稿，
    // 且 Qt 内部按原样插入（不解释 TAB、不做二次排版），整次粘贴仍是一步撤销
    insertPlainText(PasteText::normalize(source->text()));
}
