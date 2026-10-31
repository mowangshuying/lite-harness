// 套件：会话索引并入 config.json（键 app/sessions）后的读写与迁移判据。
//
// 这个套件存在的理由，是本次改动把索引从「独立文件 + 裸 QSaveFile 整表重写」换成了
// 「配置文件里的一个键 + QSettings 读写」。换轨带来的不是等价替换，而是三格旧套件覆盖不到的
// 全新风险：
//   ① 同一个文件现在有三个写侧 owner（宿主键 / FluentUI 键 / 会话索引）。裸文件整写会把别人
//      刚落的键一起抹掉，且**不报错**——所以必须直读盘上 JSON 断言「别人的键还在」。
//      （tst_configmerge 证的是两个 QSettings 实例不互覆；本套件补的是「索引整表回写」这个
//       最大颗粒的写侧：一次动几十条条目，最容易把合并语义写坏的就是这一格。）
//   ② 索引的形态判据只认 QVariantList。json 后端把数组落成 QVariantList（不是 QStringList，
//      同一条坑见 tst_configmerge 组 C），而用户手改 config.json 把它写成对象是可预期的失误：
//      此时必须降级成空索引，不崩、不猜、不写回——history.json 一个字节都没动，改回数组即全回来。
//   ③ 迁移链的四条分支（无旧文件 / 目标已是列表 / 旧文件不是数组 / 写成功且回读一致才删旧文件）。
//      其中「目标已有列表就绝不回写」最阴：旧文件此时是残留而非数据源，回写等于把用户迁移后
//      新增的会话覆盖掉，且只在第二次启动之后才暴露。
//
// 与相邻套件的分工：tst_settingscache 测 Reader 的缓存/失效判据（与业务无关），tst_configmerge
// 测「两侧 owner 真在动同一个文件」与列表形态归一化，tst_litepaths 测路径锚点（含旧 index.json
// 的拼法——迁移要靠它找旧文件）；**只有本套件**测 SessionStore 自己的条目语义与迁移分支。
//
// 本套件的写法纪律：CHECK 宏把入参**求值两次**（一次给 check()、一次给失败文案的三元式），
// 所以任何有副作用的调用（removeEntry / migrateLegacyIndex / setValue / writeFileText）一律先
// 落到具名变量再断言——否则同一段写会跑两遍，第二遍的结果已不是被测那一次，红得莫名其妙。
//
// 刻意不调 AppSettings::reader()：那对口径绑死 applicationDirPath()（reader() 里 Q_ASSERT 拦的
// 就是这条），测试会把配置写进测试 exe 目录。落点一律经 Reader 构造注入，键名口径复用生产单点
// AppSettings::scopedKey()——被测的字符串规则与线上是同一份代码（同 tst_configmerge 的取舍）。
#include "AppSettings.h" // scopedKey()/fileFormat()/keyCaseSensitivity()：宿主键名与口径单点
#include "LitePaths.h"
#include "ScopedTempRoot.h"
#include "SessionStore.h"
#include "SettingsCache.h"
#include "TestHarness.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>

#define SUITE "sessionstore"
#define TOSTR2(x) #x
#define TOSTR(x) TOSTR2(x)
#define CHECK(cond)                                                                                \
    TestHarness::check((cond), (cond) ? "" : SUITE ": " __FILE__ ":" TOSTR(__LINE__))

