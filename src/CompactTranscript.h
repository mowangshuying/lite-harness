#pragma once

// 压缩转写回溯：把 history.json 里的「压缩摘要消息」展开成被它替换掉的
// 前序消息，供会话重启时**只读回放**——不进 m_messages、不进请求上下文。
//
// 背景（用户报障）：全量压缩（CompactManager::compactHistoryAsync / reactiveCompactAsync）
// 把会话主体整体替换成一条 user 摘要消息，随后 persistHistory() 立刻落盘 → history.json
// 只剩那条摘要，重启后 loadSavedHistory 只能回放出摘要一条，压缩前的对话在界面上消失。
// 数据本身没丢：压缩前全量被 writeTranscript() 写成
// <会话根>/.transcripts/transcript_<uuid>.jsonl（路径就印在摘要正文的 `Full transcript:`
// 行里）。本头沿该链把原文读回来，交给展示层平铺在摘要之前。
//
// 语义边界（逐条钉死，勿扩大）：
//   · 只服务回放。展开结果不写回 AgentLoop::m_messages，故模型上下文、token 计量、
//     persistHistory 落盘内容、前缀缓存全部不受影响；
//   · 展开顺序即时间顺序：摘要消息被替换为「其 transcript 内容（递归展开）+ 摘要消息本身」，
//     多次压缩自然串成「最早原文 → 内层摘要 → 外层摘要」；
//   · 任何一步不成立都**保留原摘要消息**降级（路径不安全 / 文件缺失 / 坏行 / 空内容），
//     绝不因为回放而丢消息，也绝不因回放失败打断会话加载；
//   · 防环 + 深度上限：同一 transcript 只展开一次（QSet 记录），嵌套深度 ≤ kMaxExpandDepth；
//   · 路径必须落在 <会话根>/.transcripts/ 下且形如 transcript_*.jsonl（摘要正文可被
//     任何来源污染，禁止据此读取任意文件）。
//
// 纯头（QtCore-only、零夹具可测）：header-only 不占测试目标 TU 清单，tst_compacttranscript
// 直接 include 本头，并以注入的 TranscriptReader 回调完全绕开真实文件系统。

#include "AgentConstants.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

