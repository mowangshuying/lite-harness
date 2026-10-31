// 套件：FluentUI 配置门面 FluConfigUtils —— 改造后的「注入 → 后端重建 → 读写往返」端到端。
//
// 与 tst_fluconfigstore 的分工：那边测纯 QtCore 的定位层（字符串与后端回调），本套件测的是
// 门面这一侧、只有真跑一遍才能证明的东西：configure() 是否真的换了落点与格式、单例已建时
// 重建是否生效、同值注入是否不重建（重建会让交出去的裸指针失效）、换定位后旧值是否**不迁移**、
// 以及 getTheme/setTheme 等既有 API 在新后端上是否照常工作。
//
// FluConfigUtils.cpp 能进这个只链 Qt6::Core 的测试目标：它 include 的 FluThemeUtils.h 虽带
// Q_OBJECT，但本套件只取其中的 FluTheme 枚举、从不实例化 FluThemeUtils，故不需要它的 moc 产物；
// FluConfigUtils 自身没有 Q_OBJECT（无信号槽），也不产生 moc 依赖。
//
// 恒跑组（组 A）刻意**不建单例**：configure 未调用时缺省定位是 "../config/config.ini"，
// 那是相对进程 CWD 的路径，一旦建出后端就会以 CWD 为锚 mkpath ../config，把测试产物写进仓库。
// 其余各组一律用绝对临时目录（ScopedTempRoot），落点与 CWD 无关。
#include "FluConfigUtils.h"
#include "ScopedTempRoot.h"
#include "TestHarness.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>

#define SUITE "fluconfigutils"
#define TOSTR2(x) #x
#define TOSTR(x) TOSTR2(x)
#define CHECK(cond)                                                                                \
    TestHarness::check((cond), (cond) ? "" : SUITE ": " __FILE__ ":" TOSTR(__LINE__))

