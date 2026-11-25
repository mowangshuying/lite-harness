// tst_toolcallacc.cpp —— SSE tool_calls 增量累加器纯头（src/ToolCallAccum.h，纯 QtCore）
// 被测：增量缓冲终局组装、占位过滤、index 对齐、id/type/name 覆写口径、arguments 键存在
//       即写回、首帧双写修复、reset 重试清零、与旧接线的逐字节等价性。
//
// 为什么必须测：本头是传输层 tool_calls 组装的唯一判据来源，六条链（主循环/子代理/
// 队友/workflow/压缩/记忆）的工具调用都经它。旧接线逐帧全量拼接 arguments（O(L²)），
// 且首帧非空 arguments 会被双写——两者都是无日志的错（前者是 GUI 线程卡顿、后者是坏
// JSON 致工具解析失败），只能靠断言钉。
//
// 等价性口径：内嵌「旧接线参照实现」（逐字复刻修复前 QOpenAi.cpp 的合并块 +
// finishStream 占位过滤）。首帧 arguments 为空/缺席的序列族断言新旧逐字节等价；
// 首帧非空族断言新实现修复了双写（旧参照恰好产出双写串，作为缺陷存在的证据一并钉住）。
// 零夹具恒跑。

#include "TestHarness.h"

#include "ToolCallAccum.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace {

using TestHarness::check;

// ---- 旧接线参照实现（逐字复刻修复前的 QOpenAi.cpp 合并块；仅用于等价性断言）----
void legacyFeed(QJsonArray &toolCalls, const QJsonObject &tc)
{
    const int index = tc.value(QStringLiteral("index")).toInt(-1);
    if (index < 0)
        return;
    while (toolCalls.size() <= index)
        toolCalls.append(QJsonObject());
    QJsonObject merged = toolCalls.at(index).toObject();
    if (merged.isEmpty())
        merged = tc;
    if (!tc.value(QStringLiteral("id")).toString().isEmpty())
        merged[QStringLiteral("id")] = tc.value(QStringLiteral("id"));
    if (!tc.value(QStringLiteral("type")).toString().isEmpty())
        merged[QStringLiteral("type")] = tc.value(QStringLiteral("type"));
    const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
    if (!fn.isEmpty())
    {
        QJsonObject mergedFn = merged.value(QStringLiteral("function")).toObject();
        if (!fn.value(QStringLiteral("name")).toString().isEmpty())
            mergedFn[QStringLiteral("name")] = fn.value(QStringLiteral("name"));
        if (fn.contains(QStringLiteral("arguments")))
        {
            const QString args = fn.value(QStringLiteral("arguments")).toString();
            mergedFn[QStringLiteral("arguments")] =
                mergedFn.value(QStringLiteral("arguments")).toString() + args;
        }
        merged[QStringLiteral("function")] = mergedFn;
    }
    toolCalls[index] = merged;
}

QJsonArray legacyFinalize(const QJsonArray &toolCalls)
{
    QJsonArray realCalls;
    for (const QJsonValue &v : toolCalls)
    {
        if (v.isObject() && !v.toObject().isEmpty())
            realCalls.append(v);
    }
    return realCalls;
}

QByteArray canon(const QJsonArray &arr)
{
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

// 构造一帧 delta.tool_calls 元素。约定：null QString = 该键缺席；
// 空串（非 null）= 键存在但值为空；args 指针为 nullptr = arguments 键缺席。
QJsonObject frame(int index, const QString &id, const QString &type,
                  const QString &name, const QString *args)
{
    QJsonObject tc;
    tc.insert(QStringLiteral("index"), index);
    if (!id.isNull())
        tc.insert(QStringLiteral("id"), id);
    if (!type.isNull())
        tc.insert(QStringLiteral("type"), type);
    QJsonObject fn;
    if (!name.isNull())
        fn.insert(QStringLiteral("name"), name);
    if (args)
        fn.insert(QStringLiteral("arguments"), *args);
    if (!fn.isEmpty())
        tc.insert(QStringLiteral("function"), fn);
    return tc;
}

} // namespace

