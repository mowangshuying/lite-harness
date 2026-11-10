// tst_memoryrules.cpp —— 记忆子系统纯判据直测（src/MemoryRules.h，header-only 零 CMake 改动
// 即入 ctest，仿 tst_activityledger / tst_heatmapgrid 先例）。
//
// 抽头动机：这些函数此前全在 `MemoryManager.cpp`，而该 TU include QOpenAi.h（Qt6::Network），
// 按「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest —— 于是**整套记忆判据零直测**。
// 它们恰是「算得出但算错」的高危面，错了不报错、不留日志，只让记忆静默变少或变脏：
//   · `extractJsonArray` 是手写括号配对状态机，`escaped`/`depth` 错一格就返回空数组，
//     LLM 已正确返回的条目被整批丢弃，用户只看到「这次没沉淀出新记忆」；
//   · `memorySlug` 错则两个不同 name 撞同一文件名，后写静默覆盖前一条（记忆丢失）；
//   · `normalizedMemoryText` 错则去重键失效，重复记忆无限增长；
//   · `shouldStoreMemory` 的 16 条临时标记与三重去重任一漂，门槛就形同虚设；
//   · `parseFrontmatter` 与 `memoryDocument` 是一对写/读，任一侧漂则整条记忆读回为空。
//
// 分组：全部零夹具恒跑（纯字符串/JSON 算术，不碰磁盘、不碰网络、不读挂钟）。

#include "TestHarness.h"

#include "MemoryRules.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

MemoryRules::ExistingMemory existing(const QString &name, const QString &description,
                                     const QString &body)
{
    MemoryRules::ExistingMemory m;
    m.name = name;
    m.description = description;
    m.body = body;
    return m;
}

QJsonObject candidate(const QString &scope = QStringLiteral("persistent"),
                      const QString &type = QStringLiteral("user"),
                      const QString &name = QStringLiteral("some name"),
                      const QString &description = QStringLiteral("some description"),
                      const QString &body = QStringLiteral("some body"))
{
    QJsonObject o;
    o[QStringLiteral("scope")] = scope;
    o[QStringLiteral("type")] = type;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("description")] = description;
    o[QStringLiteral("body")] = body;
    return o;
}

// ---- 组 1：parseFrontmatter 回落路径（失败时 body 必须是全文，否则记忆正文凭空消失）----

