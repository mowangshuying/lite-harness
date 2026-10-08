// MessageBus 单测（lcc s13 移植）：send→drain 往返、至多一次投递、hasPending 三态、
// 三重 fail-closed 路径校验、畸形行防毒（D9-defensive）、unlink 失败 fail-closed（M8）、
// 追加顺序与默认参。
// 套件临时根经 ScopedTempRoot（受 LITE_TEST_TMPROOT 环境变量管辖，禁用裸 QDir::tempPath
// 自清理——曾有姊妹套件失控清理酿成事故）；未配置则整组 SKIP 返回 0。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "MessageBus.h"
#include "AgentConstants.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>

namespace {

// 邮箱目录/文件路径（测试侧自拼，用于直接布数据与验文件消失；根经参数下传，防全局态）
QString mailboxesDir(const QString &root)
{
    return QDir(root).filePath(AgentConst::kMailboxesDirName);
}

QString mailboxPath(const QString &root, const QString &name)
{
    return QDir(mailboxesDir(root)).filePath(name + QStringLiteral(".jsonl"));
}

MessageBus makeBus(const QString &root)
{
    return MessageBus([root]() -> QString { return root; });
}

// ① 合法 send→drain 往返（ts float 秒、metadata 保真）+ drain 后文件消失（至多一次）
void testSendDrainRoundTrip(const QString &root)
{
    MessageBus bus = makeBus(root);

    QJsonObject meta;
    meta[QStringLiteral("request_id")] = QStringLiteral("req_000001");
    meta[QStringLiteral("count")] = 7;
    const QString cnContent = QString::fromUtf8("计划已出");

    const bool ok = bus.send(QStringLiteral("lead"), QStringLiteral("worker1"), cnContent,
                             QStringLiteral("plan_request"), meta);
    TestHarness::check(ok, "send: valid recipient accepted");
    TestHarness::check(bus.lastError().isEmpty(), "send: no lastError on success");
    TestHarness::check(QFile::exists(mailboxPath(root, QStringLiteral("worker1"))),
                       "send: mailbox file created at <root>/.mailboxes/worker1.jsonl");
    TestHarness::check(bus.hasPending(QStringLiteral("worker1")), "peek: hasPending true after send");

    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("worker1"));
    TestHarness::check(msgs.size() == 1, "drain: exactly one message");
    if (msgs.size() == 1) {
        const BusMessage &m = msgs.first();
        TestHarness::check(m.from == QLatin1String("lead"), "drain: from field round-trips");
        TestHarness::check(m.to == QLatin1String("worker1"), "drain: to field round-trips");
        TestHarness::check(m.content == cnContent, "drain: UTF-8 content round-trips");
        TestHarness::check(m.type == QLatin1String("plan_request"), "drain: type round-trips");
        // D3：ts 为浮点秒（time.time() 口径）。当前 epoch 秒约 1.7e9，夹逼 1e9~1e11 防写成毫秒/字符串
        TestHarness::check(m.ts > 1e9 && m.ts < 1e11, "drain: ts is float seconds (not ms/string)");
        TestHarness::check(m.metadata.value(QStringLiteral("request_id")).toString()
                               == QLatin1String("req_000001"),
                           "drain: metadata.request_id preserved");
        TestHarness::check(m.metadata.value(QStringLiteral("count")).toInt() == 7,
                           "drain: metadata.count preserved");
    }

    // 破坏性整读：读后即 unlink，邮箱消失
    TestHarness::check(!QFile::exists(mailboxPath(root, QStringLiteral("worker1"))),
                       "drain: mailbox file removed (at-most-once)");
    TestHarness::check(!bus.hasPending(QStringLiteral("worker1")), "peek: hasPending false after drain");
}

// ② hasPending 三态：不存在 / 0 字节崩溃残留 / 有内容
void testHasPendingThreeStates(const QString &root)
{
    MessageBus bus = makeBus(root);

    TestHarness::check(!bus.hasPending(QStringLiteral("ghost")), "peek: absent mailbox is false");

    QDir().mkpath(mailboxesDir(root));
    QFile zero(mailboxPath(root, QStringLiteral("zero")));
    const bool made = zero.open(QIODevice::WriteOnly);
    zero.close();
    TestHarness::check(made && QFileInfo(zero).size() == 0, "setup: 0-byte mailbox residue created");
    TestHarness::check(!bus.hasPending(QStringLiteral("zero")),
                       "peek: 0-byte residue is false (anti-livelock)");

    TestHarness::check(bus.send(QStringLiteral("a"), QStringLiteral("fill"), QStringLiteral("x")),
                       "send: into fill mailbox ok");
    TestHarness::check(bus.hasPending(QStringLiteral("fill")), "peek: non-empty mailbox is true");
}

