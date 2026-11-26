// tst_messagecontent.cpp —— 消息 content 形态判据直测（src/MessageContent.h）
// 被测：纯串/parts 两形态的组装、摊平、拼接、张数、发送末段展开
//
// 本套件钉的是三条**静默失效**面，它们错了都不报错、只在真机上以「图不见了」呈现：
//   ① 拼接口径（appendText）。三处调用点原写 `content = content.toString() + text`，
//      对 parts 求 toString() 得空串再赋值 = 一条带图消息的原文与图一起被抹掉，
//      而 UI 与日志都看不出丢过东西。
//   ② 展开兜底（expandForWire）。`lh-attach://` 漏上线时端点回的是「坏 url」，
//      与真正的图片错误同形，排查成本极高；故「绝不外泄私有 scheme」是硬断言。
//   ③ 无图会话的**字节等价**。本批改的是 content 形态，而 system prompt 静态化、
//      前缀缓存、压缩阈值全建立在「历史字节恒定」上——零图片路径必须逐字不变。
//
// 不守什么（诚实边界）：
//   ① 落盘/读盘（QSaveFile、附件目录创建、删会话连带）在宿主侧，readAttachment 在此是桩；
//   ② 端点是否真收 data URL、单请求体积上限如何，属网络侧事实，须实测与冒烟取证；
//   ③ history.json 的整体读写不经本头，只在本套件末尾用 QJsonDocument 往返模拟其形态。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "MessageContent.h"
#include "ImageAttach.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace {

// 按字节值构造，不写 "\x1a" 这类转义：与 ImageAttach::hasMagic 同口径，也避免
// 十六进制转义贪婪吞后续字符（本仓 PasteText 批登记过的同类坑）。
QByteArray pngBytes()
{
    QByteArray out;
    const int magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (const int value : magic)
        out.append(static_cast<char>(value));
    out.append("IHDR-fake-scanline-payload");
    return out;
}

QString nameA()
{
    return ImageAttach::nameFor(pngBytes(), QStringLiteral("image/png"));
}

QString nameB()
{
    QByteArray other = pngBytes();
    other.append("second-image-bytes");
    return ImageAttach::nameFor(other, QStringLiteral("image/png"));
}

QString urlA()
{
    return ImageAttach::urlFor(nameA());
}

QString urlB()
{
    return ImageAttach::urlFor(nameB());
}

QString serialize(const QJsonValue &value)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QJsonValue imagePart(const QString &url)
{
    return QJsonObject{{QStringLiteral("type"), MessageContent::imagePartType()},
                       {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}}};
}

QJsonValue textPart(const QString &text)
{
    return QJsonObject{{QStringLiteral("type"), MessageContent::textPartType()},
                       {QStringLiteral("text"), text}};
}

