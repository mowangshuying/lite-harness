// tst_updaterules.cpp —— 自更新判据纯头直测（src/UpdateRules.h，header-only 零 IO）
// 被测：版本解析与数值比较 / 发布资产择优 / digest 归一与比对 / zip 条目→安装树映射
//       / 换体计划与回滚顺序 / 可自更新性判定。
//
// 本套件为什么必须存在（不是凑覆盖率）：自更新是「一次做错、用户环境不可恢复」的操作，
// 而下面每条判据出错时程序都不会报错，只会安静地把环境搞坏：
//   · 版本比较若走字典序，`"s17.9" > "s17.11"` 为真——补丁位形态是本仓既定事实
//     （s17.10、s17.11 已发布），于是 17.11 的用户永远看不到 17.12 之后，或反向误判把
//     旧包当新版换上去。
//   · zip 条目映射不设白名单：一个损坏或恶意的包写 `../config/config.json` 就能直接毁掉
//     数据根（会话/记忆/设置全没）。实测发布 zip 里确实不含 config/，但「不在包里」
//     不等于「进不来」，拒绝必须发生在映射层而不是指望打包方。
//   · digest 缺失时若放行：等于「没校验也照样换体」，校验和形同虚设。
//   · 包内根目录名硬编码：它带版本号随每次发布变化，写死等于下次更新必然解错路径。
//   · 回滚/清理顺序不确定：半新半死的安装态无法预期，计划必须可重放。
// 这些在冒烟里全都看不出来（点一下「更新」照样转圈、照样报成功），只能逐条断言。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT）：所有盘上事实以值或清单传入，本头零 IO，
// 任何环境全量执行。网络下载、tar 解压、改名换体、重启交接属 IO 侧
// （UpdateManager.cpp 链 Qt6::Network 因而进不了 ctest），验证手段是编译期等价 + 冒烟。

#include "TestHarness.h"

#include "UpdateRules.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace {

using TestHarness::check; // 判据密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// 真实包形态的样本条目清单（与 cpack 产物实测结构同形：单一版本化根 + bin/plugins/translations）
const char *kPkgRoot = "lite-harness-s17.11-win64";

QStringList sampleEntries()
{
    return {
        QStringLiteral("lite-harness-s17.11-win64/bin/lite-harness.exe"),
        QStringLiteral("lite-harness-s17.11-win64/bin/Qt6Core.dll"),
        QStringLiteral("lite-harness-s17.11-win64/bin/qt.conf"),
        QStringLiteral("lite-harness-s17.11-win64/plugins/platforms/qwindows.dll"),
        QStringLiteral("lite-harness-s17.11-win64/translations/qtbase_zh_CN.qm"),
    };
}

} // namespace