// ③ drain 对不存在邮箱返回空且不记错误（lcc read 语义：无文件=无信，非异常）
void testDrainEmptyNotError(const QString &root)
{
    MessageBus bus = makeBus(root);
    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("nowhere"));
    TestHarness::check(msgs.isEmpty(), "drain: absent mailbox yields empty");
    TestHarness::check(bus.lastError().isEmpty(), "drain: absent mailbox is not an error");
}

// ③b unlink 失败 = fail-closed（M8 编排者裁决=①）：另一句柄占住邮箱使 remove 失败时，
// 本批不投递（返回空批）、lastError 置错、信箱保留（hasPending 仍 true，靠心跳下一拍重试）。
// 构造可行性：Qt QFile 在 Windows 下以「不共享」方式开句柄（实证：同类场景 send 的 append
// open 都会被 sharing violation 拒），故第二个只读句柄占住文件即可让 file.remove() 失败。
// 若本构造被平台放行（remove 竟成功），如实判 FAIL 交编排者裁决，不假造通过。
void testDrainUnlinkFailClosed(const QString &root)
{
    MessageBus bus = makeBus(root);
    TestHarness::check(bus.send(QStringLiteral("a"), QStringLiteral("busy"), QStringLiteral("x")),
                       "unlink-fail: setup send ok");

    QFile blocker(mailboxPath(root, QStringLiteral("busy")));
    const bool held = blocker.open(QIODevice::ReadOnly);
    TestHarness::check(held, "unlink-fail: setup holds an open read handle on mailbox");
    if (!held)
        return; // 夹具未成（如文件被意外占用/权限异常），不跑后续断言

    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("busy"));
    TestHarness::check(msgs.isEmpty(),
                       "unlink-fail: parsed batch discarded (fail-closed, not silently delivered)");
    TestHarness::check(!bus.lastError().isEmpty(), "unlink-fail: lastError set");
    TestHarness::check(QFile::exists(mailboxPath(root, QStringLiteral("busy"))),
                       "unlink-fail: mailbox retained for heartbeat retry");
    TestHarness::check(bus.hasPending(QStringLiteral("busy")),
                       "unlink-fail: doorbell still armed (retry path alive)");
    // 句柄仍被 blocker 占住：此处故意不 drain 重试（remove 必再失败，语义已由上面断言钉死）
    blocker.close();
}

// ④ 坏名字三重校验：send/drain/hasPending 一律拒绝并置 lastError；64 字符边界通过；from 不校验
void testBadNamesRejected(const QString &root)
{
    MessageBus bus = makeBus(root);

    const QStringList bad = {
        QString(),                        // 空串
        QStringLiteral("../evil"),        // 目录穿越（含 '/'）
        QStringLiteral("a/b"),            // 斜杠
        QStringLiteral("a\\b"),           // 反斜杠
        QString(65, QLatin1Char('a')),    // 超 64 字符
        QStringLiteral("has.dot"),        // 含点
        QStringLiteral("sp ace"),         // 含空格
        QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87"), // 中文（非 ASCII 白名单）
    };
    for (const QString &name : bad) {
        TestHarness::check(!bus.send(QStringLiteral("lead"), name, QStringLiteral("x")),
                           "reject: send to bad name refused");
        TestHarness::check(!bus.lastError().isEmpty(), "reject: send sets lastError");
        TestHarness::check(bus.drain(name).isEmpty(), "reject: drain of bad name yields empty");
        TestHarness::check(!bus.lastError().isEmpty(), "reject: drain sets lastError");
        TestHarness::check(!bus.hasPending(name), "reject: hasPending of bad name is false");
    }

    // 边界：恰 64 字符合法
    const QString name64(64, QLatin1Char('z'));
    TestHarness::check(bus.send(QStringLiteral("lead"), name64, QStringLiteral("x")),
                       "boundary: 64-char name accepted");

    // from 为空但收件名合法：lcc 只校验 to，不校验 from，应放行
    TestHarness::check(bus.send(QString(), QStringLiteral("emptyfrom"), QStringLiteral("x")),
                       "from is not validated (empty from + valid to accepted)");
}