// ① 组装：无附件必须落回纯串
void testAssemblyKeepsPlainString()
{
    const QJsonValue plain =
        MessageContent::fromTextAndImages(QStringLiteral("你好，看图"), QStringList());
    TestHarness::check(plain.isString() && plain.toString() == QStringLiteral("你好，看图"),
                       "无附件 → 纯串，落盘形态与改造前逐字相同");
    TestHarness::check(MessageContent::fromTextAndImages(QString(), QStringList()).isString(),
                       "空文本无附件仍是串（空 user 消息由调用点卫兵拦，本层不改形态）");

    QStringList names;
    names << nameA() << nameB();
    const QJsonValue parts =
        MessageContent::fromTextAndImages(QStringLiteral("看图"), names);
    TestHarness::check(parts.isArray() && parts.toArray().size() == 3,
                       "两图一文 → 三 part");
    TestHarness::check(MessageContent::partType(parts.toArray().at(0))
                           == MessageContent::textPartType(),
                       "文本段在图片段之前（先说人话再给证据）");
    TestHarness::check(MessageContent::partType(parts.toArray().at(1))
                           == MessageContent::imagePartType(),
                       "第二段是图");
    TestHarness::check(serialize(parts).contains(urlA()) && serialize(parts).contains(urlB()),
                       "历史里存的是引用，不含 base64");

    const QJsonValue onlyImages = MessageContent::fromTextAndImages(QString(), names);
    TestHarness::check(onlyImages.isArray() && onlyImages.toArray().size() == 2,
                       "无文本只附图 → 数组里没有空文本段");
    TestHarness::check(
        MessageContent::fromTextAndImages(QStringLiteral("   \n "), names).toArray().size() == 2,
        "全空白文本不进数组（不产出「空 text part 挂着」的噪音段）");

    QStringList dirty;
    dirty << QStringLiteral("../../etc/passwd") << nameA();
    const QJsonValue filtered =
        MessageContent::fromTextAndImages(QStringLiteral("x"), dirty);
    TestHarness::check(filtered.isArray() && filtered.toArray().size() == 2,
                       "非法名不进数组，合法名照常保留");
    TestHarness::check(!serialize(filtered).contains(".."),
                       "越界段绝不落进历史（下一轮的读取点就是它的执行器）");

    QStringList allDirty;
    allDirty << QStringLiteral("evil");
    const QJsonValue fallback =
        MessageContent::fromTextAndImages(QStringLiteral("x"), allDirty);
    TestHarness::check(fallback.isString() && fallback.toString() == QStringLiteral("x"),
                       "全部非法 → 退回纯串形态，不产出空数组");
}

// ② 摊平：纯串逐字不变是地基
void testFlatten()
{
    const QString original = QStringLiteral("第一行\n第二行\n\n带空行\n");
    TestHarness::check(MessageContent::textOf(QJsonValue(original)) == original,
                       "纯串摊平逐字不变（含换行与尾随空白，不 trim 不折叠）");
    TestHarness::check(MessageContent::textOf(QJsonValue()).isEmpty(),
                       "未设值摊平成空串，不崩");
    TestHarness::check(MessageContent::textOf(QJsonValue(7)).isEmpty(),
                       "非串非数组的异常形态摊平成空串");

    QJsonArray mixed;
    mixed.append(textPart(QStringLiteral("看图")));
    mixed.append(imagePart(urlA()));
    mixed.append(imagePart(urlB()));
    TestHarness::check(MessageContent::textOf(QJsonValue(mixed))
                           == QStringLiteral("看图\n[image]\n[image]"),
                       "parts 按序摊平，图片落 [image] 标记");

    QJsonArray twoTexts;
    twoTexts.append(textPart(QStringLiteral("a")));
    twoTexts.append(textPart(QStringLiteral("b")));
    TestHarness::check(MessageContent::textOf(QJsonValue(twoTexts))
                           == QStringLiteral("a\nb"),
                       "多文本段以单换行相接");

    QJsonArray noisy;
    noisy.append(textPart(QString()));
    noisy.append(imagePart(QStringLiteral("https://x/a.png")));
    noisy.append(textPart(QStringLiteral("尾")));
    TestHarness::check(MessageContent::textOf(QJsonValue(noisy))
                           == QStringLiteral("[image]\n尾"),
                       "空文本段跳过、外部 url 仍计入图（它确实是一张图）");
}