void testParseFrontmatter()
{
    QJsonObject meta;
    QString body;

    // 正常路径
    meta = QJsonObject();
    body.clear();
    const QString ok = QStringLiteral("---\nname: Foo\ntype: user\n---\nbody text\n");
    TestHarness::check(MemoryRules::parseFrontmatter(ok, &meta, &body), "frontmatter: 合法块解析成功");
    TestHarness::check(meta.value(QStringLiteral("name")).toString() == QStringLiteral("Foo"),
                       "frontmatter: 键值对读回");
    TestHarness::check(body == QStringLiteral("body text\n"), "frontmatter: 正文去掉前导空白");

    // 无 "---\n" 前缀 → 回落，body 必须是 trim 后的全文（不是空串）
    meta = QJsonObject();
    body.clear();
    TestHarness::check(!MemoryRules::parseFrontmatter(QStringLiteral("plain text"), &meta, &body),
                       "frontmatter: 无前缀判失败");
    TestHarness::check(body == QStringLiteral("plain text"), "frontmatter: 失败回落保留全文");
    TestHarness::check(meta.isEmpty(), "frontmatter: 失败回落 metadata 为空对象");

    // 缺闭合 "---"
    meta = QJsonObject();
    body.clear();
    TestHarness::check(!MemoryRules::parseFrontmatter(QStringLiteral("---\nname: a\n"), &meta, &body),
                       "frontmatter: 缺闭合判失败");

    // 缩进行（PyYAML 会报嵌套错 → 整体回落）
    TestHarness::check(!MemoryRules::parseFrontmatter(
                           QStringLiteral("---\n  name: a\n---\nx"), &meta, &body),
                       "frontmatter: 缩进行回落");
    // 缺冒号行
    TestHarness::check(!MemoryRules::parseFrontmatter(
                           QStringLiteral("---\nplain\n---\nx"), &meta, &body),
                       "frontmatter: 缺冒号行回落");

    // 双引号标量按 JSON 转义还原（源文本里是反斜杠+n 两个字符）
    meta = QJsonObject();
    body.clear();
    TestHarness::check(MemoryRules::parseFrontmatter(
                           QStringLiteral("---\nname: \"a\\nb\"\n---\nx"), &meta, &body),
                       "frontmatter: 双引号标量解析成功");
    TestHarness::check(meta.value(QStringLiteral("name")).toString()
                           == QStringLiteral("a\nb"),
                       "frontmatter: 双引号 \\n 还原成真换行");

    // 非法转义：实测 Qt **不**报错（与源码注释「≈ PyYAML 报错回落」不符）。
    // `["a\q"]` 被宽容解析成数组、值里的反斜杠被静默吞掉 → 得到 "aq"。
    // 这条差异登记为已实测行为：写侧 yamlScalar 只会产出合法 JSON 转义，故正常往返不受影响；
    // 但**手改的记忆文件**里若有非法转义，读回的是被改形的文本而非整体回落，排查时别按注释预期。
    meta = QJsonObject();
    body.clear();
    TestHarness::check(MemoryRules::parseFrontmatter(
                           QStringLiteral("---\nname: \"a\\q\"\n---\nx"), &meta, &body),
                       "frontmatter: 非法转义 Qt 宽容接受（实测，非注释所称回落）");
    TestHarness::check(meta.value(QStringLiteral("name")).toString() == QStringLiteral("aq"),
                       "frontmatter: 非法转义的反斜杠被静默丢弃");

    // 单引号标量：'' 是引号转义，反斜杠保持字面
    meta = QJsonObject();
    body.clear();
    TestHarness::check(MemoryRules::parseFrontmatter(
                           QStringLiteral("---\nname: 'it''s'\n---\nx"), &meta, &body),
                       "frontmatter: 单引号标量解析成功");
    TestHarness::check(meta.value(QStringLiteral("name")).toString() == QStringLiteral("it's"),
                       "frontmatter: 单引号 '' 还原成撇号");
}

// ---- 组 2：memorySlug（撞名=静默覆盖，必须钉死）----

void testMemorySlug()
{
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("Hello World"))
                           == QStringLiteral("hello-world"),
                       "slug: 空格折叠成连字符并小写");
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("  Trim Me  "))
                           == QStringLiteral("trim-me"),
                       "slug: 首尾连字符被裁掉");
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("a_b")) == QStringLiteral("a_b"),
                       "slug: 下划线属 \\w 不被替换");
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("!!!")) == QStringLiteral("memory"),
                       "slug: 全非词字符回落 memory 而非空文件名");
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("")) == QStringLiteral("memory"),
                       "slug: 空串回落 memory");
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("用户 偏好"))
                           == QStringLiteral("用户-偏好"),
                       "slug: CJK 走 unicode \\w 不被整体打码");
    // 撞名检查：不同 name 不得落到同一 slug
    TestHarness::check(MemoryRules::memorySlug(QStringLiteral("Foo Bar"))
                           != MemoryRules::memorySlug(QStringLiteral("Foo Baz")),
                       "slug: 相邻 name 不撞车");
}

// ---- 组 3：normalizedMemoryText / collapseWhitespace（去重键，错则重复记忆无限增长）----