// ⑤ 畸形行防毒（D9-defensive，有意偏离 lcc raise）：坏 JSON / 缺键行跳过，合法行仍送达
void testMalformedLinesSkipped(const QString &root)
{
    QDir().mkpath(mailboxesDir(root));
    QFile file(mailboxPath(root, QStringLiteral("merge")));
    TestHarness::check(file.open(QIODevice::WriteOnly | QIODevice::Append), "setup: open mailbox raw");
    // 手工构造邮箱内容：合法 / 非 JSON / 合法 JSON 但缺 content / 合法 / 空行
    file.write(R"({"from":"lead","to":"merge","content":"good","type":"message","ts":100.5,"metadata":{}})");
    file.write("\n");
    file.write("not-json-at-all");
    file.write("\n");
    file.write(R"({"from":"lead","to":"merge","type":"message","ts":1.0,"metadata":{}})"); // 缺 content
    file.write("\n");
    file.write(R"({"from":"lead","to":"merge","content":"good2","type":"result","ts":3.0,"metadata":{"k":1}})");
    file.write("\n");
    file.write("\n"); // 空行
    file.close();

    MessageBus bus = makeBus(root);
    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("merge"));
    TestHarness::check(msgs.size() == 2, "malformed: only 2 valid rows survive, poison skipped");
    if (msgs.size() == 2) {
        TestHarness::check(msgs[0].content == QLatin1String("good"), "malformed: first valid row kept");
        TestHarness::check(msgs[1].content == QLatin1String("good2"), "malformed: later valid row still delivered");
        TestHarness::check(msgs[1].type == QLatin1String("result"), "malformed: type parsed");
        TestHarness::check(msgs[1].ts == 3.0, "malformed: ts parsed as float");
        TestHarness::check(msgs[1].metadata.value(QStringLiteral("k")).toInt() == 1,
                           "malformed: metadata object parsed");
    }
    TestHarness::check(!QFile::exists(mailboxPath(root, QStringLiteral("merge"))),
                       "malformed: mailbox removed after drain");
}

// ⑥ 多消息追加顺序保持（send→send→send，drain 依写入序）
void testAppendOrder(const QString &root)
{
    MessageBus bus = makeBus(root);
    bus.send(QStringLiteral("lead"), QStringLiteral("seq"), QStringLiteral("1"));
    bus.send(QStringLiteral("lead"), QStringLiteral("seq"), QStringLiteral("2"));
    bus.send(QStringLiteral("lead"), QStringLiteral("seq"), QStringLiteral("3"));

    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("seq"));
    TestHarness::check(msgs.size() == 3, "append: three messages accumulated");
    if (msgs.size() == 3) {
        TestHarness::check(msgs[0].content == QLatin1String("1"), "append: order [0]=1");
        TestHarness::check(msgs[1].content == QLatin1String("2"), "append: order [1]=2");
        TestHarness::check(msgs[2].content == QLatin1String("3"), "append: order [2]=3");
    }
}

// ⑦ 默认参：type 默认 message，metadata 默认空对象
void testDefaultArgs(const QString &root)
{
    MessageBus bus = makeBus(root);
    TestHarness::check(bus.send(QStringLiteral("lead"), QStringLiteral("def"), QStringLiteral("body")),
                       "default: send with default type/metadata ok");
    const QVector<BusMessage> msgs = bus.drain(QStringLiteral("def"));
    TestHarness::check(msgs.size() == 1, "default: exactly one message");
    if (msgs.size() == 1) {
        TestHarness::check(msgs.first().type == QLatin1String("message"), "default: type defaults to message");
        TestHarness::check(msgs.first().metadata.isEmpty(), "default: metadata defaults to empty object");
    }
}

} // namespace

int tst_messagebus()
{
    ScopedTempRoot tmp(QStringLiteral("messagebus"));
    if (!tmp.isValid()) {
        // TestHarness 无打印原语（仅 check/计数），SKIP 通知走 <cstdio>，不计失败、直接返回 0。
        std::printf("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return 0;
    }

    const int before = TestHarness::failCount();
    const QString root = tmp.path();

    testSendDrainRoundTrip(root);
    testHasPendingThreeStates(root);
    testDrainEmptyNotError(root);
    testDrainUnlinkFailClosed(root);
    testBadNamesRejected(root);
    testMalformedLinesSkipped(root);
    testAppendOrder(root);
    testDefaultArgs(root);

    return TestHarness::failCount() - before;
}
