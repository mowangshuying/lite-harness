// tst_fluconfigstore.cpp —— FluentUI 配置定位层直测（3rdparty/FluentUI/utils/FluConfigStore.h）
// 被测：三段定位（目录/文件名/格式）的路径解析、格式名折叠、相对目录锚定、
//       JSON ⇄ QSettings 扁平键互转、自定义 QSettings 格式的落盘与回读。
//
// 本套件为什么必须存在（不是凑覆盖率）：FluConfigUtils 改造前把 "../config/config.ini" 写死在
// 构造函数里，锚点跟随进程 CWD；改造后三段皆可由宿主注入。风险面全部是「跑得通但落错地方」：
//   · 缺省口径漂了（不再等价 ../config/config.ini）：没调 configure 的宿主静默换文件，
//     主题/语言回到缺省值，用户读成「软件忘了我的设置」，真凶是那一行拼接；
//   · 扩展名与 format 分裂（.ini 文件按 JSON 解析）：QSettings 判 FormatError，读侧全部回落默认；
//   · 相对目录没锚到 exe 而是随 CWD：正是改造前分发场景丢配置的根因，改错了等于没改；
//   · JSON 读写不对称（写出去嵌套、读回来扁平键对不上）：第一次改主题后重启就「忘了」。
// 四条都没有失败信号，只能靠断言钉死，冒烟运行看不全。
//
// 覆盖面边界（勿高估）：本套件测的是**定位层与格式层**（纯 QtCore）。它不证明
// FluConfigUtils::configure() 真的被 App.cpp 在首次 getUtils() 之前调用（那是接线，
// 属宿主 main 的顺序约束，验证手段是代码审查 + 冒烟运行），也不证明 FluentUI 内部
// 各 getUtils() 消费点在新定位下仍然一致。与 tst_settingscache / tst_uiscale 同口径。
//
// 分组：组 1~5 零夹具恒跑（纯判据 + QBuffer 内存往返）；组 6~8 需 LITE_TEST_TMPROOT，
// 未配置则整组 SKIP 并返回 0 失败（假绿由 LITE_TEST_MIN_PASS 门禁兜住）。
// 夹具组一律用**绝对目录**：createSettings 的缺省目录是相对的 "../config"，在测试进程里
// 会按 CWD 落点——那正是本改造要消灭的行为，绝不能让测试自己复现它。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include <FluConfigStore.h>

#include <QByteArray>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <memory>