void testNormalizedMemoryText()
{
    TestHarness::check(MemoryRules::normalizedMemoryText(QStringLiteral("  Foo   BAR \n baz "))
                           == QStringLiteral("foo bar baz"),
                       "normalize: 大小写归一 + 任意空白折叠为单空格");
    TestHarness::check(MemoryRules::normalizedMemoryText(QStringLiteral("a\t\tb"))
                           == QStringLiteral("a b"),
                       "normalize: 制表符同样折叠");
    TestHarness::check(MemoryRules::collapseWhitespace(QStringLiteral("  a \u3000 b "))
                           == QStringLiteral("a b"),
                       "collapse: unicode 空白（全角空格）也折叠");
    TestHarness::check(MemoryRules::collapseWhitespace(QStringLiteral("")) == QString(),
                       "collapse: 空串得空串");
}

// ---- 组 4：messageText（工具结果不得混进记忆语料）----

void testMessageText()
{
    QJsonObject tool;
    tool[QStringLiteral("role")] = QStringLiteral("tool");
    tool[QStringLiteral("content")] = QStringLiteral("raw tool payload");
    TestHarness::check(MemoryRules::messageText(tool).isEmpty(),
                       "messageText: role==tool 返回空，工具输出不进记忆语料");

    QJsonObject user;
    user[QStringLiteral("role")] = QStringLiteral("user");
    user[QStringLiteral("content")] = QStringLiteral("hi");
    TestHarness::check(MemoryRules::messageText(user) == QStringLiteral("hi"),
                       "messageText: 普通角色取 content");

    QJsonObject none;
    none[QStringLiteral("role")] = QStringLiteral("assistant");
    TestHarness::check(MemoryRules::messageText(none).isEmpty(),
                       "messageText: 缺 content 返回空而非崩溃");
}

// ---- 组 5：extractJsonArray（括号状态机，一错就整批丢记忆）----

void testExtractJsonArray()
{
    // 前置散文 + 数组
    const QJsonArray a = MemoryRules::extractJsonArray(
        QStringLiteral("here you go: [\"one\", \"two\"] thanks"));
    TestHarness::check(a.size() == 2, "extract: 跳过前置散文取到 2 项");
    TestHarness::check(a.at(0).toString() == QStringLiteral("one"), "extract: 首项正确");

    // 字符串内含括号：状态机必须忽略串内的 [ ]
    const QJsonArray b = MemoryRules::extractJsonArray(
        QStringLiteral("[\"a]b\", \"c[\"]"));
    TestHarness::check(b.size() == 2, "extract: 串内右括号不提前闭合");
    TestHarness::check(b.at(0).toString() == QStringLiteral("a]b"), "extract: 串内 ] 原样保留");
    TestHarness::check(b.at(1).toString() == QStringLiteral("c["), "extract: 串内 [ 不计深度");

    // 转义引号：串内的 \" 不得结束字符串，否则后续括号计数全错
    const QJsonArray c = MemoryRules::extractJsonArray(
        QStringLiteral("[\"a\\\"b]\", \"x\"]"));
    TestHarness::check(c.size() == 2, "extract: 转义引号不提前出串");
    TestHarness::check(c.at(0).toString() == QStringLiteral("a\"b]"),
                       "extract: 转义引号项内容完整");

    // 嵌套数组
    const QJsonArray d = MemoryRules::extractJsonArray(QStringLiteral("[1, [2, 3], 4]"));
    TestHarness::check(d.size() == 3, "extract: 嵌套数组按深度配对整体取出");
    TestHarness::check(d.at(1).toArray().size() == 2, "extract: 内层数组保留");

    // 第一个候选不可解析 → 继续试下一个（不是直接放弃）
    const QJsonArray e = MemoryRules::extractJsonArray(QStringLiteral("[oops] then [3]"));
    TestHarness::check(e.size() == 1, "extract: 首个候选非法时回退到下一候选");
    TestHarness::check(e.at(0).toInt() == 3, "extract: 取到的是可解析候选");

    // 未闭合 / 无数组 → 空数组而非崩溃
    TestHarness::check(MemoryRules::extractJsonArray(QStringLiteral("[1, 2")).isEmpty(),
                       "extract: 未闭合返回空数组");
    TestHarness::check(MemoryRules::extractJsonArray(QStringLiteral("no array here")).isEmpty(),
                       "extract: 无左括号返回空数组");
    TestHarness::check(MemoryRules::extractJsonArray(QString()).isEmpty(),
                       "extract: 空串返回空数组");
}

