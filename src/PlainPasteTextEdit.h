#pragma once

#include <QTextEdit>

class QMimeData;

/*
    PlainPasteTextEdit —— 只吃纯文本的输入框编辑器

    存在理由见 PasteText.h 文件头：QTextEdit 默认 acceptRichText=true，粘贴与拖拽会优先
    消费剪贴板的 text/html 并把行内样式落成逐字符 format，盖掉 QSS 的字色与字号。
    本类把「粘贴/拖拽」两路统一收拢成纯文本 + PasteText::normalize 归一，
    发送口 (toPlainText) 的取值口径不变。

    不写 Q_OBJECT：本类只覆写虚函数、不新增信号槽，省掉一次 moc 环节。
*/
class PlainPasteTextEdit : public QTextEdit
{
public:
    explicit PlainPasteTextEdit(QWidget *parent = nullptr);

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
};