namespace {

using TestHarness::check; // 本套件断言密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// ---- 组 1：缺省口径 = 改造前的硬编码 ----------------------------------------------

void testDefaultEquivalence()
{
    // 这一条是整个改造的兼容性底座：宿主三个键都不配时不调 configure，单例走的就是这里的缺省。
    // 一旦有人把 defaultDirPath/defaultFileName 改成 applicationDirPath 或别的名字，
    // 老用户的 config.ini 就地失联（主题回 Light、语言回 zh-CN），且没有任何报错。
    check(FluConfigStore::defaultDirPath() == QStringLiteral("../config"),
          "default dir stays '../config' (pre-refactor hardcode)");
    check(FluConfigStore::defaultFileName() == QStringLiteral("config"),
          "default file name stays 'config'");
    check(FluConfigStore::defaultFormat() == FluConfigFormat::Ini,
          "default format stays Ini");
    check(FluConfigStore::resolveFilePath(QString(), QString(), FluConfigFormat::Ini)
              == QStringLiteral("../config/config.ini"),
          "empty dir+name resolves to the historical '../config/config.ini'");

    // 空白串等同未设。宿主侧自配置合流批起**恒注入非空**目录/文件名（原 fluConfigDir/
    // fluConfigFile/fluConfigFormat 三键已删除），所以这条钉的不再是「用户手改留空」，而是
    // vendor API 自身的契约边界：一旦空白被当成有效目录，就会解析出 "/.ini" 这种落点
    // （QDir("").filePath 给出相对当前目录的裸文件名），错误比缺省更难查。
    check(FluConfigStore::resolveFilePath(QStringLiteral("   "), QStringLiteral("  "),
                                         FluConfigFormat::Ini)
              == QStringLiteral("../config/config.ini"),
          "whitespace-only dir/name falls back to defaults (not to a bare '.ini')");
}

// ---- 组 2：扩展名与格式强制对齐 ----------------------------------------------------

void testSuffixAlignment()
{
    // 传 "config.ini" 而 format=Json 必须得到 "config.json"：宁可改写调用方的扩展名，
    // 也不能让「按 JSON 解析一个 ini 文本」这种磁盘/格式分裂状态出现（后果是 FormatError + 全回落）
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("config.ini"),
                                         FluConfigFormat::Json)
              == QStringLiteral("D:/app/config.json"),
          "Json format rewrites a stale .ini extension instead of trusting it");
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("config.json"),
                                         FluConfigFormat::Ini)
              == QStringLiteral("D:/app/config.ini"),
          "Ini format rewrites a stale .json extension");
    // 无扩展名 → 补上；已是正确扩展名 → 原样（不得出现 "config.json.json"）
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("config"),
                                         FluConfigFormat::Json)
              == QStringLiteral("D:/app/config.json"),
          "extensionless name gets the format suffix appended");
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("my.json"),
                                         FluConfigFormat::Json)
              == QStringLiteral("D:/app/my.json"),
          "already-matching extension is not doubled");
    // 大小写不敏感比对：手写成 .JSON 也视为已对齐，不再改写（磁盘名原样保留）
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("My.JSON"),
                                         FluConfigFormat::Json)
              == QStringLiteral("D:/app/My.JSON"),
          "case-variant matching extension is accepted as-is");
    // 只剥最后一个点之后的部分：多段名 "app.config" 的后段被当作扩展名换掉（文档化行为，
    // 免得日后有人「顺手修成只认已知扩展名」，把同一份 config.json 写出两种落点）
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral("app.config"),
                                         FluConfigFormat::Ini)
              == QStringLiteral("D:/app/app.ini"),
          "only the last dot-segment is treated as the extension");
    // 隐藏文件式命名 ".json"：整体就是名字，dot==0 不该被当成扩展名剥掉
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app"), QStringLiteral(".json"),
                                         FluConfigFormat::Ini)
              == QStringLiteral("D:/app/.json.ini"),
          "leading-dot name is not mistaken for an extension (dot > 0 guard)");
    // 目录尾斜杠不产生双分隔符
    check(FluConfigStore::resolveFilePath(QStringLiteral("D:/app/"), QStringLiteral("config"),
                                         FluConfigFormat::Ini)
              == QStringLiteral("D:/app/config.ini"),
          "trailing slash in dir does not double the separator");
}

// ---- 组 3：格式名折叠（手改面的唯一入口） -----------------------------------------

void testFormatNameFolding()
{
    // 缺省/空/认不出 → Ini：配错格式名的后果该是「用回老格式」，不是「读不到配置」
    check(FluConfigStore::formatFromName(QString()) == FluConfigFormat::Ini,
          "missing format name -> Ini");
    check(FluConfigStore::formatFromName(QStringLiteral("json")) == FluConfigFormat::Json,
          "'json' -> Json");
    check(FluConfigStore::formatFromName(QStringLiteral("JSON")) == FluConfigFormat::Json,
          "'JSON' -> Json (case insensitive)");
    check(FluConfigStore::formatFromName(QStringLiteral("  Json  ")) == FluConfigFormat::Json,
          "surrounding spaces tolerated");
    check(FluConfigStore::formatFromName(QStringLiteral("ini")) == FluConfigFormat::Ini,
          "'ini' -> Ini");
    check(FluConfigStore::formatFromName(QStringLiteral("INI")) == FluConfigFormat::Ini,
          "'INI' -> Ini");
    check(FluConfigStore::formatFromName(QStringLiteral("xml")) == FluConfigFormat::Ini,
          "unknown name -> default Ini (not a hard failure)");
    // 显式 fallback 必须被尊重：调用方有权把缺省档换成 Json（例如新装默认走 json）
    check(FluConfigStore::formatFromName(QStringLiteral("xml"), FluConfigFormat::Json)
              == FluConfigFormat::Json,
          "explicit fallback honored for unknown names");
    check(FluConfigStore::formatFromName(QStringLiteral(""), FluConfigFormat::Json)
              == FluConfigFormat::Json,
          "explicit fallback honored for empty names");
    // 写侧/诊断名与读侧对称：nameForFormat 的输出必须能被 formatFromName 原样读回
    check(FluConfigStore::nameForFormat(FluConfigFormat::Json) == QStringLiteral("json"),
          "json name is lowercase and round-trippable");
    check(FluConfigStore::nameForFormat(FluConfigFormat::Ini) == QStringLiteral("ini"),
          "ini name is lowercase and round-trippable");
    check(FluConfigStore::formatFromName(FluConfigStore::nameForFormat(FluConfigFormat::Json))
                  == FluConfigFormat::Json
              && FluConfigStore::formatFromName(FluConfigStore::nameForFormat(FluConfigFormat::Ini))
                     == FluConfigFormat::Ini,
          "nameForFormat -> formatFromName round trip");
}