namespace {

// 在临时根下建一个子目录并返回其绝对路径（QDir::filePath 产反斜杠，QSettings 与 QDir 都吃）
QString subDir(const QString& rootPath, const QString& name)
{
    const QString dir = QDir(rootPath).filePath(name);
    QDir().mkpath(dir);
    return dir;
}

// 读整个文件（不存在/读不动返回空串）
QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// 把某个路径当 JSON 配置读成对象；非对象/坏文件返回空对象
QJsonObject jsonAt(const QString& path)
{
    const QByteArray raw = readAll(path).toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    return doc.isObject() ? doc.object() : QJsonObject();
}

// config/theme 的落盘值（JSON 侧看嵌套结构，不再用斜杠键）
QString themeInFile(const QString& path, FluConfigFormat format)
{
    if (format == FluConfigFormat::Json)
    {
        const QJsonValue config = jsonAt(path).value(QStringLiteral("config"));
        return config.toObject().value(QStringLiteral("theme")).toString();
    }
    QSettings s(path, QSettings::IniFormat);
    return s.value(QStringLiteral("config/theme")).toString();
}

// ---- 组 A：不调 configure 时的缺省口径（纯字符串，绝不触盘）----
int groupDefaultsUntouched()
{
    int failed = 0;

    CHECK(FluConfigUtils::configDirPath() == QStringLiteral("../config"));
    CHECK(FluConfigUtils::configFileName() == QStringLiteral("config"));
    CHECK(FluConfigUtils::configFormat() == FluConfigFormat::Ini);
    // 改造前构造函数里写死的就是这一条：三段解析必须逐字复现它
    CHECK(FluConfigUtils::configFilePath() == QStringLiteral("../config/config.ini"));
    return failed;
}

// ---- 组 B：注入 JSON 定位，且是首次建单例 ----
int groupJsonFirstSingleton(const QString& jsonDir, FluConfigUtils** outUtils, QString* outJsonPath)
{
    int failed = 0;
    *outJsonPath = QString();
    *outUtils = nullptr;

    FluConfigUtils::configure(jsonDir, QStringLiteral("ui-config"), FluConfigFormat::Json);

    CHECK(FluConfigUtils::configDirPath() == jsonDir);
    CHECK(FluConfigUtils::configFileName() == QStringLiteral("ui-config"));
    CHECK(FluConfigUtils::configFormat() == FluConfigFormat::Json);

    const QString jsonPath = FluConfigUtils::configFilePath();
    CHECK(jsonPath == QDir(jsonDir).filePath(QStringLiteral("ui-config.json")));
    // 单例还没建，任何文件都不该存在（configure 只改静态三段）
    CHECK(!QFile::exists(jsonPath));
    CHECK(!QFile::exists(QDir(jsonDir).filePath(QStringLiteral("ui-config.ini"))));

    FluConfigUtils* utils = FluConfigUtils::getUtils();
    CHECK(utils != nullptr);
    if (utils == nullptr)
        return failed;
    CHECK(utils == FluConfigUtils::getUtils()); // 单例语义没被改造破坏
    *outUtils = utils;
    *outJsonPath = jsonPath;

    // 空文件回落：getTheme 无值时是 Light
    CHECK(utils->getTheme() == FluTheme::Light);

    utils->setTheme(FluTheme::Dark);
    CHECK(QFile::exists(jsonPath));
    CHECK(themeInFile(jsonPath, FluConfigFormat::Json) == QStringLiteral("Dark"));
    // 值确实写成嵌套对象，而不是扁平的 "config/theme" 顶层键
    const QJsonObject root = jsonAt(jsonPath);
    CHECK(root.contains(QStringLiteral("config")));
    CHECK(root.value(QStringLiteral("config")).isObject());
    CHECK(!root.contains(QStringLiteral("config/theme")));
    // 没串到同名 .ini 上
    CHECK(!QFile::exists(QDir(jsonDir).filePath(QStringLiteral("ui-config.ini"))));

    // 其余既有 API 在 JSON 后端上照常往返
    utils->setLanguage(QStringLiteral("en-US"));
    utils->setNavWidth(260);
    utils->setViewCollapsed(true);
    utils->setNavStyle(1);
    CHECK(utils->getLanguage() == QStringLiteral("en-US"));
    CHECK(utils->getNavWidth() == 260);
    CHECK(utils->getViewCollapsed() == true);
    CHECK(utils->getNavStyle() == 1);
    // 从未写过的键走各自的回落值
    CHECK(utils->getTranslationLanguage() == QStringLiteral("zh-CN"));

    // 盘上键名保持驼峰：json 后端注册为 Qt::CaseSensitive（vendor 自己的语义，宿主不得为迁就
    // 缓存去改它）。代价落在读侧——换大小写取不到，所以宿主 SettingsCache 的归一化必须与格式
    // 成对配置（Reader 的 keyCaseSensitivity 入参），不能无条件 toLower()。判据见 tst_configmerge。
    const QJsonObject config = jsonAt(jsonPath).value(QStringLiteral("config")).toObject();
    CHECK(config.value(QStringLiteral("language")).toString() == QStringLiteral("en-US"));
    CHECK(config.value(QStringLiteral("navWidth")).toInt() == 260);
    CHECK(config.value(QStringLiteral("viewCollapsed")).toBool() == true);
    CHECK(config.value(QStringLiteral("navStyle")).toInt() == 1);
    return failed;
}

// ---- 组 C：单例已建时的重建语义（同值不重建 / 换定位不迁移）----
int groupRebuild(FluConfigUtils* utils, const QString& jsonDir, const QString& jsonPath,
                 const QString& iniDir)
{
    int failed = 0;
    QSettings* before = utils->getSettings();
    CHECK(before != nullptr);

    // 同值重复注入：必须原地不动，否则交出去的裸指针会被 free 掉
    FluConfigUtils::configure(jsonDir, QStringLiteral("ui-config"), FluConfigFormat::Json);
    CHECK(utils->getSettings() == before);
    CHECK(FluConfigUtils::configFilePath() == jsonPath);

    // 换到 ini 定位：后端换了对象，落点也换了
    FluConfigUtils::configure(iniDir, QStringLiteral("ui-config"), FluConfigFormat::Ini);
    const QString iniPath = FluConfigUtils::configFilePath();
    CHECK(iniPath == QDir(iniDir).filePath(QStringLiteral("ui-config.ini")));
    CHECK(utils->getSettings() != before);
    CHECK(utils->getSettings()->fileName() == iniPath);

    // 换定位等于换文件：旧值留在原处、不迁移过来，所以这里读到的是回落值
    CHECK(utils->getTheme() == FluTheme::Light);
    utils->setTheme(FluTheme::Light);
    CHECK(QFile::exists(iniPath));
    CHECK(themeInFile(iniPath, FluConfigFormat::Ini) == QStringLiteral("Light"));
    // ini 后端仍是 [config] 分节文本，不是 JSON
    const QString iniText = readAll(iniPath);
    CHECK(iniText.contains(QStringLiteral("[config]")));
    CHECK(!iniText.contains(QStringLiteral("{")));

    // 旧的 JSON 文件原样躺在原处（内容仍是 Dark）
    CHECK(themeInFile(jsonPath, FluConfigFormat::Json) == QStringLiteral("Dark"));

    // 切回 JSON 定位：读回的是它自己那份旧数据，证明两段定位各自独立
    FluConfigUtils::configure(jsonDir, QStringLiteral("ui-config"), FluConfigFormat::Json);
    CHECK(utils->getTheme() == FluTheme::Dark);
    return failed;
}

// ---- 组 D：文件名带扩展名时由 format 强制决定 ----
int groupForcedSuffix(FluConfigUtils* utils, const QString& jsonDir)
{
    int failed = 0;
    // 注入一个 .ini 结尾的名字但选 Json：落点必须是 .json（且就是组 B 那份文件）
    FluConfigUtils::configure(jsonDir, QStringLiteral("ui-config.ini"), FluConfigFormat::Json);
    const QString path = FluConfigUtils::configFilePath();
    CHECK(path == QDir(jsonDir).filePath(QStringLiteral("ui-config.json")));
    CHECK(utils->getTheme() == FluTheme::Dark); // 读到的仍是同一份 JSON
    return failed;
}

// ---- 组 E：exportToIni 照常工作（导出的是当前后端的全部键）----
int groupExport(FluConfigUtils* utils, const QString& exportDir)
{
    int failed = 0;
    const QString outPath = QDir(exportDir).filePath(QStringLiteral("dumped.ini"));
    utils->exportToIni(outPath);
    CHECK(QFile::exists(outPath));
    const QString text = readAll(outPath);
    CHECK(text.contains(QStringLiteral("[config]")));
    CHECK(text.contains(QStringLiteral("theme=Dark")));
    CHECK(text.contains(QStringLiteral("language=en-US")));
    return failed;
}

} // namespace

int tst_fluconfigutils()
{
    int failed = 0;
    failed += groupDefaultsUntouched();

    const ScopedTempRoot root(SUITE);
    if (!root.isValid())
    {
        TestHarness::skip("SKIP %s: 需要 LITE_TEST_TMPROOT（须以 /tmp-test 结尾）\n", SUITE);
        return failed;
    }

    // 子目录自建：ScopedTempRoot 只给 root，落点必须绝对（相对路径会随 CWD 漂进仓库）
    const QString jsonDir = subDir(root.path(), QStringLiteral("json-home"));
    const QString iniDir = subDir(root.path(), QStringLiteral("ini-home"));
    const QString exportDir = subDir(root.path(), QStringLiteral("export"));

    FluConfigUtils* utils = nullptr;
    QString jsonPath;
    failed += groupJsonFirstSingleton(jsonDir, &utils, &jsonPath);
    if (utils == nullptr || jsonPath.isEmpty())
        return failed;
    failed += groupRebuild(utils, jsonDir, jsonPath, iniDir);
    failed += groupForcedSuffix(utils, jsonDir);
    failed += groupExport(utils, exportDir);
    return failed;
}
