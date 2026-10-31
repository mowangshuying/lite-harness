// tst_settingscache.cpp —— 设置读缓存直测（src/SettingsCache.h，header-only 零 CMake 改动）
// 被测：Identity 三元组判据 / loadIsCommittable 装载可信判据 / Reader 的装载-命中-失效-写回四态
//
// 本套件的存在意义（§9.10 问题②整改的安全前提）：
// 整改把「每次取值现构造 QSettings」换成「按文件身份惰性重载的解析产物缓存」。缓存的失效
// 判据一旦出错，症状是**静默读不到新配置**——用户手改 config.json 或设置页写完再读，界面
// 显示旧值却不报错，属最难查的那类回归。README「运行时配置」节明确承诺「其余手改配置文件即可」，
// 这条承诺的全部技术依据就是本文件的组 3。
//
// 覆盖面边界（勿高估，与 §9.5 同口径）：
// · 本套件测的是**缓存层**（Reader + 两个纯判据），不是 AppSettings 的门面绑定——后者把路径
//   钉在 LitePaths::configFilePath()（存储根 <exe 目录>/../config 下的 config.json），测试里既造不出
//   也不该往真实存储根
//   写盘（ScopedTempRoot 禁的正是这类回退路径）。故 Reader 以「路径可注入」为设计前提，
//   这也是它被抽成独立纯头而非写进 AppSettings.h 的唯一理由。
// · 不变量⑤（装载失败不提交身份）在本套件里**只有判据级覆盖**（组 1b 穷举 loadIsCommittable），
//   没有行为级覆盖：造 AccessError 需要让文件存在但不可读，Windows 下靠 setPermissions 造这一格
//   并不可靠（属主仍可读、ACL 语义随机器策略变），拿它写断言就是造一条会随环境假红的用例。
//   行为级验证手段是「读失败保留旧表 + 下次重试」这条路径由代码结构保证（ensureLoaded 的
//   拒绝分支不写 m_cachedIdentity、不 m_cache.clear()），改动该结构时组 1b 的判据表先转红。
// · AgentConst 各取值函数的「范围校验/回退默认」语义不在本套件（属既有行为，本次未改）；
//   本套件只保证「缓存交出的 QVariant 与现取 QSettings 交出的那个逐键一致」——即本次改动的
//   全部风险面。
//
// 分组：组 1 零夹具恒跑（纯判据 + 缺失文件态）；组 2~6 需 LITE_TEST_TMPROOT，未配置则整组
// SKIP 并返回 0 失败（假绿由 LITE_TEST_MIN_PASS 门禁兜住）。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "SettingsCache.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

using SettingsCache::Identity;
using SettingsCache::Reader;

// 手搭身份（纯判据用例不需要真实文件）
Identity ident(bool exists, qint64 mtimeMs, qint64 sizeBytes)
{
    Identity identity;
    identity.exists = exists;
    identity.mtimeMs = mtimeMs;
    identity.sizeBytes = sizeBytes;
    return identity;
}

// 绕过 Reader 直接落盘（模拟「用户手改 config.json」：外部写手，缓存无从得知，
// 只能靠文件身份判据发现——正是组 3 要证的那件事）
bool writeIniExternally(const QString &path, const QString &key, const QVariant &value)
{
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(key, value);
    settings.sync();
    return settings.status() == QSettings::NoError && QFileInfo::exists(path);
}

// 现取一份新鲜 QSettings 的对应值（对照基准：缓存必须与它逐键一致，否则就是行为回归）
QVariant readIniDirectly(const QString &path, const QString &key)
{
    const QSettings settings(path, QSettings::IniFormat);
    return settings.value(key);
}

// 手写裸内容落盘（不经 QSettings 写侧）。这条通道是**必需**的，有两个用途：
// ① iniEscapedString（qsettings.cpp:592）见 `,` `;` `=` 就加引号，所以
//    `setValue("k","a,b,c")` 落盘成 `k="a,b,c"`，读回来是 QString 而非 QStringList——
//    「逗号串解析成列表」只在**人手编辑**无引号时才发生，要复现 textValue 防的那个
//    形态只能自己写文件。
// ② QSettings 写侧必然产出 `[General]` 段头，于是「无段头的裸 key=value」（用户手写
//    ini 的最常见形态）与「自定义 [Section]」两种形态用 QSettings 根本造不出来。
bool writeRawIni(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = file.write(content) == content.size();
    file.close();
    return written;
}