// ---- 组 4：相对目录锚定（改造的直接动机） -----------------------------------------

void testAnchorRelativeDir()
{
    // 绝对路径原样透传：调用方已经说死落点，不该再被 anchor 拼一遍变成 "D:/app/D:/cfg"
    check(FluConfigStore::anchorRelativeDir(QStringLiteral("D:/cfg"), QStringLiteral("D:/app/bin"))
              == QStringLiteral("D:/cfg"),
          "absolute dir passes through untouched");
    // 相对目录钉到 anchor：'../config' 相对 exe 目录而非进程 CWD —— 改造前主题在分发场景丢失的根因
    check(FluConfigStore::anchorRelativeDir(QStringLiteral("../config"),
                                           QStringLiteral("D:/app/bin"))
              == QStringLiteral("D:/app/config"),
          "'../config' anchors on the exe dir, not on CWD");
    check(FluConfigStore::anchorRelativeDir(QStringLiteral("config"),
                                           QStringLiteral("D:/app/bin"))
              == QStringLiteral("D:/app/bin/config"),
          "plain relative dir joins the anchor");
    // 空目录返回空：让 resolveFilePath 去回落缺省，而不是在这里猜一个
    check(FluConfigStore::anchorRelativeDir(QString(), QStringLiteral("D:/app/bin")).isEmpty(),
          "empty dir stays empty (default fallback belongs to resolveFilePath)");
    // anchor 为空时透传：applicationDirPath() 在无 QCoreApplication 实例时返回空串，
    // 此时宁可保留相对口径，也不要把 'D:/x' 之类绝对值拼坏
    check(FluConfigStore::anchorRelativeDir(QStringLiteral("../config"), QString())
              == QStringLiteral("../config"),
          "empty anchor passes the dir through (no guessing)");
    check(FluConfigStore::anchorRelativeDir(QStringLiteral("  ../config  "),
                                           QStringLiteral("D:/app/bin"))
              == QStringLiteral("D:/app/config"),
          "dir is trimmed before anchoring");
    // 锚定结果必须是干净路径（absoluteFilePath 而非 filePath：后者不折叠 ../）
    check(!FluConfigStore::anchorRelativeDir(QStringLiteral("../config"),
                                             QStringLiteral("D:/app/bin"))
                    .contains(QStringLiteral("..")),
          "anchored result is cleaned ('..' folded away)");
}

// ---- 组 5：扁平键 ⇄ 嵌套对象 + 读写回调（内存 QBuffer，零磁盘） ---------------------

void testFlattenBuildRoundTrip()
{
    // QSettings 内部是 '/' 分层的扁平 map（beginGroup("config") + setValue("theme") ⇒ "config/theme"），
    // JSON 侧按同构嵌套对象落盘：读侧 flatten、写侧 buildObject，两者必须互逆
    QJsonObject nested;
    QJsonObject config;
    config.insert(QStringLiteral("theme"), QStringLiteral("Dark"));
    config.insert(QStringLiteral("navWidth"), 300);
    nested.insert(QStringLiteral("config"), config);
    nested.insert(QStringLiteral("language"), QStringLiteral("zh-CN"));

    QVariantMap flat;
    FluConfigStore::flatten(nested, QString(), flat);
    check(flat.value(QStringLiteral("config/theme")).toString() == QStringLiteral("Dark"),
          "flatten maps nested object to 'config/theme'");
    check(flat.value(QStringLiteral("config/navWidth")).toInt() == 300,
          "flatten keeps numeric leaf types");
    check(flat.value(QStringLiteral("language")).toString() == QStringLiteral("zh-CN"),
          "flatten leaves top-level scalars alone");
    check(flat.size() == 3, "flatten produces exactly one entry per leaf");

    const QJsonObject rebuilt = FluConfigStore::buildObject(flat);
    check(rebuilt == nested, "buildObject(flatten(x)) == x (nested round trip)");

    // 三层嵌套：QSettings 允许 beginGroup 里再 beginGroup，写侧必须跟着嵌套下去，
    // 否则读回来是 "a/b/c" 这种带斜杠的裸键名，与磁盘上的扁平键不再同构
    QVariantMap deep;
    deep.insert(QStringLiteral("a/b/c"), QStringLiteral("v"));
    const QJsonObject deepObj = FluConfigStore::buildObject(deep);
    check(deepObj.contains(QStringLiteral("a"))
              && deepObj.value(QStringLiteral("a")).toObject()
                     .value(QStringLiteral("b")).toObject()
                     .value(QStringLiteral("c")).toString()
                     == QStringLiteral("v"),
          "three-level key becomes three-level object");
    QVariantMap deepBack;
    FluConfigStore::flatten(deepObj, QString(), deepBack);
    check(deepBack == deep, "deep key round trip");

    // 数组是叶子：不得被当成对象展开（QSettings 的 QStringList 值在 JSON 里就是数组）
    QVariantMap withList;
    withList.insert(QStringLiteral("config/langs"), QStringList{QStringLiteral("zh-CN"), QStringLiteral("en-US")});
    const QJsonObject listObj = FluConfigStore::buildObject(withList);
    check(listObj.value(QStringLiteral("config")).toObject().value(QStringLiteral("langs")).isArray(),
          "list value stays a JSON array leaf");
    QVariantMap listBack;
    FluConfigStore::flatten(listObj, QString(), listBack);
    check(listBack.value(QStringLiteral("config/langs")).toStringList().size() == 2,
          "list value survives the round trip");
}