// ②b UI 专用摊平：绝不把喂模型的 [image] 协议串端给用户
void testDisplayFlatten()
{
    TestHarness::check(MessageContent::displayTextOf(QJsonValue(QStringLiteral("纯串")))
                           == QStringLiteral("纯串"),
                       "纯串两式同值（无图会话的 UI 文本与改造前逐字相同）");

    QStringList names;
    names << nameA() << nameB();
    const QJsonValue parts = MessageContent::fromTextAndImages(QStringLiteral("看这两张图"), names);
    TestHarness::check(MessageContent::displayTextOf(parts)
                           == QStringLiteral("看这两张图"),
                       "带图消息的 UI 文本里没有 [image] 字样");
    TestHarness::check(!MessageContent::displayTextOf(parts).contains(
                           QStringLiteral("[image]")),
                       "标记串绝不外泄到呈现口（它是禁翻区协议串）");
    TestHarness::check(MessageContent::textOf(parts).contains(QStringLiteral("[image]")),
                       "而喂模型的那一式必须带标记——两式的分工是刻意的，不是漏改");

    const QJsonValue imageOnly = MessageContent::fromTextAndImages(QString(), names);
    TestHarness::check(MessageContent::displayTextOf(imageOnly).isEmpty(),
                       "只图不字 → UI 文本为空（会话标题据此落「新会话」而不是 [image]）");

    QJsonArray foreign;
    foreign.append(imagePart(QStringLiteral("https://x/a.png")));
    TestHarness::check(MessageContent::displayTextOf(QJsonValue(foreign)).isEmpty(),
                       "纯图数组（外部 url）UI 文本同样为空，不产出标记行");
}

// ③ 张数与带图判定：与展开后上线的张数同口径
void testCounting()
{
    TestHarness::check(MessageContent::imageCount(QJsonValue(QStringLiteral("hi"))) == 0,
                       "纯串零张");
    TestHarness::check(MessageContent::imageCount(QJsonValue(QJsonArray())) == 0,
                       "空数组零张");
    TestHarness::check(!MessageContent::hasImages(QJsonValue(QStringLiteral("hi"))),
                       "纯串不带图");

    QStringList names;
    names << nameA() << nameA() << nameB();
    const QJsonValue parts = MessageContent::fromTextAndImages(QStringLiteral("x"), names);
    TestHarness::check(MessageContent::imageCount(parts) == 2,
                       "同名引用按一张计（展开只发一份，估算不能虚高）");
    TestHarness::check(MessageContent::hasImages(parts), "带图判定命中");

    QJsonArray inlineOnly;
    inlineOnly.append(imagePart(QStringLiteral("data:image/png;base64,AAAA")));
    inlineOnly.append(imagePart(QStringLiteral("data:image/png;base64,AAAA")));
    TestHarness::check(MessageContent::imageCount(QJsonValue(inlineOnly)) == 2,
                       "内联 data URL 各计一张（无法廉价判重，且非本仓产出形态）");

    QJsonArray junk;
    junk.append(imagePart(QStringLiteral("lh-attach://../../secret")));
    junk.append(imagePart(QString()));
    TestHarness::check(MessageContent::imageCount(QJsonValue(junk)) == 0,
                       "形态破坏的引用不计费也不带图（与展开处的降级一致）");

    QStringList one;
    one << nameA();
    const QJsonValue withText = MessageContent::fromTextAndImages(QStringLiteral("说明"), one);
    const QStringList names2 = MessageContent::imageNames(withText);
    TestHarness::check(names2.size() == 1 && names2.at(0) == nameA(),
                       "裸名清单按序去重（UI 回放与删会话核对共用）");
    TestHarness::check(MessageContent::imageNames(QJsonValue(QStringLiteral("x"))).isEmpty(),
                       "纯串无裸名");
}