namespace CompactTranscript {

/// 嵌套展开上限：压缩每发生一次就多一层（transcript 内含上一层的摘要消息）。
/// 8 层已远超实际会话可能的压缩次数，超出即停手、保留摘要消息。
inline constexpr int kMaxExpandDepth = 8;

/// 摘要消息判据：role=user 且 content 以 `[<label>]` 开头**并含 `Full transcript:` 行**。
/// label 单源对齐 CompactManager::summaryMessage 的两个调用点
/// （compactHistoryAsync 传 "Compacted"、reactiveCompactAsync 传 "Reactive compact"）。
/// 第二个条件防误判：真实摘要消息的模板恒含该行（落盘失败时是
/// `Full transcript: (transcript unavailable)`），而普通用户消息恰好以 `[Compacted]`
/// 开头时不会被当成摘要。新增压缩入口若换 label/模板，必须同步本判据。
inline bool isSummaryMessage(const QJsonObject &message)
{
    if (message.value(QStringLiteral("role")).toString() != QLatin1String("user"))
        return false;
    const QString content = message.value(QStringLiteral("content")).toString();
    if (!content.contains(QStringLiteral("\nFull transcript: ")))
        return false;
    return content.startsWith(QStringLiteral("[Compacted]"))
        || content.startsWith(QStringLiteral("[Reactive compact]"));
}

/// 摘要消息正文里的 `Full transcript: <路径>` 行 → 路径串。
/// 取最后一个匹配（正文本身可能复述该字样），只取到行尾；空串与
/// `(transcript unavailable)` 占位（落盘失败时的既有占位）一律返回空串。
inline QString transcriptPath(const QJsonObject &message)
{
    const QString content = message.value(QStringLiteral("content")).toString();
    const QString marker = QStringLiteral("\nFull transcript: ");
    const int at = content.lastIndexOf(marker);
    if (at < 0)
        return QString();
    QString path = content.mid(at + marker.size());
    const int lineEnd = path.indexOf(QLatin1Char('\n'));
    if (lineEnd >= 0)
        path = path.left(lineEnd);
    path = path.trimmed();
    if (path.isEmpty() || path == QStringLiteral("(transcript unavailable)"))
        return QString();
    return path;
}

/// 路径守卫：只认 <会话根>/.transcripts/transcript_*.jsonl（目录直下、非递归）。
/// 摘要正文可被任意来源污染（历史可手工编辑），禁据其读取任意文件。
inline bool isSafeTranscriptPath(const QString &sessionRoot, const QString &path)
{
    if (sessionRoot.isEmpty() || path.isEmpty())
        return false;
    const QString root = QDir::cleanPath(sessionRoot);
    // cleanPath("") / "." 代表当前工作目录——事故形态（见 ScopedTempRoot 头注释），一律判无效
    if (root.isEmpty() || root == QStringLiteral("."))
        return false;
    const QString dir = root + QLatin1Char('/') + AgentConst::kTranscriptsDirName;
    const QString clean = QDir::cleanPath(path);
    if (!clean.startsWith(dir + QLatin1Char('/')))
        return false;
    const QString name = clean.mid(dir.size() + 1);
    if (name.contains(QLatin1Char('/')))
        return false; // 只认目录直下，子目录形态不认
    return name.startsWith(QStringLiteral("transcript_"))
        && name.endsWith(QStringLiteral(".jsonl"));
}

/// 转写读取回调（可注入：测试绕开文件系统）。
using TranscriptReader = std::function<bool(const QString &path, QByteArray *out)>;

/// 默认读取实现（QFile 一次性读入）。
inline bool readTranscriptFile(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    if (out)
        *out = data;
    return true;
}

/// JSONL 解析：一行一条 JSON 对象；空行与坏行**跳过**（转写是事后审计产物，
/// 单行损坏不该让整段历史回放失败），非对象行同样跳过。
inline QVector<QJsonObject> parseTranscriptJsonl(const QByteArray &bytes)
{
    QVector<QJsonObject> messages;
    const QList<QByteArray> lines = bytes.split('\n');
    messages.reserve(lines.size());
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        messages.append(doc.object());
    }
    return messages;
}

/// 展开入口：逐条遍历，摘要消息替换为「其转写内容（递归展开）+ 该摘要消息」。
/// reader 为空时用 readTranscriptFile；visited 为空时内部自建（防环作用域 = 本次展开）。
inline QVector<QJsonObject> expand(const QVector<QJsonObject> &messages,
                                   const QString &sessionRoot,
                                   const TranscriptReader &reader = TranscriptReader(),
                                   int depth = 0,
                                   QSet<QString> *visited = nullptr)
{
    QSet<QString> localVisited;
    QSet<QString> *seen = visited ? visited : &localVisited;
    const TranscriptReader read = reader ? reader : TranscriptReader(&readTranscriptFile);

    QVector<QJsonObject> out;
    out.reserve(messages.size());
    for (const QJsonObject &message : messages) {
        if (depth >= kMaxExpandDepth || !isSummaryMessage(message)) {
            out.append(message);
            continue;
        }
        const QString path = transcriptPath(message);
        if (path.isEmpty() || !isSafeTranscriptPath(sessionRoot, path)) {
            out.append(message);
            continue;
        }
        const QString key = QDir::cleanPath(path);
        if (seen->contains(key)) { // 防环：同一转写只展开一次
            out.append(message);
            continue;
        }
        QByteArray bytes;
        if (!read(path, &bytes)) {
            out.append(message);
            continue;
        }
        const QVector<QJsonObject> archived = parseTranscriptJsonl(bytes);
        if (archived.isEmpty()) {
            out.append(message);
            continue;
        }
        seen->insert(key);
        out.append(expand(archived, sessionRoot, read, depth + 1, seen));
        out.append(message); // 摘要消息留在原位：其后即「模型当前持有的上下文」
    }
    return out;
}

} // namespace CompactTranscript