// 把 mtime 显式往前推 deltaMs（组 3 的等长改写靠它变成确定行为）。
// 为什么必需：等长改写只有 mtime 一个区分量，而「写完恰好跨过毫秒边界」只是**大概率**——
// 实测连续等长写入有相当比例落在同一毫秒（Δ=0），FAT32/exFAT 粒度 2 秒、部分网络盘 1 秒，
// 拿它当判据就是给 CI 埋一条随机假红。文件系统不支持设时间则返回 false，调用方 SKIP。
bool advanceMtime(const QString &path, qint64 deltaMs)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    const QDateTime current = file.fileTime(QFileDevice::FileModificationTime);
    const bool ok = current.isValid()
                    && file.setFileTime(current.addMSecs(deltaMs), QFileDevice::FileModificationTime);
    file.close();
    if (!ok)
        return false;
    // 回读确认：设了没生效（某些挂载点静默忽略）也不能让断言假绿
    return QFileInfo(path).lastModified() != current;
}

// ---- 组 1a：身份判据（零 IO，逐字段穷举）----
// 任一字段单独变化都必须判「变更」，全等才判「不变」——每一格对应一类漏检的失效场景。
// （原「判据对称」断言是 false==false 的同义反复，恒过、只稀释门禁灵敏度，已换成有内容的表。）
void testIdentityPredicate()
{
    struct Row
    {
        Identity a;
        Identity b;
        bool expected;
        const char *what;
    };
    const QVector<Row> rows{
        {ident(true, 1000, 200), ident(true, 1000, 200), true,
         "全等 → 不变（命中缓存的判据不能把自己判成变更）"},
        {ident(true, 1000, 200), ident(false, 1000, 200), false,
         "仅 exists 翻转 → 变更（文件从无到有必须重载，不变量②）"},
        {ident(true, 1000, 200), ident(true, 1001, 200), false,
         "仅 mtime 变化 → 变更（等长手改只能靠这一格捕获）"},
        {ident(true, 1000, 200), ident(true, 1000, 201), false,
         "仅 size 变化 → 变更（同毫秒改写只能靠这一格捕获）"},
        {ident(false, 0, 0), ident(false, 0, 0), true,
         "两个「不存在」态视为同一身份（缺失文件也走缓存，不每次重试装载）"},
        {ident(false, 0, 0), ident(true, 0, 0), false,
         "「不存在」与「存在但空文件」不同身份（exists 独立于 size）"},
        {ident(true, 0, 0), ident(false, 0, 0), false,
         "「存在但空文件」与「不存在」不同身份（同上，反方向）"},
        {ident(true, 1000, 200), ident(false, 999, 199), false,
         "三字段全变 → 变更（判据不是「任一相等即不变」）"},
    };
    for (const Row &r : rows)
    {
        TestHarness::check(SettingsCache::sameIdentity(r.a, r.b) == r.expected, r.what);
        // 双向都跑：判据若不对称，「A→B 判变、B→A 判不变」会让缓存反复重载或反复漏检
        TestHarness::check(SettingsCache::sameIdentity(r.b, r.a) == r.expected,
                           "判据对称（同上用例反方向）");
    }
}

// ---- 组 1b：装载可信判据穷举（不变量⑤，评审 B1 的红线）----
// 症状：文件被编辑器/杀软/同步盘瞬时独占 → QFile::open 失败 → allKeys() 交出空表。
// 若把「空表 + 当时身份」一起提交，一次瞬时失败就被放大成「直到配置文件再被改动前，所有键
// 静默回落默认值」（apiToken 读空、上限回默认、侧栏偏好被重置）——旧路径每次现构造天然
// 会重试，缓存化必须显式保住这个行为。
void testLoadCommittable()
{
    using SettingsCache::loadIsCommittable;

    // 读失败：键空 + 文件声称有字节 + AccessError → 唯一要拒绝的组合
    TestHarness::check(!loadIsCommittable(QSettings::AccessError, true, true),
                       "AccessError + 空表 + 文件有字节 → 不提交身份（下次重试，不变量⑤）");
    // 空文件/缺失文件交出空表是**正确**结果，必须提交，否则每读都重载、缓存形同虚设
    TestHarness::check(loadIsCommittable(QSettings::AccessError, true, false),
                       "AccessError + 空表 + 文件无字节 → 提交（空/缺失文件的空表就是答案）");
    // FormatError 交出的是能解析的部分，与直读逐键一致；拒绝它会让坏文件把热读打回全量重装载
    TestHarness::check(loadIsCommittable(QSettings::FormatError, true, true),
                       "FormatError + 空表 + 文件有字节 → 提交（缓存不该比直读更挑食）");
    TestHarness::check(loadIsCommittable(QSettings::NoError, true, true),
                       "NoError + 空表 + 文件有字节 → 提交（全注释/全空行的合法 ini）");
    // 只要拿到过键，即便 status 非 NoError 也提交（Qt 是「尽力解析」，与每次现取一致）
    TestHarness::check(loadIsCommittable(QSettings::AccessError, false, true),
                       "有键入表 → 提交（status 非 NoError 也不拒绝部分解析结果）");
    TestHarness::check(loadIsCommittable(QSettings::NoError, false, false),
                       "正常装载到键 → 提交");
}