// ---- 组 6：validateMemoryRecord（必填门槛 + scope 白名单）----

void testValidateMemoryRecord()
{
    QJsonObject out;
    QJsonObject rec;
    rec[QStringLiteral("name")] = QStringLiteral("N");
    rec[QStringLiteral("type")] = QStringLiteral("user");
    rec[QStringLiteral("description")] = QStringLiteral("D");
    rec[QStringLiteral("body")] = QStringLiteral("B");

    out = QJsonObject();
    TestHarness::check(MemoryRules::validateMemoryRecord(rec, false, &out),
                       "validate: 四要素齐全即通过");
    TestHarness::check(out.value(QStringLiteral("name")).toString() == QStringLiteral("N"),
                       "validate: 出参写回规范化记录");
    TestHarness::check(!out.contains(QStringLiteral("scope")),
                       "validate: 缺 scope 时不写空 scope 键");

    // requireScope 分支
    TestHarness::check(!MemoryRules::validateMemoryRecord(rec, true, &out),
                       "validate: requireScope 下缺 scope 判失败");
    rec[QStringLiteral("scope")] = QStringLiteral("persistent");
    TestHarness::check(MemoryRules::validateMemoryRecord(rec, true, &out),
                       "validate: persistent 通过");
    TestHarness::check(out.value(QStringLiteral("scope")).toString() == QStringLiteral("persistent"),
                       "validate: scope 透传");
    rec[QStringLiteral("scope")] = QStringLiteral("current_task");
    TestHarness::check(MemoryRules::validateMemoryRecord(rec, true, &out),
                       "validate: current_task 通过");
    rec[QStringLiteral("scope")] = QStringLiteral("whatever");
    TestHarness::check(!MemoryRules::validateMemoryRecord(rec, true, &out),
                       "validate: 非法 scope 判失败");

    // 类型白名单（抽头后白名单本身也在纯头里，必须可断言）
    TestHarness::check(MemoryRules::memoryTypes().size() == 4, "memoryTypes: 四类白名单");
    TestHarness::check(MemoryRules::temporaryMemoryMarkers().size() == 16,
                       "markers: 16 条临时标记");
    rec[QStringLiteral("scope")] = QStringLiteral("persistent");
    rec[QStringLiteral("type")] = QStringLiteral("bogus_type");
    TestHarness::check(!MemoryRules::validateMemoryRecord(rec, true, &out),
                       "validate: 未知 type 判失败");

    // 任一必填为空都不得通过（空 name 会写出 slug=memory 的兜底文件，必须挡在门外）
    for (const QString &key : {QStringLiteral("name"), QStringLiteral("description"),
                               QStringLiteral("body")}) {
        QJsonObject weak = rec;
        weak[QStringLiteral("type")] = QStringLiteral("user");
        weak[key] = QStringLiteral("   ");
        TestHarness::check(!MemoryRules::validateMemoryRecord(weak, true, &out),
                           "validate: 必填字段空白视为缺失");
    }
}

// ---- 组 7：shouldStoreMemory（持久门槛 + 临时标记 + 三重去重）----