int tst_updaterules()
{
    // 返回失败增量而非裸 0：check 只累全局计数，硬写 return 0 会让 SUITE 行报 fail=0
    // 而进程退出码非零（本仓 main.cpp 的一致性门禁正是为此而设）。
    const int failBefore = TestHarness::failCount();

    // ---- ① 版本解析：认一段（s18 起）/「主.补丁」两段/「主.补丁.修订」三段，形态不符一律拒 ----
    int major = 0, patch = 0, micro = 0;
    check(UpdateRules::parseVersion(QStringLiteral("s17.11"), &major, &patch),
          "parseVersion: s-prefixed two-segment parses");
    check(major == 17 && patch == 11, "parseVersion: s17.11 -> (17,11)");
    check(UpdateRules::parseVersion(QStringLiteral("17.9"), &major, &patch) && major == 17 && patch == 9,
          "parseVersion: bare numeric form parses too");
    check(UpdateRules::parseVersion(QStringLiteral("v17.10"), &major, &patch) && patch == 10,
          "parseVersion: legacy v-prefix accepted (tag history v0.1.0)");
    check(UpdateRules::parseVersion(QStringLiteral("  s17.12  "), &major, &patch) && patch == 12,
          "parseVersion: surrounding whitespace trimmed");
    check(!UpdateRules::parseVersion(QString(), &major, &patch), "parseVersion: empty rejected");
    check(UpdateRules::parseVersion(QStringLiteral("s17"), &major, &patch) && major == 17 && patch == 0,
          "parseVersion: single segment parses since s18 (missing patch zero-padded, never rejected)");
    check(!UpdateRules::parseVersion(QStringLiteral("17.11.2"), &major, &patch),
          "parseVersion: a third segment is refused when the caller only asks for two, never truncated");
    check(UpdateRules::parseVersion(QStringLiteral("s17.13.1"), &major, &patch, &micro)
              && major == 17 && patch == 13 && micro == 1,
          "parseVersion: three-segment form parses once the caller asks for the third segment");
    check(UpdateRules::parseVersion(QStringLiteral("s17.13"), &major, &patch, &micro)
              && micro == 0,
          "parseVersion: two-segment form zero-pads the third segment (s17.13 == s17.13.0)");
    check(!UpdateRules::parseVersion(QStringLiteral("s17.13.x"), &major, &patch, &micro),
          "parseVersion: non-numeric third segment rejected");
    check(!UpdateRules::parseVersion(QStringLiteral("s17.13.1.2"), &major, &patch, &micro),
          "parseVersion: four segments rejected - the shipped shape is one to three");
    check(!UpdateRules::parseVersion(QStringLiteral("17.11.2"), &major, nullptr, &micro),
          "parseVersion: null out pointer rejected even on the three-segment overload");
    check(!UpdateRules::parseVersion(QStringLiteral("s17.x"), &major, &patch),
          "parseVersion: non-numeric patch rejected");
    check(!UpdateRules::parseVersion(QStringLiteral("+17.11"), &major, &patch),
          "parseVersion: toInt's tolerance for a leading sign is NOT inherited");
    check(UpdateRules::parseVersion(QStringLiteral("17.11 "), &major, &patch) && patch == 11,
          "parseVersion: trailing space tolerated (outer trim), body still two clean segments");
    check(!UpdateRules::parseVersion(QStringLiteral("s17.11"), nullptr, &patch),
          "parseVersion: null out pointer rejected, not dereferenced");
    // 一段形态（s18 起启用）：阶段 tag s13–s17 本就单段，但那时自更新模块尚未存在；
    // s18 起模块必须认得自身版本，否则 isUpdateAvailable 恒 false、静默断掉更新通道。
    // 一段没有可截断的尾段，故两指出参也接受（与三段的宽度规则不冲突）。
    check(UpdateRules::parseVersion(QStringLiteral("s18"), &major, &patch, &micro)
              && major == 18 && patch == 0 && micro == 0,
          "parseVersion: s18 -> (18,0,0) with all three out params");
    check(UpdateRules::parseVersion(QStringLiteral("18"), &major, &patch) && major == 18 && patch == 0,
          "parseVersion: bare single segment parses");
    check(UpdateRules::parseVersion(QStringLiteral("v18"), &major, &patch) && major == 18,
          "parseVersion: v-prefixed single segment parses");
    check(UpdateRules::parseVersion(QStringLiteral("  s18  "), &major, &patch) && major == 18,
          "parseVersion: single segment trimmed like the other shapes");
    check(!UpdateRules::parseVersion(QStringLiteral("s1a"), &major, &patch),
          "parseVersion: non-numeric single segment rejected");
    check(!UpdateRules::parseVersion(QStringLiteral("s"), &major, &patch),
          "parseVersion: prefix alone leaves zero segments, rejected");
    check(!UpdateRules::parseVersion(QStringLiteral("+18"), &major, &patch),
          "parseVersion: leading sign rejected on single segment too");
    check(!UpdateRules::parseVersion(QStringLiteral("s18"), nullptr, &patch),
          "parseVersion: null major out pointer rejected on single segment");

    // ---- ② 数值比较：字典序会在这里骗人，必须钉死 ----
    // 本仓真实发布链：s17.9 -> s17.10 -> s17.11。字典序认为 "17.9" > "17.10"，
    // 于是老用户会被判成「已是最新」而永远收不到更新，或反向被降级。
    check(UpdateRules::compareVersions(QStringLiteral("s17.10"), QStringLiteral("s17.9")) > 0,
          "compareVersions: 17.10 > 17.9 (lexicographic order would say the opposite)");
    check(UpdateRules::compareVersions(QStringLiteral("s17.11"), QStringLiteral("s17.12")) < 0,
          "compareVersions: 17.11 < 17.12");
    check(UpdateRules::compareVersions(QStringLiteral("s17.11"), QStringLiteral("s17.11")) == 0,
          "compareVersions: identical versions compare equal");
    check(UpdateRules::compareVersions(QStringLiteral("s18.0"), QStringLiteral("s17.99")) > 0,
          "compareVersions: major dominates patch");
    check(UpdateRules::compareVersions(QStringLiteral("bad"), QStringLiteral("s17.11")) == 0,
          "compareVersions: unparsable input yields 0 (treated as not-an-update)");
    // 三段形态（s17.13.1 起启用）。这三条钉的是「补丁位与修订位谁大」——
    // 若只比前两段，s17.13.1 会被判成与 s17.13 相等，刚发的补丁版当场消失。
    check(UpdateRules::compareVersions(QStringLiteral("s17.13.1"), QStringLiteral("s17.13")) > 0,
          "compareVersions: s17.13.1 > s17.13 (the third segment must participate, not be ignored)");
    check(UpdateRules::compareVersions(QStringLiteral("s17.13.1"), QStringLiteral("s17.14")) < 0,
          "compareVersions: the patch segment still outranks the revision segment (s17.13.1 < s17.14)");
    check(UpdateRules::compareVersions(QStringLiteral("s17.13.10"), QStringLiteral("s17.13.9")) > 0,
          "compareVersions: third segment compares numerically, not lexicographically");
    check(UpdateRules::compareVersions(QStringLiteral("s17.13"), QStringLiteral("s17.13.0")) == 0,
          "compareVersions: a missing third segment is zero, so s17.13 == s17.13.0");
    // 一段（s18）：缺段按 0 计的跨形态等值，与跨阶段量级。
    check(UpdateRules::compareVersions(QStringLiteral("s18"), QStringLiteral("s17.13.1")) > 0,
          "compareVersions: s18 > s17.13.1 (a new stage outranks any patch/revision)");
    check(UpdateRules::compareVersions(QStringLiteral("s18"), QStringLiteral("s18.0")) == 0,
          "compareVersions: s18 == s18.0 (missing patch is zero)");
    check(UpdateRules::compareVersions(QStringLiteral("s18"), QStringLiteral("s18.0.0")) == 0,
          "compareVersions: s18 == s18.0.0 (both missing segments are zero)");
    check(UpdateRules::compareVersions(QStringLiteral("s18"), QStringLiteral("s18.1")) < 0,
          "compareVersions: s18 < s18.1 (the patch segment still participates)");
    check(UpdateRules::compareVersions(QStringLiteral("s9"), QStringLiteral("s10")) < 0,
          "compareVersions: single segments compare numerically, not lexicographically");

    // ---- ③ 是否该更新：宁可不更新，也不基于垃圾串换体 ----
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s17.11"), QStringLiteral("s17.12")),
          "isUpdateAvailable: newer remote tag offers an update");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s17.11"), QStringLiteral("s17.11")),
          "isUpdateAvailable: same version is not an update");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s17.12"), QStringLiteral("s17.11")),
          "isUpdateAvailable: older remote never downgrades");
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s17.9"), QStringLiteral("s17.10")),
          "isUpdateAvailable: 17.9 -> 17.10 IS an update (the patch-digit trap)");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s17.11"), QStringLiteral("nightly")),
          "isUpdateAvailable: unparsable remote tag refuses to update");
    check(!UpdateRules::isUpdateAvailable(QString(), QStringLiteral("s17.12")),
          "isUpdateAvailable: unparsable local version refuses to update");
    // 三段：这两条是「发 s17.13.1」能不能被看见的正反两面。
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s17.13"), QStringLiteral("s17.13.1")),
          "isUpdateAvailable: s17.13 -> s17.13.1 IS an update (the reason three segments exist)");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s17.13.1"), QStringLiteral("s17.13")),
          "isUpdateAvailable: s17.13.1 never downgrades to s17.13");
    // 一段：「发 s18」能否被看见的正反两面，与装上 s18 后的自等价。
    // 注意方向：s17.13.1 实例的旧判据不认单段远端串，应用内看不到 s18（须手动升级）；
    // 这里钉的是**放宽后**的判据（s18 起实例之间、以及新判据回看旧版）的行为。
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s17.13.1"), QStringLiteral("s18")),
          "isUpdateAvailable: s17.13.1 -> s18 IS an update once one segment is accepted");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s18"), QStringLiteral("s17.13.1")),
          "isUpdateAvailable: s18 never downgrades to s17.13.1");
    check(!UpdateRules::isUpdateAvailable(QStringLiteral("s18"), QStringLiteral("s18.0")),
          "isUpdateAvailable: s18 vs s18.0 is equality, not an update");
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s18"), QStringLiteral("s18.1")),
          "isUpdateAvailable: s18 -> s18.1 IS an update");
    check(UpdateRules::isUpdateAvailable(QStringLiteral("s17.13.1"), QStringLiteral("s17.13.2")),
          "isUpdateAvailable: s17.13.1 -> s17.13.2 offers the next revision");

    // ---- ④ 资产择优：错平台的包一旦下下来，校验前就已经开始动手 ----
    const QStringList assets = {
        QStringLiteral("lite-harness-s17.12-darwin.tar.gz"),
        QStringLiteral("lite-harness-s17.12-win64.zip"),
        QStringLiteral("lite-harness-s17.12-linux-x64.tar.gz"),
    };
    check(UpdateRules::pickAssetName(assets, QStringLiteral("s17.12"))
              == QStringLiteral("lite-harness-s17.12-win64.zip"),
          "pickAssetName: selects the win64 zip among other platforms");
    check(UpdateRules::pickAssetName(assets, QString())
              == QStringLiteral("lite-harness-s17.12-win64.zip"),
          "pickAssetName: empty tag still filters by -win64.zip suffix");
    check(UpdateRules::pickAssetName(assets, QStringLiteral("s17.11")).isEmpty(),
          "pickAssetName: tag mismatch yields no asset rather than a wrong-version download");
    check(UpdateRules::pickAssetName(
              {QStringLiteral("lite-harness-s17.12.zip")}, QString()).isEmpty(),
          "pickAssetName: a zip without the -win64 suffix is not accepted");
    const QStringList dup = {
        QStringLiteral("lite-harness-s17.12-win64.zip"),
        QStringLiteral("lite-harness-s17.11-win64.zip"),
    };
    check(UpdateRules::pickAssetName(dup, QString())
              == UpdateRules::pickAssetName(dup, QString()),
          "pickAssetName: deterministic across repeated calls");
    check(UpdateRules::pickAssetName(dup, QString())
              == QStringLiteral("lite-harness-s17.11-win64.zip"),
          "pickAssetName: multiple hits resolve to the sorted first (no API-order drift)");
    check(UpdateRules::pickAssetName(QStringList(), QStringLiteral("s17.12")).isEmpty(),
          "pickAssetName: empty asset list yields nothing");

    // ---- ⑤ digest：缺失/形态错一律当「不可信」，绝不因缺而降级放行 ----
    const QString good = QStringLiteral("sha256:") + QString(64, QLatin1Char('a'));
    check(UpdateRules::normalizeDigest(good) == QString(64, QLatin1Char('a')),
          "normalizeDigest: strips the sha256: prefix");
    check(UpdateRules::normalizeDigest(QStringLiteral("SHA256:") + QString(64, QLatin1Char('A')))
              == QString(64, QLatin1Char('a')),
          "normalizeDigest: lowercases hex (API casing is not a contract)");
    check(UpdateRules::normalizeDigest(QString(64, QLatin1Char('b'))) == QString(64, QLatin1Char('b')),
          "normalizeDigest: bare hex without prefix accepted");
    check(UpdateRules::normalizeDigest(QStringLiteral("sha256:") + QString(63, QLatin1Char('a'))).isEmpty(),
          "normalizeDigest: 63 chars is not a sha256");
    check(UpdateRules::normalizeDigest(QStringLiteral("sha256:") + QString(64, QLatin1Char('g'))).isEmpty(),
          "normalizeDigest: non-hex characters rejected");
    check(UpdateRules::normalizeDigest(QString()).isEmpty(), "normalizeDigest: empty stays empty");
    check(UpdateRules::normalizeDigest(QStringLiteral("sha512:") + QString(128, QLatin1Char('c'))).isEmpty(),
          "normalizeDigest: a sha512 digest is not silently reinterpreted as sha256");

    const QString hex = QString(64, QLatin1Char('d'));
    check(UpdateRules::digestMatches(hex, hex), "digestMatches: equal digests pass");
    check(UpdateRules::digestMatches(hex.toUpper(), hex), "digestMatches: comparison is case-insensitive");
    check(!UpdateRules::digestMatches(hex, QString(64, QLatin1Char('e'))),
          "digestMatches: different digests fail");
    check(!UpdateRules::digestMatches(QString(), hex),
          "digestMatches: empty computed digest never passes (fail-closed)");
    check(!UpdateRules::digestMatches(hex, QString()),
          "digestMatches: missing API digest never passes -- no digest means no swap");

    // ---- ⑥ zip 条目归一：zip-slip 与绝对路径在这里挡掉 ----
    check(UpdateRules::normalizeEntryPath(QStringLiteral("a/b/c.dll")) == QStringLiteral("a/b/c.dll"),
          "normalizeEntryPath: plain path passes through");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("a\\b\\c.dll")) == QStringLiteral("a/b/c.dll"),
          "normalizeEntryPath: backslashes normalized (tar and zip disagree)");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("./a/b.dll")) == QStringLiteral("a/b.dll"),
          "normalizeEntryPath: leading ./ stripped");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("../../etc/passwd")).isEmpty(),
          "normalizeEntryPath: parent traversal rejected");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("pkg/bin/../../config/config.json")).isEmpty(),
          "normalizeEntryPath: traversal buried mid-path rejected (not just prefix-checked)");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("/abs/path.dll")).isEmpty(),
          "normalizeEntryPath: absolute path rejected");
    check(UpdateRules::normalizeEntryPath(QStringLiteral("C:/Windows/x.dll")).isEmpty(),
          "normalizeEntryPath: drive-letter path rejected");
    check(UpdateRules::normalizeEntryPath(QString()).isEmpty(), "normalizeEntryPath: empty rejected");

    // ---- ⑦ 包内根推导：随版本变化的目录名绝不能写死 ----
    check(UpdateRules::derivePackageRoot(sampleEntries()) == QLatin1String(kPkgRoot),
          "derivePackageRoot: single versioned root discovered from the listing");
    check(UpdateRules::derivePackageRoot({QStringLiteral("rootA/bin/a.exe"),
                                          QStringLiteral("rootB/bin/b.exe")})
              .isEmpty(),
          "derivePackageRoot: multiple roots rejected (malformed package)");
    check(UpdateRules::derivePackageRoot({QStringLiteral("lite-harness.exe")}).isEmpty(),
          "derivePackageRoot: bare file at archive root is not our package shape");
    check(UpdateRules::derivePackageRoot({QStringLiteral("root/bin/../config/x.json")}).isEmpty(),
          "derivePackageRoot: a traversal entry poisons the root derivation too");
    check(UpdateRules::derivePackageRoot(QStringList()).isEmpty(),
          "derivePackageRoot: empty listing yields no root");

    // ---- ⑧ 映射到安装树：数据根 config/ 与 log/ 必须被白名单挡在外面 ----
    const QString root = QLatin1String(kPkgRoot);
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/bin/lite-harness.exe"), root)
              == QStringLiteral("bin/lite-harness.exe"),
          "mapEntryToInstallRel: bin payload maps under the install root");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/plugins/platforms/qwindows.dll"), root)
              == QStringLiteral("plugins/platforms/qwindows.dll"),
          "mapEntryToInstallRel: nested plugin path preserved");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/config/config.json"), root).isEmpty(),
          "mapEntryToInstallRel: config/ (the data root) is REFUSED -- sessions and settings die otherwise");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/log/whatever.log"), root).isEmpty(),
          "mapEntryToInstallRel: log/ is refused");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/README.md"), root).isEmpty(),
          "mapEntryToInstallRel: a file directly under the package root is refused");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/bin/"), root).isEmpty(),
          "mapEntryToInstallRel: directory entries are refused (swap only moves files)");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("otherRoot/bin/a.exe"), root).isEmpty(),
          "mapEntryToInstallRel: entries outside the derived root are refused");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/bin/../config/config.json"), root).isEmpty(),
          "mapEntryToInstallRel: traversal aiming at the data root is refused");
    check(UpdateRules::mapEntryToInstallRel(
              QStringLiteral("lite-harness-s17.11-win64/bin/a.exe"), QString()).isEmpty(),
          "mapEntryToInstallRel: empty package root refuses everything");
    // 白名单本身是判据：数据根不在名单里，且新增目录不会意外进名单
    check(!UpdateRules::replaceableTopDirs().contains(QStringLiteral("config")),
          "replaceableTopDirs: config is absent (allowlist, not denylist)");
    check(UpdateRules::replaceableTopDirs().contains(QStringLiteral("bin")),
          "replaceableTopDirs: bin present");

    // ---- ⑧b 目录条目判定：必须在归一化之前问 ----
    check(UpdateRules::isDirectoryEntry(QStringLiteral("root/bin/")),
          "isDirectoryEntry: trailing slash marks a directory");
    check(UpdateRules::isDirectoryEntry(QStringLiteral("root/bin\\")),
          "isDirectoryEntry: backslash form too (tar and zip disagree on separators)");
    check(!UpdateRules::isDirectoryEntry(QStringLiteral("root/bin/x.exe")),
          "isDirectoryEntry: a file is not a directory");
    check(!UpdateRules::isDirectoryEntry(QString()), "isDirectoryEntry: empty is not a directory");
    // 归一化会吃掉尾斜杠，所以目录判定不能放在归一之后——否则正常包被判成坏包
    check(UpdateRules::normalizeEntryPath(QStringLiteral("root/bin/")) == QStringLiteral("root/bin"),
          "normalizeEntryPath: trailing slash is dropped, which is why isDirectoryEntry reads the RAW entry");

    // ---- ⑨ 换体计划：确定性、去重、备份命名、回滚逆序、一条不落白名单即整批作废 ----
    const QStringList pkgEntries = {
        QStringLiteral("lite-harness-s17.11-win64/bin/lite-harness.exe"),
        QStringLiteral("lite-harness-s17.11-win64/bin/Qt6Core.dll"),
        QStringLiteral("lite-harness-s17.11-win64/bin/"), // 目录条目：应被跳过而非报错
        QStringLiteral("lite-harness-s17.11-win64/bin/lite-harness.exe"), // 重复项
    };
    const QVector<UpdateRules::SwapItem> plan = UpdateRules::planSwap(pkgEntries, root);
    check(plan.size() == 2, "planSwap: directory entries skipped and duplicates collapsed");
    check(plan.at(0).liveTarget == QStringLiteral("bin/lite-harness.exe"),
          "planSwap: live target is install-relative (package root stripped)");
    check(plan.at(0).stagedSource == QStringLiteral("lite-harness-s17.11-win64/bin/lite-harness.exe"),
          "planSwap: staged source keeps the package-relative path (it lives under the staging root)");
    check(plan.at(0).stagedSource != plan.at(0).liveTarget,
          "planSwap: staged and live paths are deliberately different roots (confusing them copies nothing)");
    check(plan.at(0).backupPath == QStringLiteral("bin/lite-harness.exe.update-backup"),
          "planSwap: backup is a sibling name (same directory, so rename stays on one volume)");
    check(plan.at(0).backupPath == plan.at(0).liveTarget + UpdateRules::backupSuffix(),
          "planSwap: backup naming goes through the single-sourced suffix, not a literal");
    check(UpdateRules::planSwap(QStringList(), root).isEmpty(), "planSwap: empty input yields empty plan");
    check(UpdateRules::planSwap(pkgEntries, QString()).isEmpty(),
          "planSwap: a missing package root yields no plan rather than a mis-rooted one");
    // 半份计划换体 = 一部分新、一部分旧，比不更新糟糕得多：一条不落白名单就整批拒绝
    QStringList poisoned = pkgEntries;
    poisoned.append(QStringLiteral("lite-harness-s17.11-win64/config/config.json"));
    check(UpdateRules::planSwap(poisoned, root).isEmpty(),
          "planSwap: ONE entry outside the allowlist voids the WHOLE plan (never a partial swap)");
    QStringList traversal = { QStringLiteral("lite-harness-s17.11-win64/bin/../../config/x.json") };
    check(UpdateRules::planSwap(traversal, root).isEmpty(),
          "planSwap: a traversal entry voids the plan instead of being skipped");
    QStringList foreignRoot = { QStringLiteral("otherBuild/bin/x.exe") };
    check(UpdateRules::planSwap(foreignRoot, root).isEmpty(),
          "planSwap: entries under a different package root void the plan");

    const QStringList roll = UpdateRules::rollbackOrder(plan);
    check(roll.size() == 2, "rollbackOrder: one entry per planned item");
    check(roll.at(0) == QStringLiteral("bin/Qt6Core.dll.update-backup"),
          "rollbackOrder: reverses the plan (last renamed is restored first)");
    check(roll.at(1) == QStringLiteral("bin/lite-harness.exe.update-backup"),
          "rollbackOrder: full reverse order");

    const QStringList clean = UpdateRules::cleanupOrder(plan);
    check(clean.size() == 2 && clean.at(0) == QStringLiteral("bin/lite-harness.exe.update-backup"),
          "cleanupOrder: lists backups in forward order for the next instance to delete");
    check(UpdateRules::rollbackOrder(plan) != UpdateRules::cleanupOrder(plan),
          "rollback and cleanup orders are distinct (rollback reverses, cleanup does not)");

    // ---- ⑩ 可自更新性：开发态必须拒绝，否则发布包会盖进构建树 ----
    check(UpdateRules::canSelfUpdate(true, true, true),
          "canSelfUpdate: release layout with writable root is allowed");
    check(!UpdateRules::canSelfUpdate(false, true, true),
          "canSelfUpdate: dev layout (no Qt runtime beside exe) is refused");
    check(!UpdateRules::canSelfUpdate(true, false, true),
          "canSelfUpdate: missing config root is refused (data root location unknown)");
    check(!UpdateRules::canSelfUpdate(true, true, false),
          "canSelfUpdate: read-only install is refused");
    check(!UpdateRules::canSelfUpdate(false, false, false),
          "canSelfUpdate: all-false is refused");
    check(UpdateRules::refusalReason(true, true, true).isEmpty(),
          "refusalReason: allowed layout reports no reason");
    check(UpdateRules::refusalReason(false, true, true) == QStringLiteral("dev_layout"),
          "refusalReason: dev layout names itself (UI must not say a generic failure)");
    check(UpdateRules::refusalReason(true, false, true) == QStringLiteral("no_config_root"),
          "refusalReason: missing config root names itself");
    check(UpdateRules::refusalReason(true, true, false) == QStringLiteral("read_only_install"),
          "refusalReason: read-only install names itself");
    // 多条同时不满足时报第一条：顺序即优先级，dev_layout 最先（开发态是最常见误触面）
    check(UpdateRules::refusalReason(false, false, false) == QStringLiteral("dev_layout"),
          "refusalReason: precedence is dev_layout first");

    // ---- ⑩a 残留清理的版本闸门：备份是旧版唯一副本，错删等于自毁 ----
    check(UpdateRules::shouldCleanResidue(QStringLiteral("s17.12"), QStringLiteral("s17.12")),
          "shouldCleanResidue: same version means the swap took effect, residue is safe to delete");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.12"), QStringLiteral("s17.11")),
          "shouldCleanResidue: version mismatch NEVER deletes backups (they are the only working copy)");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.12"), QStringLiteral("s18.0")),
          "shouldCleanResidue: a newer running version is also a mismatch, not 'close enough'");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.9"), QStringLiteral("s17.10")),
          "shouldCleanResidue: patch-digit trap applies here too (no string equality shortcut)");
    check(!UpdateRules::shouldCleanResidue(QString(), QStringLiteral("s17.12")),
          "shouldCleanResidue: missing marker version refuses to delete");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.12"), QString()),
          "shouldCleanResidue: missing running version refuses to delete");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("garbage"), QStringLiteral("garbage")),
          "shouldCleanResidue: two identical garbage strings do not count as a match");
    // 三段：这条是发 s17.13.1 之后最贵的一条。换到 .1 没生效时，备份是旧版可执行体的
    // 唯一副本——若闸门只比前两段，`s17.13.1` 与 `s17.13` 会被判成同一个版本而把副本删掉。
    check(UpdateRules::shouldCleanResidue(QStringLiteral("s17.13.1"), QStringLiteral("s17.13.1")),
          "shouldCleanResidue: three-segment match cleans residue");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.13.1"), QStringLiteral("s17.13")),
          "shouldCleanResidue: the third segment participates - s17.13 running is NOT the s17.13.1 swap");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s17.13"), QStringLiteral("s17.13.1")),
          "shouldCleanResidue: and the other direction is a mismatch too");
    check(UpdateRules::shouldCleanResidue(QStringLiteral("s17.13"), QStringLiteral("s17.13.0")),
          "shouldCleanResidue: a zero third segment matches the two-segment spelling");
    // 一段（s18）：闸门等值与跨形态补零，补丁位仍参与。
    check(UpdateRules::shouldCleanResidue(QStringLiteral("s18"), QStringLiteral("s18")),
          "shouldCleanResidue: single-segment match cleans residue");
    check(UpdateRules::shouldCleanResidue(QStringLiteral("s18"), QStringLiteral("s18.0")),
          "shouldCleanResidue: s18.0 running matches the single-segment s18 swap");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s18"), QStringLiteral("s18.1")),
          "shouldCleanResidue: s18.1 running is NOT the s18 swap (patch participates)");
    check(!UpdateRules::shouldCleanResidue(QStringLiteral("s18.1"), QStringLiteral("s18")),
          "shouldCleanResidue: and the other direction is a mismatch too");

    // ---- ⑩b 端点字符串：拼错的后果是「静默没有更新」，UI 上看不出来 ----
    check(UpdateRules::releaseApiUrl(QStringLiteral("mowangshuying/lite-harness"))
              == QStringLiteral("https://api.github.com/repos/mowangshuying/lite-harness/releases/latest"),
          "releaseApiUrl: canonical Releases endpoint for the shipped slug");
    check(UpdateRules::releaseApiUrl(QStringLiteral("  mowangshuying/lite-harness  "))
              == UpdateRules::releaseApiUrl(QStringLiteral("mowangshuying/lite-harness")),
          "releaseApiUrl: surrounding whitespace does not change the endpoint");
    check(UpdateRules::releaseApiUrl(QStringLiteral("mowangshuying")).isEmpty(),
          "releaseApiUrl: a slug without owner/repo yields NO url rather than a guaranteed-404 one");
    check(UpdateRules::releaseApiUrl(QString()).isEmpty(), "releaseApiUrl: empty slug yields no url");
    check(!UpdateRules::releaseApiUrl(QStringLiteral("a/b/c")).isEmpty(),
          "releaseApiUrl: a slug containing slashes is passed through (owner/repo is the contract)");

    // GitHub REST API 缺 User-Agent 一律拒答——本机实测：不带 UA 失败、带 UA 返回 200。
    // 而这个失败在 UI 上与「网络不通」「已是最新」完全同形，所以 UA 是判据不是风格。
    const QString ua = UpdateRules::userAgentFor(QStringLiteral("s17.11"));
    check(ua.startsWith(QStringLiteral("lite-harness/")), "userAgentFor: identifies the product");
    check(ua.contains(QStringLiteral("s17.11")), "userAgentFor: carries the running version (support asks for it)");
    check(UpdateRules::userAgentFor(QString()).contains(QStringLiteral("unknown")),
          "userAgentFor: empty version degrades to a literal 'unknown', never an empty UA");
    check(UpdateRules::userAgentFor(QString()).contains(QStringLiteral("lite-harness/")),
          "userAgentFor: still non-empty and product-prefixed without a version");

    check(UpdateRules::assetSizeOrUnknown(55858237) == 55858237,
          "assetSizeOrUnknown: a declared size passes through (progress denominator)");
    check(UpdateRules::assetSizeOrUnknown(0) == -1 && UpdateRules::assetSizeOrUnknown(-5) == -1,
          "assetSizeOrUnknown: a missing size becomes -1, never 0 (0 would render a 100% instant progress)");

    // ---- ⑩-b 进度分母判据（环与说明位文案共用，s17.12）----
    // 这条判据出错时不抛异常、不返回失败，只是把一个还在下的包显示成「已下完」或反过来。
    check(UpdateRules::progressPercentOrUnknown(50, 100) == 50,
          "progressPercentOrUnknown: a known denominator yields the plain percentage");
    check(UpdateRules::progressPercentOrUnknown(0, 100) == 0,
          "progressPercentOrUnknown: zero bytes of a known total is a real 0%, not 'unknown'");
    check(UpdateRules::progressPercentOrUnknown(100, 100) == 100,
          "progressPercentOrUnknown: a finished download reads exactly 100");
    // 未知分母必须走 -1 出口。这是 assetSizeOrUnknown 刻意留 -1 而不是 0 的全部理由：
    // 拿 0 当分母要么除零、要么被钳成 100%，而 100% 会让用户以为已经下完。
    check(UpdateRules::progressPercentOrUnknown(1024, -1) == -1,
          "progressPercentOrUnknown: an unknown denominator (-1) is never a percentage");
    check(UpdateRules::progressPercentOrUnknown(1024, 0) == -1,
          "progressPercentOrUnknown: a zero denominator is refused, not divided by");
    check(UpdateRules::progressPercentOrUnknown(-1, 100) == -1,
          "progressPercentOrUnknown: a negative received is 'unknown', never a negative ring");
    // received > total 在真实链路里会发生（Content-Length 中途变化、多算了头部字节），
    // 137% 画在环上是几何无意义的（弧长溢出整圆）。
    check(UpdateRules::progressPercentOrUnknown(137, 100) == 100,
          "progressPercentOrUnknown: received beyond total clamps to 100, never an over-full arc");
    check(UpdateRules::progressPercentOrUnknown(9223372036854775807LL, 9223372036854775807LL) == 100,
          "progressPercentOrUnknown: INT64_MAX takes the clamp path, not the received * 100 overflow");
    check(UpdateRules::progressPercentOrUnknown(9223372036854775807LL, 1) == 100,
          "progressPercentOrUnknown: a huge received over a tiny total still clamps instead of wrapping");
    // 上面两条都落在 `received >= total` 的钳位分支里，**碰不到**真正危险的那条路：
    // `received < total` 且 received 超过 INT64_MAX/100 时，`received * 100` 是有符号溢出（UB）。
    // 这三条钉住分路本身，而不是钉住一个恰好安全的输入组合。
    check(UpdateRules::kPercentSafeMax == 9223372036854775807LL / 100,
          "kPercentSafeMax: the integer-path ceiling is INT64_MAX/100, single-sourced with the function body");
    check(UpdateRules::progressPercentOrUnknown(UpdateRules::kPercentSafeMax, 9223372036854775807LL) == 0,
          "progressPercentOrUnknown: exactly at the ceiling still takes the exact integer path");
    check(UpdateRules::progressPercentOrUnknown(UpdateRules::kPercentSafeMax + 1, 9223372036854775807LL) < 100,
          "progressPercentOrUnknown: one past the ceiling switches paths and still never reports a finished download");
    // 最该钉的一条：received = total-1（还差一个字节没下完）时，双精度舍入会把比值抬成整 100.0，
    // 于是「还差一个字节」显示成「已下完」——正是这条判据存在的全部理由所要防的事。
    // 浮点分支的再钳一次把这条路堵死：无论舍入往哪边，结果都必须是 99。
    check(UpdateRules::progressPercentOrUnknown(9223372036854775806LL, 9223372036854775807LL) == 99,
          "progressPercentOrUnknown: total-1 over total reads 99, never a rounding-made 100 (the fake 'done' state)");
    check(UpdateRules::progressPercentOrUnknown(9223372036854775806LL, 9223372036854775807LL)
              != UpdateRules::progressPercentOrUnknown(9223372036854775807LL, 9223372036854775807LL),
          "progressPercentOrUnknown: an unfinished download is distinguishable from a finished one at the top end");
    // 整数截断口径要钉死：渲染层若以为它是四舍五入，会在 99.6% 处显示 100% 而实际还在下。
    check(UpdateRules::progressPercentOrUnknown(999, 1000) == 99,
          "progressPercentOrUnknown: truncates toward zero, so a partial download never reads 100");
    check(UpdateRules::progressPercentOrUnknown(1, 3) == 33 && UpdateRules::progressPercentOrUnknown(2, 3) == 66,
          "progressPercentOrUnknown: thirds truncate consistently (no rounding drift between text and ring)");
    // 真实包体尺寸：55,858,237 字节是本机实测 CI 资产大小，中途值必须落在 (0,100) 内。
    check(UpdateRules::progressPercentOrUnknown(27929118, 55858237) == 49,
          "progressPercentOrUnknown: a real asset size yields a plausible mid-download percentage");
    // 环与文案同源的根据：同一个入参必须给出同一个数——两处各算一次迟早分叉。
    check(UpdateRules::progressPercentOrUnknown(27929118, 55858237)
              == UpdateRules::progressPercentOrUnknown(27929118, 55858237),
          "progressPercentOrUnknown: pure and repeatable (the ring and the status text read one source)");

    // ---- ⑪ 待切换标记：内容来自磁盘，后果是删文件，必须最严 ----
    check(UpdateRules::backupSuffix() == QStringLiteral(".update-backup"),
          "backupSuffix: the literal is single-sourced (write side and cleanup side must agree)");
    check(UpdateRules::isDeletableBackupPath(QStringLiteral("bin/lite-harness.exe.update-backup")),
          "isDeletableBackupPath: a bin backup under a real directory is deletable");
    check(UpdateRules::isDeletableBackupPath(QStringLiteral("plugins/platforms/qwindows.dll.update-backup")),
          "isDeletableBackupPath: nested plugin backup is deletable");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("config/config.json.update-backup")),
          "isDeletableBackupPath: a backup claiming to sit in the DATA ROOT is refused");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("log/app.log.update-backup")),
          "isDeletableBackupPath: log/ backups refused");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("bin/lite-harness.exe")),
          "isDeletableBackupPath: a live file without the backup suffix is never deleted");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("../outside.exe.update-backup")),
          "isDeletableBackupPath: traversal backup refused");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("lite-harness.exe.update-backup")),
          "isDeletableBackupPath: a bare filename with no top directory is refused");
    check(!UpdateRules::isDeletableBackupPath(QString()),
          "isDeletableBackupPath: empty path refused");
    check(!UpdateRules::isDeletableBackupPath(QStringLiteral("C:/bin/x.exe.update-backup")),
          "isDeletableBackupPath: absolute/drive form refused even with a valid suffix");

    // 往返：写入与解析必须成对，否则下次启动读不回自己写的 marker，残留永远删不掉
    QJsonObject markerJson;
    const QStringList writtenBackups = {
        QStringLiteral("bin/lite-harness.exe.update-backup"),
        QStringLiteral("bin/Qt6Core.dll.update-backup"),
    };
    UpdateRules::writeMarker(&markerJson, QStringLiteral("s17.11"), QStringLiteral("s17.12"),
                             QStringLiteral("lite-harness-s17.12-win64"), writtenBackups);
    const UpdateRules::Marker roundtrip = UpdateRules::parseMarker(markerJson);
    check(roundtrip.valid, "parseMarker: a freshly written marker round-trips as valid");
    check(roundtrip.backups == writtenBackups,
          "parseMarker: backup list survives the round trip in order (cleanup deletes exactly what swap made)");
    check(roundtrip.toVersion == QStringLiteral("s17.12"), "parseMarker: target version preserved");
    check(roundtrip.packageRoot == QStringLiteral("lite-harness-s17.12-win64"),
          "parseMarker: package root preserved");
    UpdateRules::writeMarker(nullptr, QStringLiteral("s17.11"), QStringLiteral("s17.12"),
                             QStringLiteral("r"), writtenBackups); // 空指针不得崩
    check(true, "writeMarker: null out pointer is ignored rather than dereferenced");

    // 三段 marker：发 s17.13.1 之后 marker 里存的就是三段串。schema 版本**故意不升**——
    // 已发布实例写的两段 marker 必须还能读（否则换体没生效时残留删不掉、备份被永久滞留），
    // 兼容性靠 parseVersion 同时接受两种形态，而不是靠放宽 schema 闸门。
    QJsonObject threeSegMarker;
    UpdateRules::writeMarker(&threeSegMarker, QStringLiteral("s17.13"), QStringLiteral("s17.13.1"),
                             QStringLiteral("lite-harness-s17.13.1-win64"), writtenBackups);
    const UpdateRules::Marker threeSeg = UpdateRules::parseMarker(threeSegMarker);
    check(threeSeg.valid,
          "parseMarker: a three-segment marker round-trips valid without bumping the schema");
    check(threeSeg.toVersion == QStringLiteral("s17.13.1"),
          "parseMarker: three-segment target version preserved verbatim");
    check(UpdateRules::shouldCleanResidue(threeSeg.toVersion, QStringLiteral("s17.13.1")),
          "parseMarker + residue gate: the marker written for s17.13.1 unlocks cleanup on s17.13.1");

    // 一段 marker（s18）：schema 同样不升——已发布实例写的两段/三段 marker 必须还能读，
    // 兼容性靠 parseVersion 同时接受各形态，而不是靠放宽 schema 闸门。
    QJsonObject oneSegMarker;
    UpdateRules::writeMarker(&oneSegMarker, QStringLiteral("s17.13.1"), QStringLiteral("s18"),
                             QStringLiteral("lite-harness-s18-win64"), writtenBackups);
    const UpdateRules::Marker oneSeg = UpdateRules::parseMarker(oneSegMarker);
    check(oneSeg.valid,
          "parseMarker: a single-segment marker round-trips valid without bumping the schema");
    check(oneSeg.toVersion == QStringLiteral("s18"),
          "parseMarker: single-segment target version preserved verbatim");
    check(UpdateRules::shouldCleanResidue(oneSeg.toVersion, QStringLiteral("s18")),
          "parseMarker + residue gate: the marker written for s18 unlocks cleanup on s18");

    // 作废路径：任一异常 → 整体 invalid（绝不部分照办）
    QJsonObject tampered = markerJson;
    QJsonArray badList = tampered.value(QStringLiteral("backups")).toArray();
    badList.append(QStringLiteral("config/config.json.update-backup")); // 夹带一条数据根路径
    tampered[QStringLiteral("backups")] = badList;
    check(!UpdateRules::parseMarker(tampered).valid,
          "parseMarker: ONE undeletable entry invalidates the WHOLE marker (never partially obeyed)");

    QJsonObject wrongSchema = markerJson;
    wrongSchema[QStringLiteral("schema")] = UpdateRules::markerSchemaVersion() + 1;
    check(!UpdateRules::parseMarker(wrongSchema).valid,
          "parseMarker: unknown schema version is refused, not guessed forward");

    QJsonObject badTo = markerJson;
    badTo[QStringLiteral("to")] = QStringLiteral("garbage");
    check(!UpdateRules::parseMarker(badTo).valid,
          "parseMarker: unparsable target version invalidates the marker");

    QJsonObject badFrom = markerJson;
    badFrom[QStringLiteral("from")] = QStringLiteral("not-a-version");
    check(!UpdateRules::parseMarker(badFrom).valid,
          "parseMarker: a present-but-malformed from version invalidates the marker");

    QJsonObject noBackups = markerJson;
    noBackups[QStringLiteral("backups")] = QJsonArray();
    check(!UpdateRules::parseMarker(noBackups).valid,
          "parseMarker: an empty backup list is anomalous, not 'nothing to clean'");

    check(!UpdateRules::parseMarker(QJsonObject()).valid,
          "parseMarker: absent/empty object is invalid (fail-closed on missing marker)");

    QJsonObject nonStringBackups = markerJson;
    QJsonArray numeric;
    numeric.append(123);
    nonStringBackups[QStringLiteral("backups")] = numeric;
    check(!UpdateRules::parseMarker(nonStringBackups).valid,
          "parseMarker: non-string backup entries are refused, not stringified into a path");

    return TestHarness::failCount() - failBefore;
}