// ---- 组 1c：缺失文件态（零夹具：只要求该路径不存在）----
// 启动时 config.json 还没被写过是常态。这一态下缓存必须：读到 fallback、且**不**每次重试
// 装载。注意 Qt 自己的短路要求 size > 0（qsettings.cpp:1345），缺失/空文件它每次构造都会
// 走读盘分支——这一格是本修复相对 Qt 内部 ConfFileCache 的净增益，故值得单独钉。
void testMissingFileState()
{
    const QString missing =
        QStringLiteral("lite-harness-tst-absent-dir/config.json"); // 不存在的相对子目录
    TestHarness::check(!QFileInfo(missing).exists(),
                       "前置：该路径确实不存在（否则本组断言全部失效）");

    const Identity absent = SettingsCache::identityOf(missing);
    TestHarness::check(!absent.exists, "缺失文件 identityOf.exists == false");
    TestHarness::check(absent.mtimeMs == 0, "缺失文件 mtimeMs 归零（不给无效 QDateTime 的野值）");
    TestHarness::check(absent.sizeBytes == 0, "缺失文件 sizeBytes == 0");
    TestHarness::check(SettingsCache::sameIdentity(absent, SettingsCache::identityOf(missing)),
                       "缺失态可重复取得且判为不变（否则每读都重载，缓存形同虚设）");

    Reader reader(missing);
    // 构造期归一绝对路径：身份 stat 与 QSettings 装载必须指向同一个文件，
    // 相对路径 + 运行期 cwd 变化会让两者分家。
    TestHarness::check(QFileInfo(reader.path()).isAbsolute(),
                       "构造期把路径归一成绝对形式（stat 与装载同目标）");
    TestHarness::check(reader.path().endsWith(QStringLiteral("/config.json"))
                           || reader.path().endsWith(QStringLiteral("\\config.json")),
                       "归一只补前缀不改末段文件名");

    TestHarness::check(reader.loadCount() == 0, "构造 Reader 零装载（装载只发生在首次取值）");
    TestHarness::check(!reader.value(QStringLiteral("anyKey")).isValid(),
                       "缺失文件取不存在的键 → 无效 QVariant（等价旧路径 value(key) 无默认参）");
    TestHarness::check(reader.value(QStringLiteral("anyKey"), 42).toInt() == 42,
                       "缺失文件取键 → 交出调用方 fallback（与 QSettings::value 同语义）");
    TestHarness::check(reader.loadCount() == 1, "首次取值装载一次");
    for (int i = 0; i < 20; ++i)
        reader.value(QStringLiteral("anyKey"));
    TestHarness::check(reader.loadCount() == 1,
                       "缺失态下 20 次再读仍只装载一次（缺失文件同样进缓存，不每次重试）");
    TestHarness::check(reader.cachedKeyCount() == 0, "缺失文件缓存键数为 0");
}

