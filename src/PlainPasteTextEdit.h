#pragma once

#include "ImageAttach.h"

#include <QList>
#include <QTextEdit>

#include <functional>

class QMimeData;
class QDragEnterEvent;
class QDropEvent;

/*
    PlainPasteTextEdit —— 只吃纯文本、但会把图片递给宿主的输入框编辑器

    存在理由见 PasteText.h 文件头：QTextEdit 默认 acceptRichText=true，粘贴与拖拽会优先
    消费剪贴板的 text/html 并把行内样式落成逐字符 format，盖掉 QSS 的字色与字号。
    本类把「粘贴/拖拽」两路统一收拢成纯文本 + PasteText::normalize 归一，
    发送口 (toPlainText) 的取值口径不变。

    s18.6 起本类多一个出口：**图片**。此前 canInsertFromMimeData 只认 hasText()，纯图片载荷
    一律拒粘——那是「发送口承载不了图」时的正确取舍（宁可拒，也不要让用户以为发出去了
    而实际蒸发）。现在发送口能承载了，于是判据反过来：图不落进文档（那只会得到一串
    占位符），而是**原样交给宿主的图片挂接槽**，由宿主决定入队与呈现。
    两条入口同判据：剪贴板（Ctrl+V / 右键粘贴，覆盖截屏工具与资源管理器复制图片文件）
    与拖拽（把 .png 拖进输入框）。

    不写 Q_OBJECT：本类只覆写虚函数、不新增信号槽，省掉一次 moc 环节；图片出口用
    一次 std::function 赋值，宿主（ChatMsgEdit）构造期设一次。
*/
class PlainPasteTextEdit : public QTextEdit
{
public:
    // 图片交接口：入参是本次 mime 载荷里**类型认得出**的图片字节，可能含超限的那张
    // （体量与张数闸门归宿主——它才知道队列里已有几张，以及超限该显示哪条原因）。
    using ImageSink = std::function<void(const QList<ImageAttach::Pending> &images)>;

    explicit PlainPasteTextEdit(QWidget *parent = nullptr);

    void setImageSink(ImageSink sink);

    // 宿主右键菜单的准入查询：与 canInsertFromMimeData **同一条判据**。
    // 菜单项的启用条件若自己另写一套（此前是裸 hasText()），就会出现
    // 「菜单里的粘贴灰着、Ctrl+V 照样生效」的分叉——两条路必须问同一个口。
    bool canAcceptMimeData(const QMimeData *source) const;

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // 便宜的「像不像图」判定：只看 MIME 形态与本地文件的后缀，**不读文件字节**。
    // 本函数会被 canInsertFromMimeData / canPaste 这类高频查询调到，
    // 在这里读一个 400MB 的 .png 路径会把输入框冻住——体量与真类型判定一律留到取用侧。
    bool looksLikeImages(const QMimeData *source) const;

    // 真取字节：剪贴板原始 image/* 载荷优先（保原图不重编码），其次本地图片文件。
    // 认不出类型 / 超单张闸门的一律不进结果（宿主据此看到的就是可发的）。
    QList<ImageAttach::Pending> collectImages(const QMimeData *source) const;

    ImageSink m_imageSink;
};