void testJsonCallbacks()
{
    // 空设备 = 空配置而非错误：首次运行本就没有这个文件，判错会让 QSettings 置 FormatError，
    // 于是所有 key 静默回落默认值（症状与「文件被写坏」一模一样，最难查的那类回归）
    QByteArray empty;
    QBuffer emptyBuf(&empty);
    emptyBuf.open(QIODevice::ReadOnly);
    QVariantMap emptyMap;
    check(FluConfigStore::readJson(emptyBuf, emptyMap), "empty json device reads as success");
    check(emptyMap.isEmpty(), "empty json device yields an empty map");

    // 顶层不是对象（数组/标量）→ false：交给 QSettings 置 FormatError，读侧回落各 key 默认值
    QByteArray arrayJson("[1,2,3]");
    QBuffer arrayBuf(&arrayJson);
    arrayBuf.open(QIODevice::ReadOnly);
    QVariantMap arrayMap;
    check(!FluConfigStore::readJson(arrayBuf, arrayMap), "top-level array is rejected");

    QByteArray brokenJson("{ not valid json ");
    QBuffer brokenBuf(&brokenJson);
    brokenBuf.open(QIODevice::ReadOnly);
    QVariantMap brokenMap;
    check(!FluConfigStore::readJson(brokenBuf, brokenMap), "malformed json is rejected");

    // 写→读内存往返：证明 writeJson 的产物能被 readJson 原样吃回去（不依赖磁盘与注册格式）
    QVariantMap src;
    src.insert(QStringLiteral("config/theme"), QStringLiteral("AtomOneDark"));
    src.insert(QStringLiteral("config/navWidth"), 260);
    src.insert(QStringLiteral("translation/language"), QStringLiteral("en-US"));
    QByteArray out;
    QBuffer writeBuf(&out);
    writeBuf.open(QIODevice::ReadWrite);
    check(FluConfigStore::writeJson(writeBuf, src), "writeJson reports success");
    writeBuf.seek(0);
    QVariantMap dst;
    check(FluConfigStore::readJson(writeBuf, dst), "written json reads back");
    check(dst == src, "writeJson -> readJson round trip is value-identical");
    // 落盘形态必须是嵌套对象（人读起来与 ini 的 [config] 段等价），不是扁平带斜杠键
    const QJsonDocument doc = QJsonDocument::fromJson(out);
    check(doc.isObject() && doc.object().contains(QStringLiteral("config"))
              && !out.contains("config/theme"),
          "on-disk shape is nested, not slash-quoted flat keys");
}

// ---- 组 6~8：真文件（需 LITE_TEST_TMPROOT） ---------------------------------------