// ---- 组 2：命中即零装载（承重断言，问题②的全部收益都押在这条上）----
void testCacheHitIsFree(const QString &iniPath)
{
    if (!writeIniExternally(iniPath, QStringLiteral("alpha"), QStringLiteral("v1")))
    {
        // 前置不成立就中止：继续跑会让「缓存 == 直读」双双落到 invalid，变成假绿
        TestHarness::check(false, "组 2 前置落盘失败，后续断言中止（防 invalid==invalid 假绿）");
        return;
    }

    Reader reader(iniPath);
    TestHarness::check(reader.loadCount() == 0, "构造不读盘（把 IO 推迟到首次取值）");
    TestHarness::check(reader.value(QStringLiteral("alpha")).toString() == QStringLiteral("v1"),
                       "首次取值得到盘上的值");
    TestHarness::check(reader.loadCount() == 1, "首次取值装载一次");

    // 生产形态：AgentConst 的取值函数被 per-tool 级消费点反复调用，这里模拟 200 次热读
    bool stable = true;
    for (int i = 0; i < 200; ++i)
        stable = stable && (reader.value(QStringLiteral("alpha")).toString() == QStringLiteral("v1"));
    TestHarness::check(stable, "200 次热读值全部一致（缓存不随读取次数漂移）");
    TestHarness::check(reader.loadCount() == 1,
                       "200 次热读只装载一次（命中缓存零 QSettings 构造——问题②的整改目标）");

    // 缺失键也必须走命中路径：不能因为「查不到」就退化成重载
    for (int i = 0; i < 10; ++i)
        reader.value(QStringLiteral("notThere"), QStringLiteral("fb"));
    TestHarness::check(reader.loadCount() == 1, "缺失键反复取不触发重载（负结果同样被缓存）");
    TestHarness::check(reader.value(QStringLiteral("notThere"), QStringLiteral("fb")).toString()
                           == QStringLiteral("fb"),
                       "缺失键交出 fallback，不是空 QVariant");
}

// ---- 组 3：外部手改立即生效（README「其余手改配置文件即可」的技术依据）----
// 简单 static 缓存会让这四条全红——那正是当初否决它的原因。
void testExternalEditTakesEffect(const QString &iniPath)
{
    if (!writeIniExternally(iniPath, QStringLiteral("beta"), QStringLiteral("001")))
    {
        TestHarness::check(false, "组 3 前置落盘失败，后续断言中止");
        return;
    }

    Reader reader(iniPath);
    TestHarness::check(reader.value(QStringLiteral("beta")).toString() == QStringLiteral("001"),
                       "读到初值 001");
    const int loadsAfterFirst = reader.loadCount();

    // ① 等长改写：size 不变，只有 mtime 变 → 专治「只看 size」的实现。
    //    mtime 显式前推，不赌「写完恰好跨过毫秒边界」（见 advanceMtime 注释）。
    if (!writeIniExternally(iniPath, QStringLiteral("beta"), QStringLiteral("002")))
    {
        TestHarness::check(false, "组 3 等长改写落盘失败，后续断言中止");
        return;
    }
    TestHarness::check(QFileInfo(iniPath).size() > 0, "前置：等长改写后文件仍非空");
    if (!advanceMtime(iniPath, 5000))
    {
        TestHarness::skip("SKIP: setFileTime unsupported, equal-length external edit case skipped\n");
    }
    else
    {
        TestHarness::check(reader.value(QStringLiteral("beta")).toString() == QStringLiteral("002"),
                           "等长手改立即读到新值（mtime 单字段足以捕获，不变量②）");
        TestHarness::check(reader.loadCount() == loadsAfterFirst + 1,
                           "等长改写触发且只触发一次重载");
    }

    // ② 变长改写：size 变 → 与 mtime 双保险（文件系统时间精度不足时的兜底，
    //    FAT32/exFAT 粒度 2 秒、部分网络盘 1 秒，这类机器上等长改写靠的就是这一格）
    if (!writeIniExternally(iniPath, QStringLiteral("beta"), QStringLiteral("a-much-longer-value")))
    {
        TestHarness::check(false, "组 3 变长改写落盘失败，后续断言中止");
        return;
    }
    TestHarness::check(reader.value(QStringLiteral("beta")).toString()
                           == QStringLiteral("a-much-longer-value"),
                       "变长手改立即读到新值");

    // ③ 新增键：以前查不到，改写后必须查得到（缓存不能只刷旧键）
    TestHarness::check(!reader.value(QStringLiteral("gamma")).isValid(), "前置：gamma 尚不存在");
    if (!writeIniExternally(iniPath, QStringLiteral("gamma"), QStringLiteral("new")))
    {
        TestHarness::check(false, "组 3 新增键落盘失败，后续断言中止");
        return;
    }
    TestHarness::check(reader.value(QStringLiteral("gamma")).toString() == QStringLiteral("new"),
                       "手改新增的键立即可读（重载是整表重建，不是增量修补）");

    // ④ 删除键：改写后必须回落到 fallback（残留旧值 = 配置删不掉的经典症状）
    TestHarness::check(reader.value(QStringLiteral("beta")).isValid(), "前置：beta 尚在");
    {
        QSettings settings(iniPath, QSettings::IniFormat);
        settings.remove(QStringLiteral("beta"));
        settings.sync();
    }
    TestHarness::check(!reader.value(QStringLiteral("beta")).isValid(),
                       "手改删除的键立即消失（回落到调用方 fallback）");
}

