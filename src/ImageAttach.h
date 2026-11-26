#pragma once

// 图片附件载荷层单源（header-only，仅 QtCore，零 IO）：类型嗅探、张数/体量闸门、
// 附件命名、附件引用形态与 data URL 拼装。
//
// 抽头动机：图片这件事的**后果面**全在「发出去之前」——一张 1MB 截图内联成 base64 是
// 1.37M 字符，而本仓的 token 计量（CompactTokens）与压缩闸门（CompactManager 五级）
// 都按字符折算，内联等于让一条消息自己吃掉整个窗口预算；history.json 又是**每工具批
// 全量重写**（见 AgentLoopHistory::persistHistory 那条 Compact-vs-Indented 注释）。
// 故二进制一律落 <会话根>/.attachments/，历史里只留引用，展开发生在请求装配末段
// （MessageContent::expandForWire）。本头只管引用与载荷的判据，不碰盘。
//
// 嗅探刻意**不经 QImage / 图像插件**：发布包 plugins/imageformats 在 CI 产物里实测缺
// qwebp（AGENTS 打包节的本地-CI 条目差 6 枚之一就是它），若判据依赖解码器，同一份历史
// 在两台机器上会给出不同的 mime。魔数是字节层面的事实，与安装内容无关。

#include "AgentConstants.h" // 图片闸门常量单源

#include <QByteArray>
#include <QCryptographicHash>
#include <QLatin1Char>
#include <QString>

