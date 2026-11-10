// tst_compacttranscript.cpp —— 压缩转写回溯（src/CompactTranscript.h，纯 QtCore 纯头）
// 被测：摘要消息判据 / `Full transcript:` 路径解析 / 路径守卫 / JSONL 解析 /
//       链式展开（单层、多层嵌套、环、读失败与不安全路径的降级）。
//
// 为什么必须测：这段逻辑决定「压缩过的会话重启后还能不能看到压缩前的对话」。
// 它有三个不会报错、只会静默变错的失效面：
//   ① 摘要 label 与 CompactManager::summaryMessage 的调用点漂移 → 判据恒假、历史不再回放；
//   ② 路径守卫写松 → 摘要正文（历史文件可被任意编辑）能指向任意文件被读进来；
//   ③ 展开失败时没保留摘要消息 → 回放比不回放还少内容（用户看到的是"记录又丢了"）。
// 三者都无编译期或运行期信号，只能靠本套件钉住。
//
// 零夹具依赖：所有读取经注入的 TranscriptReader 回调，不碰真实文件系统，任何环境全量执行。

#include "TestHarness.h"

#include "CompactTranscript.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace {

using TestHarness::check;

const QString kRoot = QStringLiteral("D:/lite-harness-s17.10-win64/config/.lite-harness/sessions/0123abcd");
const QString kTranscripts = kRoot + QStringLiteral("/.transcripts");

QJsonObject userMessage(const QString &content)
{
    QJsonObject message;
    message[QStringLiteral("role")] = QStringLiteral("user");
    message[QStringLiteral("content")] = content;
    return message;
}

QJsonObject assistantMessage(const QString &content)
{
    QJsonObject message;
    message[QStringLiteral("role")] = QStringLiteral("assistant");
    message[QStringLiteral("content")] = content;
    return message;
}

// 与 CompactManager::summaryMessage 同形（label / request / summary / transcript 四段）
QString summaryContent(const QString &label, const QString &transcriptPath)
{
    return QStringLiteral("[%1]\n\nCurrent user request:\nlast request\n\n"
                          "Conversation summary (reference only):\nsummary text\n\n"
                          "Full transcript: %2")
        .arg(label, transcriptPath);
}

QByteArray jsonl(const QVector<QJsonObject> &messages)
{
    QByteArray out;
    for (const QJsonObject &message : messages) {
        out.append(QJsonDocument(message).toJson(QJsonDocument::Compact));
        out.append('\n');
    }
    return out;
}

// 注入式"文件系统"：路径 → 内容；miss 即读失败
CompactTranscript::TranscriptReader makeReader(const QHash<QString, QByteArray> &files,
                                               int *callCount = nullptr)
{
    return [files, callCount](const QString &path, QByteArray *out) {
        if (callCount)
            ++(*callCount);
        const auto it = files.constFind(path);
        if (it == files.constEnd())
            return false;
        if (out)
            *out = it.value();
        return true;
    };
}

} // namespace