// ---- 组 4：缓存与现取逐键一致（改动的全部风险面就是「交出不一样的值」）----
// 只要有一条不等，消费点（AgentConst 的范围校验/回退默认）就会看到与整改前不同的输入。
void testParityWithDirectRead(const QString &iniPath)
{
    // 覆盖生产里出现过的全部值形态：标量串、整数、bool、逗号串、空值
    const bool prepared =
        writeIniExternally(iniPath, QStringLiteral("contextCharLimit"), QStringLiteral("300000"))
        && writeIniExternally(iniPath, QStringLiteral("maxRetries"), 7)
        && writeIniExternally(iniPath, QStringLiteral("sidebarVisible"), false)
        && writeIniExternally(iniPath, QStringLiteral("modelOptions"), QStringLiteral("m-a,m-b,m-c"))
        && writeIniExternally(iniPath, QStringLiteral("defaultWorkDir"), QString());
    if (!prepared)
    {
        TestHarness::check(false, "组 4 前置落盘失败，后续断言中止（防 invalid==invalid 假绿）");
        return;
    }

    const QStringList keys{QStringLiteral("contextCharLimit"), QStringLiteral("maxRetries"),
                           QStringLiteral("sidebarVisible"), QStringLiteral("modelOptions"),
                           QStringLiteral("defaultWorkDir")};

    Reader reader(iniPath);
    for (const QString &key : keys)
    {
        const QVariant cached = reader.value(key);
        const QVariant direct = readIniDirectly(iniPath, key);
        TestHarness::check(cached.typeId() == direct.typeId(),
                           (QStringLiteral("逐键一致：类型同 [") + key + QStringLiteral("]"))
                               .toUtf8()
                               .constData());
        TestHarness::check(cached == direct,
                           (QStringLiteral("逐键一致：值同 [") + key + QStringLiteral("]"))
                               .toUtf8()
                               .constData());
    }

    // 整表对表：逐键清单是写死的，抓不到「allKeys 里某个键没进缓存」这类形态漏
    {
        const QSettings settings(iniPath, QSettings::IniFormat);
        TestHarness::check(reader.cachedKeyCount() == settings.allKeys().size(),
                           "缓存键数 == 现取 allKeys 数（整表覆盖，无漏装）");
    }

    // 逗号串经 QSettings 写侧会被加引号（见 writeRawIni 注释），读回来是 QString——
    // 这里只要求「缓存 == 现取」，列表形态的专门验证在组 5。
    TestHarness::check(reader.value(QStringLiteral("modelOptions")).typeId()
                           == readIniDirectly(iniPath, QStringLiteral("modelOptions")).typeId(),
                       "程序写侧的逗号串形态两侧一致（引号由 Qt 处理，缓存不参与）");

    // 「键存在但值为空」≠「键缺失」：前者交出空串，后者交出 fallback。消费点靠这两者的
    // 区别区分「用户清空了配置」与「用户从没配过」。
    TestHarness::check(reader.value(QStringLiteral("defaultWorkDir")).isValid(),
                       "空值键的 QVariant 有效（区别于缺失键）");
    TestHarness::check(reader.value(QStringLiteral("defaultWorkDir"), QStringLiteral("FB"))
                           .toString() == QString(),
                       "空值键不吃 fallback（否则「清空配置」会被读成默认值）");

    // 大小写不敏感（不变量④）：IniFormat 的 IniCaseSensitivity == Qt::CaseInsensitive
    // （qsettings_p.h:54 默认分支），直查精确串会让「手改时大小写打错」这种以前能用的写法
    // 突然失效。两条一起断言：既证缓存命中，又证这不是「两边都查不到」的假绿。
    const QVariant exact = reader.value(QStringLiteral("contextCharLimit"));
    TestHarness::check(reader.value(QStringLiteral("CONTEXTCHARLIMIT")) == exact
                           && reader.value(QStringLiteral("ContextCharLimit")) == exact,
                       "缓存查表大小写不敏感（与 IniFormat 语义一致，不变量④）");
    TestHarness::check(readIniDirectly(iniPath, QStringLiteral("CONTEXTCHARLIMIT")) == exact,
                       "对照基准：现取 QSettings 同样大小写不敏感（证明上一条不是假绿）");
}