void testJsonBackendOnDisk(const QString &dir)
{
    // createSettings 走的是 FluConfigUtils 重建后端时的同一条路：解析路径 → 铺目录 → 建 QSettings
    std::unique_ptr<QSettings> settings(FluConfigStore::createSettings(
        dir, QStringLiteral("liteharness"), FluConfigFormat::Json));
    check(settings != nullptr, "createSettings returns a QSettings for the Json format");
    if (settings == nullptr)
        return;

    settings->beginGroup(QStringLiteral("config"));
    settings->setValue(QStringLiteral("theme"), QStringLiteral("Dark"));
    settings->setValue(QStringLiteral("navWidth"), 320);
    settings->setValue(QStringLiteral("viewCollapsed"), true);
    settings->endGroup();
    settings->setValue(QStringLiteral("translation/language"), QStringLiteral("en-US"));
    settings->sync();
    check(settings->status() == QSettings::NoError, "json backend writes without error");

    const QString filePath =
        FluConfigStore::resolveFilePath(dir, QStringLiteral("liteharness"), FluConfigFormat::Json);
    check(QFileInfo::exists(filePath), "json config file lands at the resolved path");

    // 磁盘形态必须是 JSON 而非 ini：注册格式没生效时 QSettings 会退回 IniFormat 的写法，
    // 文件名叫 .json 但内容是 [config] 段——下次按 JSON 解析直接 FormatError，全量回落默认
    QFile file(filePath);
    const bool jsonReadable = file.open(QIODevice::ReadOnly);
    check(jsonReadable, "json config file is readable");
    if (jsonReadable)
    {
        const QByteArray raw = file.readAll();
        file.close();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        check(doc.isObject(), "file content really is a JSON object (custom format took effect)");
        check(doc.object().value(QStringLiteral("config")).toObject()
                      .value(QStringLiteral("theme"))
                      .toString()
                  == QStringLiteral("Dark"),
              "theme lands under the nested 'config' object");
        check(!raw.contains("[config]"), "no ini section header leaked into the .json file");
    }

    // 换一个新实例回读（等价于重启进程）：写出去的东西必须读得回来，否则「改完主题重启就忘」
    const QSettings reopened(filePath, FluConfigStore::settingsFormat(FluConfigFormat::Json));
    check(reopened.status() == QSettings::NoError, "reopen reports NoError");
    check(reopened.value(QStringLiteral("config/theme")).toString() == QStringLiteral("Dark"),
          "nested key reads back flat-notation after reopen");
    check(reopened.value(QStringLiteral("config/navWidth")).toInt() == 320,
          "int value reads back as int");
    check(reopened.value(QStringLiteral("config/viewCollapsed")).toBool(),
          "bool value reads back true");
    check(reopened.value(QStringLiteral("translation/language")).toString()
              == QStringLiteral("en-US"),
          "second group reads back");
    // FluentUI 的实际读法是 beginGroup("config") + value("theme")，与全键名读法必须同值
    {
        QSettings grouped(filePath, FluConfigStore::settingsFormat(FluConfigFormat::Json));
        grouped.beginGroup(QStringLiteral("config"));
        check(grouped.value(QStringLiteral("theme")).toString() == QStringLiteral("Dark"),
              "beginGroup read matches the flat-key read (FluConfigUtils' own access path)");
        grouped.endGroup();
    }
}

void testIniBackendOnDisk(const QString &dir)
{
    // 缺省档必须真的还是 ini：改造只加注入口，不该把没配 json 的宿主顺带切了格式
    const QSettings settings(dir + QStringLiteral("/config.ini"), QSettings::IniFormat);
    QSettings &mutableSettings = const_cast<QSettings &>(settings);
    mutableSettings.beginGroup(QStringLiteral("config"));
    mutableSettings.setValue(QStringLiteral("theme"), QStringLiteral("Light"));
    mutableSettings.endGroup();
    mutableSettings.sync();
    check(mutableSettings.status() == QSettings::NoError, "ini backend writes without error");

    QFile file(dir + QStringLiteral("/config.ini"));
    const bool iniReadable = file.open(QIODevice::ReadOnly);
    check(iniReadable, "ini config file is readable");
    if (iniReadable)
    {
        const QByteArray raw = file.readAll();
        file.close();
        check(raw.contains("[config]") && raw.contains("theme=Light"),
              "ini file keeps the historical text shape (readable by hand-editing users)");
    }
}

