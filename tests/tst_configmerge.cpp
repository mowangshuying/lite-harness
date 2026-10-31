// 套件：宿主配置与 FluentUI 配置**合流为同一个文件**后的三条承重不变量（P2）。
//
// 这个套件存在的理由，是先纠正一条写在改造规划里的错误前提。规划把「两个 QSettings 实例写
// 同一文件会互相覆盖」列为本次最大风险项，并据此要求「写侧只能有一个 owner」。读 Qt 6.9 源码
// （qsettings.cpp）证否：
//   · QConfFile::fromName（:153-171）的进程级缓存键**只有 absPath**，不含 format——同一路径的
//     多个 QSettings 实例引用计数共享**同一个 QConfFile**，即同一张 originalKeys/addedKeys 表；
//   · syncConfFile（:1337 起）写前先 fileInfo.refresh()，盘上 size/mtime 变了就重读并把新键
//     并进 originalKeys（自定义格式走 readFunc → tempNewKeys → insert，:1425-1434），落盘写的是
//     mergedKeyMap() = originalKeys + addedKeys − removedKeys（:1452），全程 QLockFile 保护。
// 所以「宿主写 app/apiToken 会把 FluentUI 的 config/theme 抹掉」不成立，单 owner 是多余约束。
//
// 但同一条源码翻过来给出**真正的**硬约束，本套件钉的就是它：
//   ① 缓存键只有 absPath ⇒ 同一个文件必须全仓用**同一个 QSettings::Format**。若一侧 Ini、一侧
//      Json，两个实例仍共享同一个 QConfFile，却各持一套 readFunc/writeFunc 交替读写同一份字节
//      ——先解析者独占那张表。格式单源因此必须是 FluConfigStore::jsonFormat()，宿主不得自注册。
//   ② 宿主侧 LitePaths 与 vendor 侧 resolveFilePath 必须算出**逐字相同**的路径。差一个分隔符或
//      扩展名就是两个文件、两套内存表，「合流」静默失效且没有任何报错。
//   ③ 折叠口径必须与格式**成对**交出：vendor 的 json 后端注册为 Qt::CaseSensitive，那是第三方
//      库自己的语义、宿主无权为迁就缓存去改它（本轮一度把它改成 CaseInsensitive 让键折小写落盘，
//      已回退）。正确的对齐方向是宿主侧：SettingsCache 原先无条件 toLower() 的成立前提是「后端
//      本身不敏感」，换成敏感后端后它会让**缓存 hit、直取 QSettings miss**——即 Reader 自己违反
//      它的不变量①。故 Reader 增 keyCaseSensitivity 入参，AppSettings 配对传 CaseSensitive。
//      这条比①②更阴：①②错了当场读不到值，③错了**读得到**、只是缓存与直取给出不同答案。
//
// 与相邻套件的分工：tst_settingscache 测 Reader 的缓存/失效判据（格式无关），tst_fluconfigstore
// 测 vendor 定位层纯函数，tst_fluconfigutils 测门面注入→重建→往返；**只有本套件**同时握两侧入口，
// 因而也只有它能证明「这两个 owner 真的在动同一个文件」。
//
// 刻意不调 AppSettings::value()/setValue()：那对口径绑死 applicationDirPath()（reader() 里
// Q_ASSERT 拦的就是这条），测试里会把配置写进测试 exe 目录。落点一律经 Reader 构造注入，
// 而键名口径直接复用生产单点 AppSettings::scopedKey()——被测的字符串规则与线上是同一份代码。
#include "AgentConstants.h" // normalizedText()：清单键「列表形态」归一化单点（纯 QtCore）
#include "AppSettings.h" // scopedKey()/keyGroup()：宿主键名口径单点（纯字符串，不触盘）
#include "FluConfigUtils.h"
#include "LitePaths.h"
#include "ScopedTempRoot.h"
#include "SettingsCache.h"
#include "TestHarness.h"

#include <QDir>
#include <QFile>
#include <cstdio>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QVariant>

#define SUITE "configmerge"
#define TOSTR2(x) #x
#define TOSTR(x) TOSTR2(x)
#define CHECK(cond)                                                                                \
    TestHarness::check((cond), (cond) ? "" : SUITE ": " __FILE__ ":" TOSTR(__LINE__))