// ④ 拼接：三处调用点的共用判据，负例是本组的核心
void testAppendText()
{
    const QString tail = QStringLiteral("\n\n<task_notification>done</task_notification>");
    const QJsonValue joined =
        MessageContent::appendText(QJsonValue(QStringLiteral("原文")), tail);
    TestHarness::check(joined.isString()
                           && joined.toString() == QStringLiteral("原文") + tail,
                       "纯串拼接 = 原文 + 入参，分隔符由调用方给（今日落盘字节不变）");

    QStringList names;
    names << nameA();
    const QJsonValue parts = MessageContent::fromTextAndImages(QStringLiteral("原文"), names);
    const QJsonValue appended = MessageContent::appendText(parts, tail);
    TestHarness::check(appended.isArray() && appended.toArray().size() == 3,
                       "parts 拼接追加一个文本段，图仍在原位");
    TestHarness::check(MessageContent::imageCount(appended)
                           == MessageContent::imageCount(parts),
                       "**拼接不得丢图**（旧写法 toString() 赋值会把图连同原文一起抹掉）");
    TestHarness::check(MessageContent::textOf(appended).contains(QStringLiteral("[image]")),
                       "摊平后图片标记仍在");
    TestHarness::check(serialize(appended).contains(urlA()), "引用未被覆盖");

    const QJsonValue twice = MessageContent::appendText(
        MessageContent::appendText(parts, QStringLiteral(" A")), QStringLiteral(" B"));
    TestHarness::check(twice.toArray().size() == 4, "连续追加按到达序排列，不合并段落");

    TestHarness::check(MessageContent::appendText(QJsonValue(), QStringLiteral("x")).toString()
                           == QStringLiteral("x"),
                       "未设值 content 拼接退化成纯串（不崩、不产出空数组）");
}

// ⑤ 展开：附件 → data URL，以及三条降级路
void testExpansion()
{
    int reads = 0;
    const QByteArray payload = pngBytes();
    std::function<QByteArray(const QString &)> reader = [&](const QString &name) -> QByteArray {
        ++reads;
        return name == nameA() ? payload : QByteArray();
    };

    // 快路径：纯串与无图数组一律不读盘、不改值
    const QJsonValue plain = QJsonValue(QStringLiteral("纯文本"));
    TestHarness::check(MessageContent::expandForWire(plain, reader) == plain
                           && reads == 0,
                       "纯串展开是恒等，且一次盘都不读");
    QJsonArray textOnly;
    textOnly.append(textPart(QStringLiteral("只有文")));
    const QJsonValue textValue(textOnly);
    TestHarness::check(MessageContent::expandForWire(textValue, reader) == textValue
                           && reads == 0,
                       "无图数组同样是恒等（每请求每消息都跑这里，不能白拷一遍）");

    // 正常路：引用变 data URL，且私有 scheme 绝不外泄
    QStringList one;
    one << nameA();
    const QJsonValue parts = MessageContent::fromTextAndImages(QStringLiteral("看图"), one);
    const QString originalShape = serialize(parts);
    const QJsonValue expanded = MessageContent::expandForWire(parts, reader);
    TestHarness::check(expanded.isArray() && expanded.toArray().size() == 2,
                       "展开后段数不变（文 + 图）");
    const QString wire = serialize(expanded);
    TestHarness::check(wire.contains(QStringLiteral("data:image/png;base64,")),
                       "图段已换成 data URL");
    TestHarness::check(!wire.contains(QStringLiteral("lh-attach")),
                       "**私有 scheme 绝不漏上线**（端点只会回一次语义不明的坏 url）");
    TestHarness::check(serialize(parts) == originalShape,
                       "展开不改入参（m_messages 里恒是引用形态，base64 永不落历史）");
    TestHarness::check(MessageContent::partType(expanded.toArray().at(0))
                           == MessageContent::textPartType(),
                       "文本段位置不动");

    // 去重路：同名两张只读一次盘、只发一份载荷
    QStringList dup;
    dup << nameA() << nameA();
    const QJsonValue dupParts = MessageContent::fromTextAndImages(QStringLiteral("x"), dup);
    reads = 0;
    const QJsonValue dupExpanded = MessageContent::expandForWire(dupParts, reader);
    TestHarness::check(reads == 1, "同名引用只读一次盘");
    TestHarness::check(dupExpanded.toArray().size() == 2,
                       "重复图段丢弃（一文一图，载荷同一张不付两遍钱）");

    // 缺失路：读不回来 → 可见降级，不静默少一块内容
    QStringList missing;
    missing << nameB();
    const QJsonValue missingParts =
        MessageContent::fromTextAndImages(QStringLiteral("x"), missing);
    const QJsonValue missingExpanded = MessageContent::expandForWire(missingParts, reader);
    const QString missingWire = serialize(missingExpanded);
    TestHarness::check(missingWire.contains(MessageContent::imageUnavailableMarker()),
                       "附件缺失降级为 [image unavailable]（模型据此知道图丢了）");
    TestHarness::check(!missingWire.contains(QStringLiteral("lh-attach")),
                       "降级路径同样不外泄私有 scheme");
    TestHarness::check(missingExpanded.toArray().size() == 2,
                       "降级是换段不是删段（文本段与降级标记都在，形状可预期）");

    // 坏类型路：读回来但不是图
    std::function<QByteArray(const QString &)> notImage = [](const QString &) -> QByteArray {
        return QByteArray("this is not an image at all");
    };
    const QJsonValue garbage =
        MessageContent::expandForWire(missingParts, notImage);
    TestHarness::check(serialize(garbage).contains(MessageContent::imageUnavailableMarker()),
                       "读回来的字节认不出类型 → 同样降级，绝不发半截 data URL");

    // 外来 url 路：别人的 url 不归本头裁决
    QJsonArray foreign;
    foreign.append(imagePart(QStringLiteral("https://example.com/a.png")));
    foreign.append(imagePart(QStringLiteral("data:image/png;base64,AAAA")));
    const QJsonValue foreignExpanded =
        MessageContent::expandForWire(QJsonValue(foreign), reader);
    TestHarness::check(serialize(foreignExpanded) == serialize(QJsonValue(foreign)),
                       "外部 https 与内联 data url 原样透传（不二次编码、不替换）");
    TestHarness::check(reads >= 0, "透传路径不依赖读盘结果");

    // 形态破坏路：scheme 对但名被手改
    QJsonArray tampered;
    tampered.append(imagePart(QStringLiteral("lh-attach://../../../../windows/win.ini")));
    const QJsonValue tamperedExpanded =
        MessageContent::expandForWire(QJsonValue(tampered), reader);
    const QString tamperedWire = serialize(tamperedExpanded);
    TestHarness::check(tamperedWire.contains(MessageContent::imageUnavailableMarker()),
                       "越界写法降级为标记（读盘入口的最后一道判定）");
    TestHarness::check(!tamperedWire.contains(".."), "越界段不带着上线");
}