void testBadJsonFallsBackAndEmptyFileIsNotAnError(const QString &dir)
{
    // 夹具目录自建：QSettings 写侧会 mkpath 父目录，但本组要的是「先摆一个坏/空文件在磁盘上」，
    // 用裸 QFile 写就必须目录先存在，否则 open 失败会把「坏文件回落默认」这条判据变成空跑
    QDir().mkpath(dir);
    // 手改坏 JSON：QSettings 判 FormatError，读侧一律带默认值取（症状是「设置回到出厂」，
    // 不是崩溃）——本条把「坏文件不炸」钉住，免得日后有人给 readJson 加 qWarning/assert
    const QString brokenPath = dir + QStringLiteral("/broken.json");
    QFile broken(brokenPath);
    const bool brokenWritable = broken.open(QIODevice::WriteOnly | QIODevice::Truncate);
    check(brokenWritable, "broken.json is writable");
    if (brokenWritable)
    {
        broken.write("{ oops ");
        broken.close();
        const QSettings settings(brokenPath, FluConfigStore::settingsFormat(FluConfigFormat::Json));
        check(settings.status() == QSettings::FormatError, "malformed json surfaces as FormatError");
        check(settings.value(QStringLiteral("config/theme"), QStringLiteral("Light")).toString()
                  == QStringLiteral("Light"),
              "malformed json still yields the caller's default, not a crash");
    }

    // 空文件（0 字节）不是错误：ensureDirFor/打包流程都可能留下一个空文件，
    // 判错会让它和「文件被写坏」共用同一条静默回落路径，无从区分
    const QString emptyPath = dir + QStringLiteral("/empty.json");
    QFile empty(emptyPath);
    const bool emptyCreatable = empty.open(QIODevice::WriteOnly | QIODevice::Truncate);
    check(emptyCreatable, "empty.json is creatable");
    if (emptyCreatable)
    {
        empty.close();
        const QSettings settings(emptyPath, FluConfigStore::settingsFormat(FluConfigFormat::Json));
        check(settings.status() == QSettings::NoError, "empty json file is not an error");
        check(settings.value(QStringLiteral("config/theme"), QStringLiteral("Light")).toString()
                  == QStringLiteral("Light"),
              "empty json file reads defaults");
    }
}

void testMissingDirectoryIsCreated(const QString &dir)
{
    // 「打包剥离 config/」正是改造前主题丢失的直接成因：QSettings 不自建父目录，
    // 目录不存在时写入失败且**完全无声**。ensureDirFor 补的就是这一课。
    const QString nestedDir = QDir(dir).filePath(QStringLiteral("nested/deeper/config"));
    check(!QFileInfo::exists(nestedDir), "precondition: nested config dir does not exist yet");

    std::unique_ptr<QSettings> settings(
        FluConfigStore::createSettings(nestedDir, QStringLiteral("config"), FluConfigFormat::Ini));
    settings->beginGroup(QStringLiteral("config"));
    settings->setValue(QStringLiteral("theme"), QStringLiteral("Dark"));
    settings->endGroup();
    settings->sync();

    check(QFileInfo::exists(nestedDir), "createSettings mkpaths the missing directory");
    check(QFileInfo::exists(QDir(nestedDir).filePath(QStringLiteral("config.ini"))),
          "config file lands inside the freshly created directory");
    check(settings->status() == QSettings::NoError, "write into a previously missing dir succeeds");
}

} // namespace

int tst_fluconfigstore()
{
    const int before = TestHarness::failCount();

    // 组 1~5：零夹具，任何环境全量执行
    testDefaultEquivalence();
    testSuffixAlignment();
    testFormatNameFolding();
    testAnchorRelativeDir();
    testFlattenBuildRoundTrip();
    testJsonCallbacks();

    ScopedTempRoot root(QStringLiteral("fluconfigstore"));
    if (!root.isValid())
    {
        TestHarness::skip(
            "SKIP: LITE_TEST_TMPROOT unavailable, flu config store file groups skipped\n");
        return TestHarness::failCount() - before;
    }

    // 每组各自的目录：JSON/INI/坏文件互不污染同一份磁盘文件（共用一个文件会让「扩展名改写」
    // 之类的断言依赖执行顺序，改天插新用例就假红）
    testJsonBackendOnDisk(QDir(root.path()).filePath(QStringLiteral("json")));
    testIniBackendOnDisk(QDir(root.path()).filePath(QStringLiteral("ini")));
    testBadJsonFallsBackAndEmptyFileIsNotAnError(
        QDir(root.path()).filePath(QStringLiteral("badjson")));
    testMissingDirectoryIsCreated(QDir(root.path()).filePath(QStringLiteral("mkpath")));

    return TestHarness::failCount() - before;
}