namespace ImageAttach {

// 魔数比对：不走 "\x89" 这类窄字符串转义——/utf-8 下 >127 的十六进制转义在窄字面量里
// 是实现定义行为（MSVC 会报 C4566 一族），且 \x 会贪婪吞后续十六进制位。按字节值比最稳。
inline bool hasMagic(const QByteArray &data, const uchar *magic, int len)
{
    if (data.size() < len)
        return false;
    for (int i = 0; i < len; ++i) {
        if (static_cast<quint8>(static_cast<char>(data.at(i))) != magic[i])
            return false;
    }
    return true;
}

// 由文件头字节判定 MIME；认不出即返回空串（调用方按「不支持的类型」处置，不猜）。
// 支持集取「OpenAI 兼容端点与 qwen-vl 两族都收」的交集，且 BMP 一并放行（Qt 原生解码、
// 截图工具常见产物）。
inline QString mimeFor(const QByteArray &head)
{
    static const uchar kPng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const uchar kJpeg[] = {0xFF, 0xD8, 0xFF};

    if (hasMagic(head, kPng, static_cast<int>(sizeof(kPng))))
        return QStringLiteral("image/png");
    if (hasMagic(head, kJpeg, static_cast<int>(sizeof(kJpeg))))
        return QStringLiteral("image/jpeg");
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return QStringLiteral("image/gif");
    if (head.startsWith("BM"))
        return QStringLiteral("image/bmp");
    if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return QStringLiteral("image/webp");
    return QString();
}

// MIME ↔ 扩展名两向单源（命名时用一向，展开时不必回读元数据即用另一向反查）。
inline QString extensionFor(const QString &mime)
{
    if (mime == QLatin1String("image/png"))
        return QStringLiteral("png");
    if (mime == QLatin1String("image/jpeg"))
        return QStringLiteral("jpg");
    if (mime == QLatin1String("image/gif"))
        return QStringLiteral("gif");
    if (mime == QLatin1String("image/bmp"))
        return QStringLiteral("bmp");
    if (mime == QLatin1String("image/webp"))
        return QStringLiteral("webp");
    return QString();
}

inline QString mimeForExtension(const QString &extension)
{
    if (extension == QLatin1String("png"))
        return QStringLiteral("image/png");
    if (extension == QLatin1String("jpg"))
        return QStringLiteral("image/jpeg");
    if (extension == QLatin1String("gif"))
        return QStringLiteral("image/gif");
    if (extension == QLatin1String("bmp"))
        return QStringLiteral("image/bmp");
    if (extension == QLatin1String("webp"))
        return QStringLiteral("image/webp");
    return QString();
}

// 附件名 = 内容哈希前 16 位十六进制 + 扩展名。**按内容命名**而不是序号：同一段截图重复
// 粘贴落到同一个文件，历史里两条引用指向同一份载荷，删重与磁盘占用都天然收敛。
// 入参字节为空或类型认不出 → 返回空串（无附件名可谈）。
inline QString nameFor(const QByteArray &bytes, const QString &mime)
{
    const QString extension = extensionFor(mime);
    if (bytes.isEmpty() || extension.isEmpty())
        return QString();
    return QString::fromLatin1(
               QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
               .left(16)
        + QLatin1Char('.') + extension;
}

// 引用名合法性：形态必须是 `<16 位小写十六进制>.<扩展名>`，且扩展名在支持集内。
// 逐字符判而非正则——本仓对「来自盘上的串」一律自己拆到字符层（同 ActivityLedger::dateFromKey
// 那条：Qt 的宽松解析会把畸形值静默读成合法值）。这条判据的**后果**是文件 IO：松一寸
// 就等于让一份手改的 history.json 指定任意路径去读，属 zip-slip 同一族护栏
// （对照 UpdateRules::normalizeEntryPath、CompactTranscript 的 .transcripts 目录守卫）。
inline bool isSafeName(const QString &name)
{
    const int dot = name.indexOf(QLatin1Char('.'));
    if (dot != 16 || name.size() - dot - 1 < 3) // 最短扩展名 "bmp"=3
        return false;
    for (int i = 0; i < dot; ++i) {
        const QChar ch = name.at(i);
        const bool hex = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
        if (!hex)
            return false;
    }
    return !mimeForExtension(name.mid(dot + 1)).isEmpty();
}

// 历史里图片 part 的 url 形态：私有 scheme，永不上线。选它而不是 file:// 绝对路径，两条理由：
// ① file:// 带着机器布局，换机器/换存储根即失效，而附件名是内容哈希、天然可随会话目录搬走；
// ② 私有 scheme 不可能被端点误当成可取用的地址，展开漏一处时报的是「未知 scheme」而不是
//    一次看似正常的远程取图。
inline QString attachmentScheme()
{
    return QStringLiteral("lh-attach://");
}

inline QString urlFor(const QString &name)
{
    return isSafeName(name) ? attachmentScheme() + name : QString();
}

// 从 url 反查附件名；非本 scheme、或名字不合形态 → 空串（调用方按不可用降级）。
// 只认前缀剥离后的裸名：scheme 之后不得再有任何分隔符形态（`..` 与 `/` 由 isSafeName 兜死）。
inline QString nameFromUrl(const QString &url)
{
    const QString scheme = attachmentScheme();
    if (!url.startsWith(scheme))
        return QString();
    const QString name = url.mid(scheme.size());
    return isSafeName(name) ? name : QString();
}

// 是否已经是可直接下发的 url（data: 形态）。恢复历史后再次发送、或用户手工塞入的
// data URL 都走这条：展开点遇到它原样保留，不二次编码。
inline bool isInlineUrl(const QString &url)
{
    return url.startsWith(QStringLiteral("data:"));
}

// 闸门两问：单张体量与单条消息张数。超限一律**拒绝并可见**，不做静默压缩/缩放——
// 缩放要重编码（JPEG 有损），等于把用户给的原图换掉；对照 LineEnding 批 edit_file 那条
// 「超限不尝试部分读写（部分写会毁文件）」的同族取舍。
inline bool withinByteCap(qint64 bytes)
{
    return bytes > 0 && bytes <= AgentConst::kImageMaxBytes;
}

inline bool withinCountCap(int count)
{
    return count >= 0 && count < AgentConst::kImageMaxPerMessage;
}

// 支持的图片类型**有序清单**：顺序即剪贴板取用优先级（无损优先——PNG 排最前，
// 截图工具给的 BMP 排最后）。mimeFor / extensionFor / mimeForExtension 三张表都以此为准，
// 加一项只在这里加一次，避免「嗅得出、存不下、展不开」这类半拉子支持。
inline QStringList supportedMimes()
{
    return {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
            QStringLiteral("image/gif"), QStringLiteral("image/webp"),
            QStringLiteral("image/bmp")};
}

// 按**后缀**判「可能是图片文件」：只看名字、不碰盘，供拖拽/粘贴的高频准入查询用。
// 真类型仍由 mimeFor 读魔数定——后缀可以是 .png 而内容是 zip，准入宽、取用严。
inline bool isSupportedFile(const QString &path)
{
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot == path.size() - 1)
        return false;
    return !mimeForExtension(path.mid(dot + 1).toLower()).isEmpty();
}

// data URL 拼装（请求装配末段用）：mime 由调用方给出，本函数不猜。
// 载荷为空 → 空串，绝不产出 `data:image/png;base64,` 这种半截形态（端点会当成坏 url）。
inline QString dataUrlFor(const QString &mime, const QByteArray &payload)
{
    if (mime.isEmpty() || payload.isEmpty())
        return QString();
    return QStringLiteral("data:") + mime + QStringLiteral(";base64,")
        + QString::fromLatin1(payload.toBase64());
}

// 一张待发图片：原始字节 + 嗅探出的 MIME。
// 生命周期刻意只到「点发送」为止：附件文件**在发送时才落盘**（落盘点只有 AgentLoop 知道
// 会话根），否则用户贴一张图又清掉草稿，会在会话目录里留下一个永远没人引用的孤儿文件。
// 4MB 上限下四张 = 16MB 内存驻留，且只存在于当前输入框实例，可接受。
struct Pending
{
    QByteArray bytes;
    QString mime;