int tst_compacttranscript()
{
    const int before = TestHarness::failCount();

    // ---- ① 摘要消息判据（label 单源对齐 CompactManager 两个调用点）----
    check(CompactTranscript::isSummaryMessage(
              userMessage(summaryContent(QStringLiteral("Compacted"), QStringLiteral("x")))),
          "compacttranscript: [Compacted] recognized");
    check(CompactTranscript::isSummaryMessage(
              userMessage(summaryContent(QStringLiteral("Reactive compact"), QStringLiteral("x")))),
          "compacttranscript: [Reactive compact] recognized");
    check(!CompactTranscript::isSummaryMessage(userMessage(QStringLiteral("hello"))),
          "compacttranscript: plain user message not a summary");
    check(!CompactTranscript::isSummaryMessage(
              userMessage(QStringLiteral("[Compacted] but no transcript line"))),
          "compacttranscript: label without transcript line not a summary");
    check(!CompactTranscript::isSummaryMessage(
              assistantMessage(summaryContent(QStringLiteral("Compacted"), QStringLiteral("x")))),
          "compacttranscript: assistant role never a summary");
    check(!CompactTranscript::isSummaryMessage(QJsonObject()),
          "compacttranscript: empty object not a summary");

    // ---- ② `Full transcript:` 解析 ----
    const QString goodPath = kTranscripts + QStringLiteral("/transcript_abc.jsonl");
    check(CompactTranscript::transcriptPath(
              userMessage(summaryContent(QStringLiteral("Compacted"), goodPath)))
              == goodPath,
          "compacttranscript: transcript path parsed verbatim");
    check(CompactTranscript::transcriptPath(
              userMessage(summaryContent(QStringLiteral("Compacted"),
                                         QStringLiteral("(transcript unavailable)"))))
              .isEmpty(),
          "compacttranscript: unavailable placeholder yields empty path");
    check(CompactTranscript::transcriptPath(userMessage(QStringLiteral("no marker"))).isEmpty(),
          "compacttranscript: missing marker yields empty path");
    check(CompactTranscript::transcriptPath(
              userMessage(QStringLiteral("body\n\nFull transcript: %1\ntrailing line").arg(goodPath)))
              == goodPath,
          "compacttranscript: path stops at end of line");
    check(CompactTranscript::transcriptPath(QJsonObject()).isEmpty(),
          "compacttranscript: empty object yields empty path");

    // ---- ③ 路径守卫（防摘要正文指向任意文件）----
    check(CompactTranscript::isSafeTranscriptPath(kRoot, goodPath),
          "compacttranscript: path under .transcripts accepted");
    check(CompactTranscript::isSafeTranscriptPath(
              kRoot, kTranscripts + QStringLiteral("\\transcript_abc.jsonl")),
          "compacttranscript: backslash form normalized and accepted");
    check(!CompactTranscript::isSafeTranscriptPath(
              kRoot, kTranscripts + QStringLiteral("/sub/transcript_abc.jsonl")),
          "compacttranscript: subdirectory rejected");
    check(!CompactTranscript::isSafeTranscriptPath(
              kRoot, kTranscripts + QStringLiteral("/../history.json")),
          "compacttranscript: parent escape rejected");
    check(!CompactTranscript::isSafeTranscriptPath(
              kRoot, QStringLiteral("D:/other/.transcripts/transcript_abc.jsonl")),
          "compacttranscript: other session root rejected");
    check(!CompactTranscript::isSafeTranscriptPath(
              kRoot, kTranscripts + QStringLiteral("/snip_archive.jsonl")),
          "compacttranscript: snip archive name rejected");
    check(!CompactTranscript::isSafeTranscriptPath(
              kRoot, kTranscripts + QStringLiteral("/transcript_abc.json")),
          "compacttranscript: wrong extension rejected");
    check(!CompactTranscript::isSafeTranscriptPath(QString(), goodPath),
          "compacttranscript: empty session root rejected");
    check(!CompactTranscript::isSafeTranscriptPath(kRoot, QString()),
          "compacttranscript: empty path rejected");

    // ---- ④ JSONL 解析（坏行跳过，不整段失败）----
    QByteArray mixed = jsonl({userMessage(QStringLiteral("a")), assistantMessage(QStringLiteral("b"))});
    mixed.append("\n{not json}\n[1,2,3]\n\"scalar\"\n");
    check(CompactTranscript::parseTranscriptJsonl(mixed).size() == 2,
          "compacttranscript: bad lines skipped, good lines kept");
    check(CompactTranscript::parseTranscriptJsonl(QByteArray()).isEmpty(),
          "compacttranscript: empty bytes yields no messages");
    check(CompactTranscript::parseTranscriptJsonl(QByteArray("\n\n")).isEmpty(),
          "compacttranscript: blank lines yield no messages");

    // ---- ⑤ 单层展开：原文在前、摘要消息留在原位 ----
    const QVector<QJsonObject> archived{userMessage(QStringLiteral("old-1")),
                                        assistantMessage(QStringLiteral("old-2"))};
    QHash<QString, QByteArray> files;
    files.insert(goodPath, jsonl(archived));
    const QVector<QJsonObject> summaryOnly{
        userMessage(summaryContent(QStringLiteral("Compacted"), goodPath))};
    int calls = 0;
    const QVector<QJsonObject> expanded =
        CompactTranscript::expand(summaryOnly, kRoot, makeReader(files, &calls));
    check(expanded.size() == 3, "compacttranscript: expand yields archived + summary");
    check(calls == 1, "compacttranscript: one read per summary message");
    check(expanded.at(0).value(QStringLiteral("content")).toString() == QStringLiteral("old-1"),
          "compacttranscript: archived head first");
    check(expanded.at(1).value(QStringLiteral("content")).toString() == QStringLiteral("old-2"),
          "compacttranscript: archived tail second");
    check(CompactTranscript::isSummaryMessage(expanded.at(2)),
          "compacttranscript: summary message kept last");

    // ---- ⑥ 无摘要消息：原样返回（零读取）----
    const QVector<QJsonObject> plain{userMessage(QStringLiteral("u")),
                                     assistantMessage(QStringLiteral("a"))};
    int plainCalls = 0;
    const QVector<QJsonObject> untouched =
        CompactTranscript::expand(plain, kRoot, makeReader(files, &plainCalls));
    check(untouched.size() == 2 && plainCalls == 0,
          "compacttranscript: plain history untouched and no reads");

    // ---- ⑦ 两层嵌套：最老原文 → 内层摘要 → 外层摘要 ----
    const QString innerPath = kTranscripts + QStringLiteral("/transcript_inner.jsonl");
    const QString outerPath = kTranscripts + QStringLiteral("/transcript_outer.jsonl");
    QHash<QString, QByteArray> nested;
    nested.insert(outerPath,
                  jsonl({userMessage(QStringLiteral("oldest")),
                         userMessage(summaryContent(QStringLiteral("Compacted"), innerPath))}));
    nested.insert(innerPath, jsonl({userMessage(QStringLiteral("older"))}));
    const QVector<QJsonObject> nestedOut = CompactTranscript::expand(
        {userMessage(summaryContent(QStringLiteral("Compacted"), outerPath))}, kRoot,
        makeReader(nested));
    check(nestedOut.size() == 4, "compacttranscript: nested expand size");
    check(nestedOut.at(0).value(QStringLiteral("content")).toString() == QStringLiteral("oldest"),
          "compacttranscript: outer transcript head first");
    check(nestedOut.at(1).value(QStringLiteral("content")).toString() == QStringLiteral("older"),
          "compacttranscript: inner transcript expanded at its position");
    check(CompactTranscript::isSummaryMessage(nestedOut.at(2)),
          "compacttranscript: inner summary preserved");
    check(CompactTranscript::isSummaryMessage(nestedOut.at(3)),
          "compacttranscript: outer summary preserved");

    // ---- ⑧ 降级：读失败 / 路径不安全 / 空转写一律保留摘要消息 ----
    const QVector<QJsonObject> missing{
        userMessage(summaryContent(QStringLiteral("Compacted"),
                                   kTranscripts + QStringLiteral("/transcript_missing.jsonl")))};
    const QVector<QJsonObject> missingOut =
        CompactTranscript::expand(missing, kRoot, makeReader(files));
    check(missingOut.size() == 1 && CompactTranscript::isSummaryMessage(missingOut.at(0)),
          "compacttranscript: unreadable transcript keeps summary");

    const QVector<QJsonObject> unsafe{
        userMessage(summaryContent(QStringLiteral("Compacted"),
                                   QStringLiteral("D:/other/.transcripts/transcript_evil.jsonl")))};
    int unsafeCalls = 0;
    const QVector<QJsonObject> unsafeOut =
        CompactTranscript::expand(unsafe, kRoot, makeReader(files, &unsafeCalls));
    check(unsafeOut.size() == 1 && unsafeCalls == 0,
          "compacttranscript: unsafe path never read, summary kept");

    QHash<QString, QByteArray> emptyTranscript;
    emptyTranscript.insert(goodPath, QByteArray("\n{broken}\n"));
    const QVector<QJsonObject> emptyOut =
        CompactTranscript::expand(summaryOnly, kRoot, makeReader(emptyTranscript));
    check(emptyOut.size() == 1 && CompactTranscript::isSummaryMessage(emptyOut.at(0)),
          "compacttranscript: empty transcript keeps summary");

    // ---- ⑨ 防环：转写里含指向自身的摘要消息只展开一次 ----
    const QString loopPath = kTranscripts + QStringLiteral("/transcript_loop.jsonl");
    QHash<QString, QByteArray> loopFiles;
    loopFiles.insert(loopPath, jsonl({userMessage(QStringLiteral("loop-body")),
                                      userMessage(summaryContent(QStringLiteral("Compacted"),
                                                                 loopPath))}));
    const QVector<QJsonObject> loopOut = CompactTranscript::expand(
        {userMessage(summaryContent(QStringLiteral("Compacted"), loopPath))}, kRoot,
        makeReader(loopFiles));
    check(loopOut.size() == 3, "compacttranscript: self-reference expanded once only");
    check(loopOut.at(0).value(QStringLiteral("content")).toString() == QStringLiteral("loop-body"),
          "compacttranscript: loop body kept");

    // ---- ⑩ 深度上限：到顶后不再展开（保留摘要，不递归）----
    int depthCalls = 0;
    const QVector<QJsonObject> depthOut = CompactTranscript::expand(
        summaryOnly, kRoot, makeReader(files, &depthCalls), CompactTranscript::kMaxExpandDepth);
    check(depthOut.size() == 1 && depthCalls == 0,
          "compacttranscript: depth cap stops expansion before any read");

    return TestHarness::failCount() - before;
}