namespace {

// 与生产完全配对的一支 Reader（格式 + 折叠口径成对传入，见 SettingsCache.h 不变量④）
SettingsCache::Reader makeReader(const QString &configPath)
{
    return SettingsCache::Reader(configPath, AppSettings::fileFormat(),
                                 AppSettings::keyCaseSensitivity());
}

QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QJsonObject jsonAt(const QString &path)
{
    const QJsonDocument doc = QJsonDocument::fromJson(readAll(path).toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// 盘上 app/sessions 的**原始形态**（不信内存，直接读文件）
QJsonValue diskSessionsValue(const QString &configPath)
{
    return jsonAt(configPath).value(QStringLiteral("app")).toObject()
        .value(SessionStore::sessionsKey());
}

QJsonArray diskSessions(const QString &configPath)
{
    return diskSessionsValue(configPath).toArray();
}

bool writeFileText(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

QVariantMap entryOf(const QVariantList &list, int index)
{
    return list.at(index).toMap();
}

// ---- 组 A：键名口径（零 IO，任何环境恒跑）----
// 裸键必须是 sessions（用户裁定的字段名），且必须被 scopedKey 收进 app/ 组——
// 漏到顶层就是盘上同时存在 {"sessions":[…]} 与 {"app":{…}} 两份真相的开始。
int groupKeyShape()
{
    int failed = 0;
    CHECK(SessionStore::sessionsKey() == QStringLiteral("sessions"));
    CHECK(SessionStore::scopedSessionsKey() == QStringLiteral("app/sessions"));
    CHECK(SessionStore::scopedSessionsKey()
          == AppSettings::keyGroup() + QLatin1Char('/') + SessionStore::sessionsKey());
    CHECK(!SessionStore::sessionsKey().contains(QLatin1Char('/')));
    return failed;
}

// ---- 组 B：读写换轨后的行为面（夹具门控，真落盘）----
int groupReadWrite(const QString &dir)
{
    int failed = 0;
    const QString configPath = LitePaths::configFilePathFor(dir);
    const SettingsCache::Reader reader = makeReader(configPath);

    // ① 空配置：读索引得空表，而不是报错或凭空造一个键
    CHECK(SessionStore::loadIndex(reader).isEmpty());

    // ② 首条登记：追加一项，字段齐全，createdMs 是毫秒整数
    const QString title = QStringLiteral("中文标题·会话一");
    SessionStore::upsertEntry(reader, QStringLiteral("aa11bb22"), title,
                              QStringLiteral("qwen-max"), QStringLiteral("D:/work/one"));
    QVariantList entries = SessionStore::loadIndex(reader);
    CHECK(entries.size() == 1);
    if (entries.size() != 1)
        return failed;
    QVariantMap first = entryOf(entries, 0);
    CHECK(first.value(QStringLiteral("dataId")).toString() == QStringLiteral("aa11bb22"));
    CHECK(first.value(QStringLiteral("title")).toString() == title); // 中文逐字往返
    CHECK(first.value(QStringLiteral("model")).toString() == QStringLiteral("qwen-max"));
    CHECK(first.value(QStringLiteral("workDir")).toString() == QStringLiteral("D:/work/one"));
    const qint64 created = first.value(QStringLiteral("createdMs")).toLongLong();
    CHECK(created > 0);
    CHECK(qAbs(created - QDateTime::currentMSecsSinceEpoch()) < 60LL * 1000 * 1000);
    // lastActiveMs 已随本次合并删除：新写的条目里**不得**再有这个键（留着等于留着续活链接口）
    CHECK(!first.contains(QStringLiteral("lastActiveMs")));

    // ③ 盘上真相：app/sessions 是**数组**、元素是对象（不是被 flatten 成 app/sessions/0/… 的怪形态）
    CHECK(diskSessionsValue(configPath).isArray());
    const QJsonArray disk = diskSessions(configPath);
    CHECK(disk.size() == 1);
    if (!disk.isEmpty())
    {
        CHECK(disk.at(0).isObject());
        CHECK(disk.at(0).toObject().value(QStringLiteral("title")).toString() == title);
        CHECK(disk.at(0).toObject().value(QStringLiteral("createdMs")).toDouble()
              == static_cast<double>(created));
    }
    // 顶层不得出现裸 sessions（scopedKey 单点被绕过就会在这里露形）
    CHECK(!jsonAt(configPath).contains(QStringLiteral("sessions")));

    // ④ 同 dataId 再登记 = 就地更新，不追加；空 model/workDir 不覆盖既有值
    //    （重命名走的就是这一格：只传标题，模型与工作目录必须原样留着）
    SessionStore::upsertEntry(reader, QStringLiteral("aa11bb22"), QStringLiteral("改名后的标题"),
                              QString(), QString());
    entries = SessionStore::loadIndex(reader);
    CHECK(entries.size() == 1);
    if (entries.size() != 1)
        return failed;
    first = entryOf(entries, 0);
    CHECK(first.value(QStringLiteral("title")).toString() == QStringLiteral("改名后的标题"));
    CHECK(first.value(QStringLiteral("model")).toString() == QStringLiteral("qwen-max"));
    CHECK(first.value(QStringLiteral("workDir")).toString() == QStringLiteral("D:/work/one"));
    CHECK(first.value(QStringLiteral("createdMs")).toLongLong() == created); // 创建时间不被刷新

    // ⑤ 第二条：追加而非替换；两条各归各的 dataId
    SessionStore::upsertEntry(reader, QStringLiteral("cc33dd44"), QStringLiteral("会话二"),
                              QStringLiteral("qwen-turbo"), QStringLiteral("D:/work/two"));
    entries = SessionStore::loadIndex(reader);
    CHECK(entries.size() == 2);
    if (entries.size() == 2)
    {
        CHECK(entryOf(entries, 0).value(QStringLiteral("dataId")).toString()
              == QStringLiteral("aa11bb22"));
        CHECK(entryOf(entries, 1).value(QStringLiteral("dataId")).toString()
              == QStringLiteral("cc33dd44"));
        CHECK(entryOf(entries, 1).value(QStringLiteral("model")).toString()
              == QStringLiteral("qwen-turbo"));
    }

    // ⑥ 空 dataId 卫兵：不登记、不写盘（未注入 ID 的会话不该出现在索引里）
    SessionStore::upsertEntry(reader, QString(), QStringLiteral("幽灵"), QString(), QString());
    CHECK(SessionStore::loadIndex(reader).size() == 2);

    // ⑦ 删除：命中返 true 并移出，未命中返 false 且不写盘（副作用先落变量，见文件头纪律）
    const bool removedHit = SessionStore::removeEntry(reader, QStringLiteral("cc33dd44"));
    CHECK(removedHit);
    entries = SessionStore::loadIndex(reader);
    CHECK(entries.size() == 1);
    if (entries.size() == 1)
        CHECK(entryOf(entries, 0).value(QStringLiteral("dataId")).toString()
              == QStringLiteral("aa11bb22"));
    const bool removedMiss = SessionStore::removeEntry(reader, QStringLiteral("no-such-id"));
    CHECK(!removedMiss);
    CHECK(SessionStore::loadIndex(reader).size() == 1);
    CHECK(!SessionStore::removeEntry(reader, QString()));
    CHECK(diskSessions(configPath).size() == 1);

    // ⑧ 核心红线①：索引整表回写**不得**抹掉别人的键。
    //    宿主键与 FluentUI 键各写一把，再连写两次索引（一次追加一次改名），然后直读盘：
    //    三把键都必须在。裸 QSaveFile 重写在这里必红。
    const bool tokenSaved = reader.setValue(AppSettings::scopedKey(QStringLiteral("apiToken")),
                                            QStringLiteral("tok-keep-me"));
    CHECK(tokenSaved);
    QSettings fluent(configPath, AppSettings::fileFormat());
    fluent.setValue(QStringLiteral("config/navWidth"), 320);
    fluent.sync();
    SessionStore::upsertEntry(reader, QStringLiteral("ee55ff66"), QStringLiteral("会话三"),
                              QStringLiteral("m"), QStringLiteral("D:/w"));
    SessionStore::upsertEntry(reader, QStringLiteral("aa11bb22"), QStringLiteral("再改一次名"),
                              QString(), QString());
    const QJsonObject root = jsonAt(configPath);
    const QJsonObject app = root.value(QStringLiteral("app")).toObject();
    CHECK(app.value(QStringLiteral("apiToken")).toString() == QStringLiteral("tok-keep-me"));
    CHECK(app.value(QStringLiteral("sessions")).toArray().size() == 2);
    CHECK(root.value(QStringLiteral("config")).toObject().value(QStringLiteral("navWidth")).toInt()
          == 320);
    // FluentUI 侧也看得见索引写后的自己那份（跨实例可见 = 盘上只有一份真相）
    const SettingsCache::Reader other = makeReader(configPath);
    CHECK(other.value(QStringLiteral("config/navWidth")).toInt() == 320);
    CHECK(SessionStore::loadIndex(other).size() == 2);

    // ⑨ 空表也要落回「列表形态」：删光会话后键必须**仍是数组**（不是消失、不是 @Invalid）。
    //    这不是洁癖：迁移判据「目标已是列表 = 早已迁过」正靠这个形态成立；若空表在盘上退化成
    //    「键不存在」，残留的旧 index.json 就会把用户删光的会话又搬回来。
    const bool rm1 = SessionStore::removeEntry(reader, QStringLiteral("ee55ff66"));
    const bool rm2 = SessionStore::removeEntry(reader, QStringLiteral("aa11bb22"));
    CHECK(rm1);
    CHECK(rm2);
    CHECK(SessionStore::loadIndex(reader).isEmpty());
    CHECK(diskSessionsValue(configPath).isArray()); // 形态本身，而非「取出来正好是空」
    CHECK(diskSessions(configPath).isEmpty());

    // ⑩ 核心红线②：坏形态降级。用户手改 config.json 把 sessions 写成对象（`{"dataId":…}`）
    //    是可预期失误——必须读成空索引，不崩、不猜、不写回。
    const QString badPath = QDir(dir).filePath(QStringLiteral("bad.json"));
    const bool badWritten = writeFileText(
        badPath, "{\"app\":{\"sessions\":{\"dataId\":\"x\",\"title\":\"手改坏了\"}}}");
    CHECK(badWritten);
    const SettingsCache::Reader badReader = makeReader(badPath);
    CHECK(SessionStore::loadIndex(badReader).isEmpty());
    const QVariant stored = badReader.value(SessionStore::scopedSessionsKey());
    CHECK(stored.metaType() != QMetaType::fromType<QVariantList>()); // 取证：它确实不是列表
    // 降级不是破坏：改回数组形态，条目原样回来（history.json 从未被这次读取碰过）
    const QString goodPath = QDir(dir).filePath(QStringLiteral("good.json"));
    const bool goodWritten =
        writeFileText(goodPath, "{\"app\":{\"sessions\":[{\"dataId\":\"x\",\"title\":\"回来了\"}]}}");
    CHECK(goodWritten);
    const SettingsCache::Reader goodReader = makeReader(goodPath);
    const QVariantList restored = SessionStore::loadIndex(goodReader);
    CHECK(restored.size() == 1);
    if (restored.size() == 1)
    {
        CHECK(entryOf(restored, 0).value(QStringLiteral("dataId")).toString() == QStringLiteral("x"));
        CHECK(entryOf(restored, 0).value(QStringLiteral("title")).toString()
              == QStringLiteral("回来了"));
    }
    return failed;
}

// ---- 组 C：旧 index.json 一次性迁移（夹具门控，真落盘）----
int groupMigration(const QString &dir)
{
    int failed = 0;
    const QString configPath = LitePaths::configFilePathFor(dir);
    const QString legacyPath = LitePaths::legacyIndexFilePathFor(dir);

    // ① 无旧文件 → 返回 0、不碰配置（绝大多数启动走这条，零成本）
    {
        const SettingsCache::Reader reader = makeReader(configPath);
        const int rc = SessionStore::migrateLegacyIndex(reader, legacyPath);
        CHECK(rc == 0);
        CHECK(SessionStore::loadIndex(reader).isEmpty());
    }

    // ② 正常迁移：两条旧条目（含 lastActiveMs）→ 搬进 app/sessions、createdMs 保真、
    //    lastActiveMs 不搬、旧文件删除
    {
        const QByteArray legacy =
            "[{\"dataId\":\"a1\",\"title\":\"旧会话一\",\"model\":\"qwen-max\","
            "\"workDir\":\"D:/w1\",\"createdMs\":1700000000000,\"lastActiveMs\":1700000099000},"
            "{\"dataId\":\"a2\",\"title\":\"旧会话二\",\"model\":\"qwen-turbo\","
            "\"workDir\":\"D:/w2\",\"createdMs\":1700000050000,\"lastActiveMs\":1700000088000}]";
        const bool legacyWritten = writeFileText(legacyPath, legacy);
        CHECK(legacyWritten);
        const SettingsCache::Reader reader = makeReader(configPath);
        const int rc = SessionStore::migrateLegacyIndex(reader, legacyPath);
        CHECK(rc == 2);
        CHECK(!QFile::exists(legacyPath)); // 只有写成功且回读一致才删（顺序不可颠倒）
        const QVariantList entries = SessionStore::loadIndex(reader);
        CHECK(entries.size() == 2);
        if (entries.size() == 2)
        {
            CHECK(entryOf(entries, 0).value(QStringLiteral("dataId")).toString()
                  == QStringLiteral("a1"));
            CHECK(entryOf(entries, 0).value(QStringLiteral("title")).toString()
                  == QStringLiteral("旧会话一"));
            CHECK(entryOf(entries, 0).value(QStringLiteral("createdMs")).toLongLong()
                  == 1700000000000LL); // 毫秒时间戳经 json 后端逐位保真
            CHECK(entryOf(entries, 0).value(QStringLiteral("workDir")).toString()
                  == QStringLiteral("D:/w1"));
            CHECK(!entryOf(entries, 0).contains(QStringLiteral("lastActiveMs")));
            CHECK(entryOf(entries, 1).value(QStringLiteral("dataId")).toString()
                  == QStringLiteral("a2"));
        }
        // 盘上形态同样是数组（迁移写的是一个键，不是又一个文件）
        CHECK(diskSessions(configPath).size() == 2);

        // 幂等：旧文件已删，第二次启动返回 0 且索引不变
        const int rc2 = SessionStore::migrateLegacyIndex(reader, legacyPath);
        CHECK(rc2 == 0);
        CHECK(SessionStore::loadIndex(reader).size() == 2);
    }

    // ③ 目标已有列表 = 早已迁移过，旧文件只是残留 → **只删不回写**。
    //    这条是本组核心：回写会把用户迁移后新增的会话覆盖掉，且只在第二次启动后才暴露。
    {
        const SettingsCache::Reader reader = makeReader(configPath);
        CHECK(SessionStore::loadIndex(reader).size() == 2);
        // 模拟「迁移之后用户又新建了一条会话」
        SessionStore::upsertEntry(reader, QStringLiteral("new9"), QStringLiteral("迁移后新建"),
                                  QStringLiteral("m"), QStringLiteral("D:/w"));
        CHECK(SessionStore::loadIndex(reader).size() == 3);
        // 此时旧文件被人工放回（残留形态）
        const bool residueWritten = writeFileText(
            legacyPath, "[{\"dataId\":\"a1\",\"title\":\"旧会话一\",\"createdMs\":1700000000000}]");
        CHECK(residueWritten);
        const int rc = SessionStore::migrateLegacyIndex(reader, legacyPath);
        CHECK(rc == 0);
        CHECK(!QFile::exists(legacyPath)); // 残留被清
        const QVariantList after = SessionStore::loadIndex(reader);
        CHECK(after.size() == 3); // **没被旧表覆盖**
        if (after.size() == 3)
            CHECK(entryOf(after, 2).value(QStringLiteral("dataId")).toString()
                  == QStringLiteral("new9"));

        // 空表也算「已迁移」：用户真的删光了会话时，不得让旧文件把条目又搬回来
        const QString emptyHome = QDir(dir).filePath(QStringLiteral("empty-home"));
        QDir().mkpath(emptyHome);
        const QString emptyConfig = LitePaths::configFilePathFor(emptyHome);
        const QString emptyLegacy = LitePaths::legacyIndexFilePathFor(emptyHome);
        const SettingsCache::Reader emptyReader = makeReader(emptyConfig);
        const bool emptySaved = SessionStore::saveIndex(emptyReader, QVariantList()); // 落一个空数组
        CHECK(emptySaved);
        const bool ghostWritten =
            writeFileText(emptyLegacy, "[{\"dataId\":\"ghost\",\"title\":\"幽灵\",\"createdMs\":1}]");
        CHECK(ghostWritten);
        const int rcEmpty = SessionStore::migrateLegacyIndex(emptyReader, emptyLegacy);
        CHECK(rcEmpty == 0);
        CHECK(SessionStore::loadIndex(emptyReader).isEmpty()); // 幽灵没回来
        CHECK(!QFile::exists(emptyLegacy));
    }

    // ④ 旧文件读不出数组 → 保留原文件 + 返回 -1 + 不写空表（写空表等于删数据）
    {
        const QString badHome = QDir(dir).filePath(QStringLiteral("bad-home"));
        QDir().mkpath(badHome);
        const QString badConfig = LitePaths::configFilePathFor(badHome);
        const QString badLegacy = LitePaths::legacyIndexFilePathFor(badHome);
        const SettingsCache::Reader reader = makeReader(badConfig);

        const bool objWritten = writeFileText(badLegacy, "{\"dataId\":\"x\"}"); // 对象，不是数组
        CHECK(objWritten);
        const int rcObj = SessionStore::migrateLegacyIndex(reader, badLegacy);
        CHECK(rcObj == -1);
        CHECK(QFile::exists(badLegacy)); // 保留待人工处理，下次启动再试
        CHECK(SessionStore::loadIndex(reader).isEmpty());

        const bool junkWritten = writeFileText(badLegacy, "not json at all [");
        CHECK(junkWritten);
        const int rcJunk = SessionStore::migrateLegacyIndex(reader, badLegacy);
        CHECK(rcJunk == -1);
        CHECK(QFile::exists(badLegacy));

        // 空数组是**合法**旧索引（用户删光了会话）：这不是坏形态，必须正常搬完并删旧文件
        const bool emptyArrayWritten = writeFileText(badLegacy, "[]");
        CHECK(emptyArrayWritten);
        const int rcEmptyArray = SessionStore::migrateLegacyIndex(reader, badLegacy);
        CHECK(rcEmptyArray == 0);
        CHECK(!QFile::exists(badLegacy));
    }

    // ⑤ 缺字段的旧条目：createdMs 缺失/为 0 → 补当前时刻（排序键不能是 0，否则恢复顺序随机）
    {
        const QString partialHome = QDir(dir).filePath(QStringLiteral("partial-home"));
        QDir().mkpath(partialHome);
        const QString partialConfig = LitePaths::configFilePathFor(partialHome);
        const QString partialLegacy = LitePaths::legacyIndexFilePathFor(partialHome);
        const SettingsCache::Reader reader = makeReader(partialConfig);
        const bool partialWritten = writeFileText(
            partialLegacy,
            "[{\"dataId\":\"p1\",\"title\":\"没有时间戳\"},{\"dataId\":\"p2\","
            "\"title\":\"零\",\"createdMs\":0}]");
        CHECK(partialWritten);
        const int rc = SessionStore::migrateLegacyIndex(reader, partialLegacy);
        CHECK(rc == 2);
        const QVariantList entries = SessionStore::loadIndex(reader);
        CHECK(entries.size() == 2);
        if (entries.size() == 2)
        {
            CHECK(entryOf(entries, 0).value(QStringLiteral("createdMs")).toLongLong() > 0);
            CHECK(entryOf(entries, 1).value(QStringLiteral("createdMs")).toLongLong() > 0);
            CHECK(entryOf(entries, 0).value(QStringLiteral("model")).toString().isEmpty());
        }

        // 非对象元素混在数组里 → 跳过该元素，不整体失败
        const QString mixedHome = QDir(dir).filePath(QStringLiteral("mixed-home"));
        QDir().mkpath(mixedHome);
        const SettingsCache::Reader mixedReader = makeReader(LitePaths::configFilePathFor(mixedHome));
        const QString mixedLegacy = QDir(mixedHome).filePath(QStringLiteral("mixed.json"));
        const bool mixedWritten = writeFileText(
            mixedLegacy, "[\"字符串不是对象\",{\"dataId\":\"ok\",\"createdMs\":7}]");
        CHECK(mixedWritten);
        const int rcMixed = SessionStore::migrateLegacyIndex(mixedReader, mixedLegacy);
        CHECK(rcMixed == 1);
        const QVariantList mixed = SessionStore::loadIndex(mixedReader);
        CHECK(mixed.size() == 1);
        if (mixed.size() == 1)
            CHECK(entryOf(mixed, 0).value(QStringLiteral("dataId")).toString()
                  == QStringLiteral("ok"));
    }
    return failed;
}

} // namespace

int tst_sessionstore()
{
    int failed = 0;
    failed += groupKeyShape();

    const ScopedTempRoot root(SUITE);
    if (!root.isValid())
    {
        TestHarness::skip("SKIP %s: 需要 LITE_TEST_TMPROOT（须以 /tmp-test 结尾）\n", SUITE);
        return failed;
    }
    const QString rwDir = QDir(root.path()).filePath(QStringLiteral("rw-home"));
    QDir().mkpath(rwDir);
    const QString migDir = QDir(root.path()).filePath(QStringLiteral("mig-home"));
    QDir().mkpath(migDir);
    failed += groupReadWrite(rwDir);
    failed += groupMigration(migDir);
    return failed;
}