// ---- 组 5：人手编辑的三种真实形态（QSettings 写侧造不出来，只能裸写文件）----
// 为什么必须单独测：缓存是用 allKeys() 整表建的，而消费点用的是 value(key)——**两个入口的
// 键形态必须一致**，否则「直读取到、缓存读不到」会静默发生。最脆弱的就是带 [Section] 的
// 分组键（allKeys 交 "Section/key"，value 也按 "Section/key" 查），而 QSettings 写侧永远
// 只产出 [General]，这条路径在本轮改动前从未被执行过。
void testHandWrittenForms(const QString &barePath, const QString &groupPath, const QString &invalidPath)
{
    // ① 无段头的裸 key=value（用户手写 ini 的最常见形态；readIniFile 走空段分支）
    if (!writeRawIni(barePath, QByteArrayLiteral("modelOptions=qwen-a,qwen-b,qwen-c\n")))
    {
        TestHarness::check(false, "组 5 裸无段头文件落盘失败，后续断言中止");
        return;
    }
    {
        const QVariant direct = readIniDirectly(barePath, QStringLiteral("modelOptions"));
        TestHarness::check(direct.metaType() == QMetaType::fromType<QStringList>(),
                           "前提：无段头 + 裸逗号串仍被解析成 QStringList（空段分支不是漏解析）");
        Reader reader(barePath);
        const QVariant cached = reader.value(QStringLiteral("modelOptions"));
        TestHarness::check(cached.metaType() == QMetaType::fromType<QStringList>(),
                           "缓存保留 QStringList 形态（textValue 的归一化前提不被绕过）");
        TestHarness::check(cached.toStringList() == direct.toStringList(),
                           "无段头形态下缓存与现取逐项一致");
        TestHarness::check(cached.toStringList().size() == 3, "裸逗号串仍是 3 项");
    }

    // ② 自定义 [Section] + 分组键：allKeys 的拼法与 value() 的查法必须对得上
    if (!writeRawIni(groupPath, QByteArrayLiteral("[Workflow]\nruntime=120\n")))
    {
        TestHarness::check(false, "组 5 分组键文件落盘失败，后续断言中止");
        return;
    }
    {
        const QSettings settings(groupPath, QSettings::IniFormat);
        const QStringList allKeys = settings.allKeys();
        TestHarness::check(allKeys.contains(QStringLiteral("Workflow/runtime")),
                           "前提：分组键在 allKeys 里带 Section/ 前缀");
        Reader reader(groupPath);
        const QVariant cached = reader.value(QStringLiteral("Workflow/runtime"));
        TestHarness::check(cached == settings.value(QStringLiteral("Workflow/runtime")),
                           "分组键：缓存与现取一致（allKeys 拼法 == value 查法）");
        TestHarness::check(cached.toString() == QStringLiteral("120"),
                           "分组键读得到值（不是静默回落 fallback）");
        TestHarness::check(reader.cachedKeyCount() == allKeys.size(),
                           "分组键整表覆盖（缓存装了这个段的全部键）");
        // [General] 之外的段不该以裸键名泄漏出来
        TestHarness::check(!reader.value(QStringLiteral("runtime")).isValid(),
                           "裸键名查不到分组键（与直查同语义，不因归一化而多命中）");
    }

    // ③ @Invalid() 形态：QSettings::setValue(key, invalid) 不删键而是写 key=@Invalid()
    //   （qsettings.cpp:373-377），读回来是无效 QVariant。缓存既不该把它换成 fallback，
    //   也不该把它当成键不存在——否则「传 invalid 即删键」的直觉会把两者混成一团。
    if (!writeRawIni(invalidPath, QByteArrayLiteral("[General]\nbroken=@Invalid()\n")))
    {
        TestHarness::check(false, "组 5 @Invalid() 文件落盘失败，后续断言中止");
        return;
    }
    {
        const QVariant direct = readIniDirectly(invalidPath, QStringLiteral("broken"));
        TestHarness::check(!direct.isValid(), "前提：@Invalid() 读回是无效 QVariant");
        Reader reader(invalidPath);
        TestHarness::check(!reader.value(QStringLiteral("broken")).isValid(),
                           "缓存原样交出无效 QVariant（不换成 fallback）");
        TestHarness::check(reader.cachedKeyCount() == 1,
                           "@Invalid() 的键仍占一个缓存位（区别于「键不存在」）");
    }
}