int tst_toolcallacc()
{
    const int before = TestHarness::failCount();

    // ---- ① 分块组装（首帧 arguments 为空串——典型 OpenAI 流式形态）----
    {
        ToolCallAcc::Accumulator acc;
        const QString empty = QStringLiteral("");
        acc.feed(frame(0, QStringLiteral("call_1"), QStringLiteral("function"),
                       QStringLiteral("write_file"), &empty));
        const QString c1 = QStringLiteral("{\"path\":\"a.txt\",");
        const QString c2 = QStringLiteral("\"content\":\"hello\"}");
        acc.feed(frame(0, QString(), QString(), QString(), &c1));
        acc.feed(frame(0, QString(), QString(), QString(), &c2));
        const QJsonArray out = acc.finalize();
        check(out.size() == 1, "chunked: single tool call assembled");
        const QJsonObject obj = out.at(0).toObject();
        check(obj.value(QStringLiteral("id")).toString() == QStringLiteral("call_1"),
              "chunked: id kept from first frame");
        const QJsonObject fn = obj.value(QStringLiteral("function")).toObject();
        check(fn.value(QStringLiteral("name")).toString() == QStringLiteral("write_file"),
              "chunked: name kept from first frame");
        check(fn.value(QStringLiteral("arguments")).toString() == c1 + c2,
              "chunked: arguments concatenated in arrival order");
        check(obj.contains(QStringLiteral("index")),
              "chunked: first-frame extra field index preserved (on-disk format unchanged)");
    }

    // ---- ② 首帧双写修复（首帧携带非空 arguments）----
    {
        ToolCallAcc::Accumulator acc;
        QJsonArray legacy;
        const QString first = QStringLiteral("{\"path\":");
        const QString rest = QStringLiteral("\"a.txt\"}");
        const QJsonObject f0 = frame(0, QStringLiteral("call_1"), QStringLiteral("function"),
                                     QStringLiteral("write_file"), &first);
        acc.feed(f0);
        legacyFeed(legacy, f0);
        const QJsonObject f1 = frame(0, QString(), QString(), QString(), &rest);
        acc.feed(f1);
        legacyFeed(legacy, f1);
        const QString got = acc.finalize().at(0).toObject()
                                .value(QStringLiteral("function")).toObject()
                                .value(QStringLiteral("arguments")).toString();
        check(got == first + rest,
              "first-frame non-empty arguments counted once (double-write fixed)");
        const QString legacyGot = legacyFinalize(legacy).at(0).toObject()
                                      .value(QStringLiteral("function")).toObject()
                                      .value(QStringLiteral("arguments")).toString();
        check(legacyGot == first + first + rest,
              "legacy reference reproduces the double-write defect (fix targets a real bug)");
    }

    // ---- ③ 单帧完整 arguments（单发网关形态）----
    {
        ToolCallAcc::Accumulator acc;
        const QString whole = QStringLiteral("{\"command\":\"echo hi\"}");
        acc.feed(frame(0, QStringLiteral("call_9"), QStringLiteral("function"),
                       QStringLiteral("bash"), &whole));
        const QJsonObject fn = acc.finalize().at(0).toObject()
                                   .value(QStringLiteral("function")).toObject();
        check(fn.value(QStringLiteral("arguments")).toString() == whole,
              "single-frame whole arguments not doubled");
        check(fn.value(QStringLiteral("name")).toString() == QStringLiteral("bash"),
              "single-frame: name kept");
    }

    // ---- ④ 占位过滤与 index 排序（空洞 + 乱序到达）----
    {
        ToolCallAcc::Accumulator acc;
        const QString a = QStringLiteral("{}");
        acc.feed(frame(2, QStringLiteral("call_c"), QString(), QStringLiteral("t2"), &a));
        acc.feed(frame(0, QStringLiteral("call_a"), QString(), QStringLiteral("t0"), &a));
        const QJsonArray out = acc.finalize();
        check(out.size() == 2, "index gap placeholder filtered");
        check(out.at(0).toObject().value(QStringLiteral("id")).toString() == QStringLiteral("call_a")
                  && out.at(1).toObject().value(QStringLiteral("id")).toString() == QStringLiteral("call_c"),
              "output ordered by index, not arrival order");
    }

    // ---- ⑤ id/type/name 覆写口径（非空覆写、空串不覆写）----
    {
        ToolCallAcc::Accumulator acc;
        const QString empty = QStringLiteral("");
        acc.feed(frame(0, QStringLiteral("id-old"), QStringLiteral("function"),
                       QStringLiteral("name-old"), &empty));
        const QString chunk = QStringLiteral("x");
        acc.feed(frame(0, QStringLiteral("id-new"), QString(), QStringLiteral("name-new"), &chunk));
        acc.feed(frame(0, QStringLiteral(""), QStringLiteral(""), QStringLiteral(""), &chunk));
        const QJsonObject obj = acc.finalize().at(0).toObject();
        check(obj.value(QStringLiteral("id")).toString() == QStringLiteral("id-new"),
              "non-empty id overwrites");
        check(obj.value(QStringLiteral("type")).toString() == QStringLiteral("function"),
              "empty type does not overwrite");
        check(obj.value(QStringLiteral("function")).toObject()
                  .value(QStringLiteral("name")).toString() == QStringLiteral("name-new"),
              "non-empty name overwrites");
        check(obj.value(QStringLiteral("function")).toObject()
                  .value(QStringLiteral("arguments")).toString() == QStringLiteral("xx"),
              "empty-string id/type/name frames still append arguments");
    }

    // ---- ⑥ arguments 键存在即写回（空串）+ 首帧 function:{} 原样保留 ----
    {
        ToolCallAcc::Accumulator acc;
        const QString empty = QStringLiteral("");
        acc.feed(frame(0, QStringLiteral("c"), QString(), QString(), &empty));
        const QJsonObject fn = acc.finalize().at(0).toObject()
                                   .value(QStringLiteral("function")).toObject();
        check(fn.contains(QStringLiteral("arguments"))
                  && fn.value(QStringLiteral("arguments")).toString().isEmpty(),
              "arguments key present even when empty string (contains criterion)");
    }
    {
        ToolCallAcc::Accumulator acc;
        QJsonObject tc;
        tc.insert(QStringLiteral("index"), 0);
        tc.insert(QStringLiteral("function"), QJsonObject{});
        acc.feed(tc);
        const QJsonObject obj = acc.finalize().at(0).toObject();
        check(obj.contains(QStringLiteral("function"))
                  && obj.value(QStringLiteral("function")).toObject().isEmpty(),
              "first-frame empty function object preserved verbatim");
    }

    // ---- ⑦ 后续帧 function 非空即确保键存在（旧接线写回口径，参照等价）----
    {
        ToolCallAcc::Accumulator acc;
        QJsonObject f0;
        f0.insert(QStringLiteral("index"), 0);
        f0.insert(QStringLiteral("id"), QStringLiteral("c"));
        acc.feed(f0);
        QJsonObject f1;
        f1.insert(QStringLiteral("index"), 0);
        QJsonObject fn;
        fn.insert(QStringLiteral("x"), 1); // 非空但无 name/arguments
        f1.insert(QStringLiteral("function"), fn);
        acc.feed(f1);
        QJsonArray legacy;
        legacyFeed(legacy, f0);
        legacyFeed(legacy, f1);
        check(canon(acc.finalize()) == canon(legacyFinalize(legacy)),
              "non-empty function without name/arguments still ensures function key (legacy parity)");
    }

    // ---- ⑧ 无效 index 跳过 ----
    {
        ToolCallAcc::Accumulator acc;
        QJsonObject noIndex;
        noIndex.insert(QStringLiteral("id"), QStringLiteral("c"));
        acc.feed(noIndex);
        QJsonObject neg;
        neg.insert(QStringLiteral("index"), -1);
        acc.feed(neg);
        check(acc.finalize().isEmpty(), "missing/negative index skipped");
    }

    // ---- ⑨ reset 重试清零 ----
    {
        ToolCallAcc::Accumulator acc;
        const QString a = QStringLiteral("{}");
        acc.feed(frame(0, QStringLiteral("c"), QString(), QStringLiteral("t"), &a));
        acc.reset();
        check(acc.finalize().isEmpty(), "reset clears accumulation (retry semantics)");
        acc.feed(frame(0, QStringLiteral("c2"), QString(), QStringLiteral("t2"), &a));
        check(acc.finalize().at(0).toObject().value(QStringLiteral("id")).toString()
                  == QStringLiteral("c2"),
              "accumulator reusable after reset");
    }

    // ---- ⑩ 等价性语料：首帧空/缺席族，新旧逐字节等价（确定性 LCG，可复现）----
    {
        quint32 seed = 0x5EEDu;
        const auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
        int mismatches = 0;
        for (int seq = 0; seq < 300; ++seq)
        {
            ToolCallAcc::Accumulator acc;
            QJsonArray legacy;
            bool seen[4] = {false, false, false, false};
            const int frames = 1 + static_cast<int>(next() % 24);
            for (int f = 0; f < frames; ++f)
            {
                const int index = static_cast<int>(next() % 4);
                QJsonObject tc;
                tc.insert(QStringLiteral("index"), index);
                if (next() % 3 == 0)
                    tc.insert(QStringLiteral("id"), QStringLiteral("id%1").arg(next() % 5));
                if (next() % 4 == 0)
                    tc.insert(QStringLiteral("type"), QStringLiteral("function"));
                QJsonObject fn;
                if (next() % 3 == 0)
                    fn.insert(QStringLiteral("name"), QStringLiteral("tool%1").arg(next() % 3));
                const bool firstFrame = !seen[index];
                if (next() % 2 == 0)
                {
                    // 族约束：每个 index 的首帧 arguments 只给空串（或缺席），后续帧任意
                    // 增量——这正是旧接线无双写、可逐字节等价的形态。
                    const QString chunk = firstFrame
                        ? QStringLiteral("")
                        : QStringLiteral("c%1").arg(next() % 100);
                    fn.insert(QStringLiteral("arguments"), chunk);
                }
                if (next() % 5 == 0)
                    fn.insert(QStringLiteral("x"), 1); // 非空 function 无 name/arguments 路径
                if (!fn.isEmpty())
                    tc.insert(QStringLiteral("function"), fn);
                seen[index] = true;
                acc.feed(tc);
                legacyFeed(legacy, tc);
            }
            if (canon(acc.finalize()) != canon(legacyFinalize(legacy)))
                ++mismatches;
        }
        check(mismatches == 0,
              "legacy parity: 300 deterministic sequences (first-frame-empty family) byte-identical");
    }

    // ---- ⑪ 大 arguments 分块组装（50KB 形态，钉正确性；O(L) 缓冲路径）----
    {
        ToolCallAcc::Accumulator acc;
        const QString empty = QStringLiteral("");
        acc.feed(frame(0, QStringLiteral("big"), QStringLiteral("function"),
                       QStringLiteral("write_file"), &empty));
        QString expected;
        const QString chunk(1000, QLatin1Char('x'));
        for (int i = 0; i < 50; ++i)
        {
            acc.feed(frame(0, QString(), QString(), QString(), &chunk));
            expected += chunk;
        }
        check(acc.finalize().at(0).toObject().value(QStringLiteral("function")).toObject()
                  .value(QStringLiteral("arguments")).toString() == expected,
              "50KB chunked arguments assembled exactly (O(L) buffer path)");
    }

    // ---- ⑫ arguments 非字符串形态（旧接线 toString 降级空串，口径不变）----
    {
        ToolCallAcc::Accumulator acc;
        QJsonObject tc;
        tc.insert(QStringLiteral("index"), 0);
        QJsonObject fn;
        fn.insert(QStringLiteral("name"), QStringLiteral("t"));
        fn.insert(QStringLiteral("arguments"), QJsonObject{}); // 非字符串：toString() → 空串
        tc.insert(QStringLiteral("function"), fn);
        acc.feed(tc);
        QJsonArray legacy;
        legacyFeed(legacy, tc);
        check(canon(acc.finalize()) == canon(legacyFinalize(legacy)),
              "non-string arguments degrades to empty string (legacy parity)");
    }

    // ---- ⑬ 双工具调用交错分块（每 index 缓冲互不串）----
    {
        ToolCallAcc::Accumulator acc;
        const QString empty = QStringLiteral("");
        acc.feed(frame(0, QStringLiteral("a"), QStringLiteral("function"), QStringLiteral("ta"), &empty));
        acc.feed(frame(1, QStringLiteral("b"), QStringLiteral("function"), QStringLiteral("tb"), &empty));
        const QString a1 = QStringLiteral("{\"x\":");
        const QString b1 = QStringLiteral("{\"y\":");
        acc.feed(frame(0, QString(), QString(), QString(), &a1));
        acc.feed(frame(1, QString(), QString(), QString(), &b1));
        const QString a2 = QStringLiteral("1}");
        const QString b2 = QStringLiteral("2}");
        acc.feed(frame(0, QString(), QString(), QString(), &a2));
        acc.feed(frame(1, QString(), QString(), QString(), &b2));
        const QJsonArray out = acc.finalize();
        check(out.size() == 2
                  && out.at(0).toObject().value(QStringLiteral("function")).toObject()
                         .value(QStringLiteral("arguments")).toString() == a1 + a2
                  && out.at(1).toObject().value(QStringLiteral("function")).toObject()
                         .value(QStringLiteral("arguments")).toString() == b1 + b2,
              "interleaved two-call chunking keeps per-index buffers separate");
    }

    return TestHarness::failCount() - before;
}