void testShouldStoreMemory()
{
    const QVector<MemoryRules::ExistingMemory> none;

    TestHarness::check(!MemoryRules::shouldStoreMemory(candidate(QStringLiteral("current_task")), none),
                       "store: 非 persistent 一律不落盘");
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("bogus")), none),
                       "store: 未知 type 被拒");
    TestHarness::check(MemoryRules::shouldStoreMemory(candidate(), none),
                       "store: 全新合法候选放行");

    // 空字段
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("  ")),
                           none),
                       "store: 空 name 被拒");

    // 临时标记（英文 + 中文各取一条，标记表一漂门槛即失效）
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("note"), QStringLiteral("about this session"),
                                     QStringLiteral("b")),
                           none),
                       "store: 含 this session 标记被拒");
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("本次会话的偏好"), QStringLiteral("d"),
                                     QStringLiteral("b")),
                           none),
                       "store: 含中文标记被拒（归一后仍命中）");

    // 三重去重：slug / 描述 / 正文，任一命中即拒
    const QVector<MemoryRules::ExistingMemory> has{
        existing(QStringLiteral("Foo Bar"), QStringLiteral("Hello There"),
                 QStringLiteral("body text"))};
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("foo  bar"), QStringLiteral("d2"),
                                     QStringLiteral("b2")),
                           has),
                       "store: slug 相同判重复");
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("Other"), QStringLiteral("hello   THERE"),
                                     QStringLiteral("b2")),
                           has),
                       "store: 描述归一后相同判重复");
    TestHarness::check(!MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("Other"), QStringLiteral("d2"),
                                     QStringLiteral("BODY   TEXT")),
                           has),
                       "store: 正文归一后相同判重复");
    TestHarness::check(MemoryRules::shouldStoreMemory(
                           candidate(QStringLiteral("persistent"), QStringLiteral("user"),
                                     QStringLiteral("Totally"), QStringLiteral("d2"),
                                     QStringLiteral("b2")),
                           has),
                       "store: 三者皆不同则放行");
}

// ---- 组 8：memoryDocument ↔ parseFrontmatter 写读往返 ----

void testMemoryDocumentRoundtrip()
{
    const QString doc = MemoryRules::memoryDocument(QStringLiteral("Foo Bar"),
                                                    QStringLiteral("user"),
                                                    QStringLiteral("keeps key: order"),
                                                    QStringLiteral("the body"));
    TestHarness::check(doc.startsWith(QStringLiteral("---\n")), "document: 以围栏开头");
    // 键序固定 name/description/type（lcc safe_dump sort_keys=False）
    const int iName = doc.indexOf(QStringLiteral("name:"));
    const int iDesc = doc.indexOf(QStringLiteral("description:"));
    const int iType = doc.indexOf(QStringLiteral("type:"));
    TestHarness::check(iName >= 0 && iName < iDesc && iDesc < iType,
                       "document: 键序为 name/description/type");

    QJsonObject meta;
    QString body;
    TestHarness::check(MemoryRules::parseFrontmatter(doc, &meta, &body),
                       "document: 写出的文档能读回");
    TestHarness::check(meta.value(QStringLiteral("name")).toString() == QStringLiteral("Foo Bar"),
                       "document: name 往返一致");
    TestHarness::check(meta.value(QStringLiteral("description")).toString()
                           == QStringLiteral("keeps key: order"),
                       "document: 含冒号的描述经引号转义往返一致");
    TestHarness::check(meta.value(QStringLiteral("type")).toString() == QStringLiteral("user"),
                       "document: type 往返一致");
    TestHarness::check(body.trimmed() == QStringLiteral("the body"), "document: 正文往返一致");

    // 换行正文：写出必须带引号转义，读回仍是真换行
    const QString multiline = MemoryRules::memoryDocument(QStringLiteral("n"),
                                                          QStringLiteral("feedback"),
                                                          QStringLiteral("d"),
                                                          QStringLiteral("line1\nline2"));
    TestHarness::check(MemoryRules::parseFrontmatter(multiline, &meta, &body),
                       "document: 多行正文文档可读回");
    TestHarness::check(body.trimmed() == QStringLiteral("line1\nline2"),
                       "document: 正文换行未被压平");
}

} // namespace

int tst_memoryrules()
{
    const int before = TestHarness::failCount();

    testParseFrontmatter();
    testMemorySlug();
    testNormalizedMemoryText();
    testMessageText();
    testExtractJsonArray();
    testValidateMemoryRecord();
    testShouldStoreMemory();
    testMemoryDocumentRoundtrip();

    return TestHarness::failCount() - before;
}