// ---- 组 6：写侧（AppSettings::setValue 的等价物）——落盘与失效成对发生 ----
void testWritePath(const QString &iniPath)
{
    Reader reader(iniPath);
    TestHarness::check(!reader.value(QStringLiteral("language")).isValid(),
                       "前置：language 尚未配置");

    TestHarness::check(reader.setValue(QStringLiteral("language"), QStringLiteral("zh_CN")),
                       "写返回成功（status()==NoError）");
    TestHarness::check(reader.value(QStringLiteral("language")).toString()
                           == QStringLiteral("zh_CN"),
                       "写后立即读得到新值（显式 invalidate 补上了同毫秒等长改写的盲区）");
    TestHarness::check(readIniDirectly(iniPath, QStringLiteral("language")).toString()
                           == QStringLiteral("zh_CN"),
                       "值确实落到了磁盘（不是只在内存里改了个副本）");

    // 等长自写：001 → 002，size 完全不变。只靠身份判据在这一格是不确定的（文件系统 mtime
    // 精度到毫秒，同一毫秒内写完完全可能判「未变」），显式失效把它变成确定行为——这正是
    // 不变量③把「显式 invalidate」列为承重项的原因。
    reader.setValue(QStringLiteral("seq"), QStringLiteral("001"));
    TestHarness::check(reader.value(QStringLiteral("seq")).toString() == QStringLiteral("001"),
                       "等长自写前置值可读");
    reader.setValue(QStringLiteral("seq"), QStringLiteral("002"));
    TestHarness::check(reader.value(QStringLiteral("seq")).toString() == QStringLiteral("002"),
                       "等长自写立即读到新值（不变量③：写侧失效不依赖文件身份判据）");
    TestHarness::check(readIniDirectly(iniPath, QStringLiteral("seq")).toString()
                           == QStringLiteral("002"),
                       "等长自写同样落了盘");

    // 写不得破坏其它键（QSettings 写侧是整表回写，缓存不该只记得刚写的那一个）
    TestHarness::check(reader.value(QStringLiteral("language")).toString()
                           == QStringLiteral("zh_CN"),
                       "连续写后先前写的键仍在（写侧未丢其它键）");

    // 显式失效 = 重载一次，不是「清空即返回空」
    const int loadsBeforeInvalidate = reader.loadCount();
    reader.invalidate();
    TestHarness::check(reader.value(QStringLiteral("language")).toString()
                           == QStringLiteral("zh_CN"),
                       "显式 invalidate 后重读仍取到盘上的值");
    TestHarness::check(reader.loadCount() == loadsBeforeInvalidate + 1,
                       "显式 invalidate 触发且只触发一次重载");
}

} // namespace

int tst_settingscache()
{
    const int before = TestHarness::failCount();

    // 组 1：零夹具，任何环境全量执行
    testIdentityPredicate();
    testLoadCommittable();
    testMissingFileState();

    ScopedTempRoot root(QStringLiteral("settingscache"));
    if (!root.isValid())
    {
        TestHarness::skip("SKIP: LITE_TEST_TMPROOT unavailable, settings cache file groups skipped\n");
        return TestHarness::failCount() - before;
    }

    // 每个组用各自的 ini：组间互不污染键空间（共用一个文件会让「删除键」之类的断言依赖
    // 执行顺序，改天插新用例就假红）
    testCacheHitIsFree(QDir(root.path()).filePath(QStringLiteral("hit.ini")));
    testExternalEditTakesEffect(QDir(root.path()).filePath(QStringLiteral("external.ini")));
    testParityWithDirectRead(QDir(root.path()).filePath(QStringLiteral("parity.ini")));
    testHandWrittenForms(QDir(root.path()).filePath(QStringLiteral("bare.ini")),
                         QDir(root.path()).filePath(QStringLiteral("group.ini")),
                         QDir(root.path()).filePath(QStringLiteral("invalid.ini")));
    testWritePath(QDir(root.path()).filePath(QStringLiteral("write.ini")));

    return TestHarness::failCount() - before;
}