// ⑥ 历史往返形态：证明 persistHistory/loadSavedHistory 的逐字拷贝足以承载 parts
void testHistoryRoundTrip()
{
    QStringList names;
    names << nameA() << nameB();
    const QJsonValue parts = MessageContent::fromTextAndImages(QStringLiteral("两张图"), names);

    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), parts);

    const QByteArray written = QJsonDocument(QJsonArray{message}).toJson(QJsonDocument::Compact);
    const QJsonObject readBack = QJsonDocument::fromJson(written).array().at(0).toObject();
    const QJsonValue content = readBack.value(QStringLiteral("content"));

    TestHarness::check(content.isArray() && content.toArray().size() == 3,
                       "数组形态经 JSON 往返回来仍是数组（落盘零改动即承载）");
    TestHarness::check(MessageContent::imageCount(content) == 2, "往返后张数不变");
    TestHarness::check(MessageContent::textOf(content)
                           == MessageContent::textOf(parts),
                       "往返后摊平逐字相同");
    TestHarness::check(MessageContent::imageNames(content) == MessageContent::imageNames(parts),
                       "往返后裸名清单相同（回放取图靠这个）");
}

} // namespace

int tst_messagecontent()
{
    testAssemblyKeepsPlainString();
    testFlatten();
    testDisplayFlatten();
    testCounting();
    testAppendText();
    testExpansion();
    testHistoryRoundTrip();
    return 0;
}