namespace {

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

// ---- 组 A：纯字符串不变量（零 IO，任何环境恒跑）----
// ② 的静态那一半：两侧路径拼法在此对表，不依赖夹具就能抓到分叉。
int groupPathAndKeyAgreement()
{
    int failed = 0;
    const QString root = QStringLiteral("D:/pkg/config");

    // 宿主落点单源：config.json 直接挂在存储根下
    CHECK(LitePaths::configFilePathFor(root) == root + QStringLiteral("/config.json"));

    // ② 关键对表：vendor 按「目录 + 文件名 + 格式」三段解析出的路径，必须与宿主 LitePaths 拼出
    // 的逐字相同。App.cpp 正是把 LitePaths::configFileName() 交给 configure()，这条断言就是
    // 那个交接契约的可执行定义——它一红，合流就退化成两个文件。
    const QString vendorPath = FluConfigStore::resolveFilePath(
        root, LitePaths::configFileName(), FluConfigFormat::Json);
    const QString hostPath = LitePaths::configFilePathFor(root);
    CHECK(vendorPath == hostPath);

    // ① 格式单源：宿主取的就是 vendor 的注册口，两侧必须是同一个枚举值（不是「两个恰好等价的
    // 格式」——registerFormat 每次调用都会占一个新 CustomFormat 槽，值不同就是不同后端）。
    CHECK(AppSettings::fileFormat() == FluConfigStore::jsonFormat());

    // ③ 大小写口径不在这里读注册项：Qt 6.9 的 QSettings::FormatOptions / registeredFormatOptions
    // 都不是公开 API（qsettings.h 公开段只有 registerFormat 的入参类型可见），故该不变量改由组 B
    // 用**行为**钉住——往 json 后端写混排大小写的键、再用另一种大小写取回。这比读一个注册结构更硬：
    // 它验的是「两个 owner 实际看到的折叠规则」，而不是「我们声称注册成了什么」。

    // 宿主键名口径：裸键一律收进 app/ 组（盘上即 {"app":{...}}），与 FluentUI 的 config/ 分属
    // 两个子对象——这是合流后两个 owner 不抢顶层命名空间的唯一保证。
    CHECK(AppSettings::keyGroup() == QStringLiteral("app"));
    CHECK(AppSettings::scopedKey(QStringLiteral("apiBaseUrl")) == QStringLiteral("app/apiBaseUrl"));
    CHECK(AppSettings::scopedKey(QStringLiteral("language")) == QStringLiteral("app/language"));
    // FluentUI 自有键不归宿主前缀管：合流后它仍写 config/*、translation/*（门面内部 beginGroup）
    CHECK(!AppSettings::scopedKey(QStringLiteral("theme")).startsWith(QStringLiteral("config/")));
    return failed;
}

// ---- 组 B：合写不互覆（夹具门控，真落盘）----
// ①②③ 的动态那一半：让两个 owner 交替写同一个文件，逐轮验证对方的键都还在。
int groupCoWrite(const QString &dir)
{
    int failed = 0;

    // 宿主注入 FluentUI 落点：与 App.cpp 同一套参数（根 + 单源文件名 + Json）
    FluConfigUtils::configure(dir, LitePaths::configFileName(), FluConfigFormat::Json);
    FluConfigUtils *utils = FluConfigUtils::getUtils();

    const QString path = LitePaths::configFilePathFor(dir);
    // ② 的动态确认：门面解析出的路径与宿主拼出的必须是同一个串
    CHECK(FluConfigUtils::configFilePath() == path);

    // 格式与折叠口径**成对**传入（与 AppSettings::reader() 同一套参数）。只传 format 不传口径
    // 就是本套件要抓的那个错配形态，测试自己绝不能先犯——犯了就等于用错的口径去验对的实现。
    SettingsCache::Reader host(path, AppSettings::fileFormat(), AppSettings::keyCaseSensitivity());

    // 起点：FluentUI 先写两个自有键
    utils->setNavWidth(320);
    utils->setViewCollapsed(true);
    CHECK(utils->getNavWidth() == 320);

    // 宿主写入自己的键
    CHECK(host.setValue(AppSettings::scopedKey(QStringLiteral("apiToken")), QStringLiteral("tok-1")));
    CHECK(host.value(AppSettings::scopedKey(QStringLiteral("apiToken"))).toString()
          == QStringLiteral("tok-1"));

    // ② 核心断言：宿主写完，FluentUI 的键没被抹掉（规划声称会被抹掉，此处证否）
    CHECK(utils->getNavWidth() == 320);
    CHECK(utils->getViewCollapsed() == true);

    // 反向：FluentUI 再写，宿主的键也没被抹掉
    utils->setNavWidth(400);
    CHECK(utils->getNavWidth() == 400);
    CHECK(host.value(AppSettings::scopedKey(QStringLiteral("apiToken"))).toString()
          == QStringLiteral("tok-1"));

    // 盘上真相（不信内存，直接读文件）：一个文件、两个子对象、四把键都在
    // 键名保持驼峰：json 后端注册为 Qt::CaseSensitive（vendor 自己的语义），落盘不折键名。
    const QJsonObject root = jsonAt(path);
    CHECK(!root.isEmpty());
    const QJsonObject app = root.value(QStringLiteral("app")).toObject();
    const QJsonObject config = root.value(QStringLiteral("config")).toObject();
    CHECK(app.value(QStringLiteral("apiToken")).toString() == QStringLiteral("tok-1"));
    CHECK(config.value(QStringLiteral("navWidth")).toInt() == 400);
    CHECK(config.value(QStringLiteral("viewCollapsed")).toBool() == true);
    // 宿主键不得漏到顶层（漏了就是 scopedKey 单点被绕过，盘上会同时存在两份）
    CHECK(!root.contains(QStringLiteral("apiToken")));

    // ③ 口径配对（本套件的核心红线）：json 后端是 CaseSensitive，换大小写一律取不到。
    //    要钉的不是「取不到」本身，而是**缓存与直取必须给出同一个答案**——两侧同 miss 才对。
    QSettings direct(path, AppSettings::fileFormat());
    CHECK(host.value(AppSettings::scopedKey(QStringLiteral("APITOKEN"))).toString().isEmpty());
    CHECK(direct.value(QStringLiteral("app/APITOKEN")).toString().isEmpty());
    CHECK(direct.value(QStringLiteral("config/NAVWIDTH")).toInt() == 0);
    // 精确拼法两侧同 hit
    CHECK(host.value(AppSettings::scopedKey(QStringLiteral("apiToken"))).toString()
          == QStringLiteral("tok-1"));
    CHECK(direct.value(QStringLiteral("app/apiToken")).toString() == QStringLiteral("tok-1"));

    // 反证：口径配错（沿用 Reader 的 ini 缺省 CaseInsensitive 去读敏感后端）会怎样——
    // 缓存把 `app/APITOKEN` 折成小写命中，直取却 miss。两侧给出不同答案，且**不报任何错**。
    // 这条断言之所以能成立，正因为落盘键名是驼峰（见上面 jsonAt 那组）：折叠后的小写串
    // 在表里必然存在，而 QSettings 按精确串查它。这就是 AppSettings 必须成对传口径的全部理由。
    const SettingsCache::Reader mismatched(path, AppSettings::fileFormat());
    CHECK(!mismatched.value(AppSettings::scopedKey(QStringLiteral("APITOKEN"))).toString().isEmpty());

    // 跨实例可见性：另开一个 Reader（模拟「手改配置」路径），两侧写的键都读得到
    const SettingsCache::Reader other(path, AppSettings::fileFormat());
    CHECK(other.value(AppSettings::scopedKey(QStringLiteral("apiToken"))).toString()
          == QStringLiteral("tok-1"));
    CHECK(other.value(QStringLiteral("config/navWidth")).toInt() == 400);
    return failed;
}

// ---- 组 C：清单键的「列表形态」归一化（ini→json 合流带出的分叉，零夹具恒跑）----
// 合流前只有 ini 后端：裸逗号串被 QSettings 解析成 QStringList，归一化判 QStringList 就够。
// 合流后走 vendor 的 json 后端，其 flatten() 用 QJsonValue::toVariant()，数组落的是
// **QVariantList**（Qt 6.9 实测 typeId=9），不是 QStringList。若仍只判 QStringList，用户手写
// `"modelOptions": ["a","b"]`（JSON 下最自然的清单写法）就会走 toString() 得空串 → 被误判
// 「未配置」→ 静默回退内置清单——正是这道防线本要拦住的症状在新格式下复发。
int groupListShapeNormalization()
{
    int failed = 0;

    const QStringList iniList{QStringLiteral("qwen-a"), QStringLiteral("qwen-b"), QStringLiteral("qwen-c")};
    QVariantList jsonList;
    for (const QString &item : iniList)
        jsonList << QVariant(item);
    const QVariant asJsonArray = QVariant::fromValue(jsonList);

    // 本组的核心取证：json 数组形态的 metaType **不是** QStringList。旧写法只判 QStringList，
    // 在这一格必然漏过并落到 toString()——下面两条把「它确实不是 QStringList」与「它的
    // toString() 确实是空串」分开钉死，任一条红就说明归一化前提被破坏。
    CHECK(asJsonArray.metaType() != QMetaType::fromType<QStringList>());
    CHECK(asJsonArray.metaType() == QMetaType::fromType<QVariantList>());
    CHECK(asJsonArray.toString().isEmpty());

    // 两种列表形态都必须回接成同一个逗号串
    const QString expected = QStringLiteral("qwen-a,qwen-b,qwen-c");
    CHECK(AgentConst::normalizedText(QVariant(iniList)) == expected);
    CHECK(AgentConst::normalizedText(asJsonArray) == expected);

    // 标量形态原样取回（ini 带引号的串、json 字符串都走这条），未配置回空串交消费点回退默认
    CHECK(AgentConst::normalizedText(QVariant(expected)) == expected);
    CHECK(AgentConst::normalizedText(QVariant()).isEmpty());

    return failed;
}

} // namespace

int tst_configmerge()
{
    int failed = 0;
    failed += groupPathAndKeyAgreement();
    failed += groupListShapeNormalization();

    const ScopedTempRoot root(SUITE);
    if (!root.isValid())
    {
        TestHarness::skip("SKIP %s: 需要 LITE_TEST_TMPROOT（须以 /tmp-test 结尾）\n", SUITE);
        return failed;
    }
    const QString dir = QDir(root.path()).filePath(QStringLiteral("merged-home"));
    QDir().mkpath(dir);
    failed += groupCoWrite(dir);
    return failed;
}