    // 附件名（内容哈希 + 扩展名）；字节或类型缺失时为空
    QString name() const
    {
        return nameFor(bytes, mime);
    }

    bool isValid() const
    {
        return !bytes.isEmpty() && !mime.isEmpty();
    }
};

// 由原始字节造一张待发图片：**只嗅类型，不做体量裁决**。
// 体量必须留到 rejectReason 判，因为「超限」是一条要显示给用户的原因——若在这里顺手丢掉，
// 用户贴了一张 8MB 的截图后输入框毫无反应，正是本批要消灭的那类静默蒸发。
// 入参建议已由调用方限长（读文件时最多读 kImageMaxBytes+1 字节），本函数不担这个约束。
inline Pending pendingFromBytes(const QByteArray &bytes)
{
    Pending pending;
    if (bytes.isEmpty())
        return pending;
    pending.mime = mimeFor(bytes.left(static_cast<int>(AgentConst::kImageSniffBytes)));
    if (pending.mime.isEmpty())
        return pending;
    pending.bytes = bytes;
    return pending;
}

// 拒收原因（UI 文案的取值域，判据只在这里算一次）。
// 顺序有讲究：先判类型再判体量——把一段 zip 字节拖进来时报「不是支持的图片类型」
// 比报「超过 4MB」更有用，且体量闸门在前会让超大非图文件得到一条误导文案。
enum class Reject
{
    None,
    UnsupportedType,
    TooLarge,
    TooMany
};

inline Reject rejectReason(const QByteArray &bytes, int alreadyQueued)
{
    const QString mime =
        mimeFor(bytes.left(static_cast<int>(AgentConst::kImageSniffBytes)));
    if (mime.isEmpty())
        return Reject::UnsupportedType;
    if (!withinByteCap(bytes.size()))
        return Reject::TooLarge;
    if (!withinCountCap(alreadyQueued))
        return Reject::TooMany;
    return Reject::None;
}

} // namespace ImageAttach
