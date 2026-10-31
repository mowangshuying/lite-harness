#pragma once

// ============================================================================
// 设置读缓存纯头（header-only，仅 QtCore，零 CMake 改动即入 ctest）
//
// 修的是 docs/doc.md §9.10 问题②：`AgentConst` 的取值函数（contextCharLimitValue /
// maxToolIterationsValue / maxRetriesValue / modelOptions / defaultModel /
// fallbackModelIdValue）每次调用都构造一个 QSettings，而消费点已下沉到 per-tool 级
// （ChatSessionPage 每个工具结果读一次 contextTokenBudget()）。
//
// 【口径纠正（读 Qt 6.9 源码 qsettings.cpp 所得，勿再沿用旧说法）】
// 原登记说「每次 open + parse 整个 ini」**不准确**：Qt 自己就有进程级 ConfFileCache
// （qsettings.cpp:81/97 `QCache<QString,QConfFile>`），且 syncConfFile()（:1337）在
// 「只读 + size 与 lastModified 均未变」时直接 return，不重读磁盘。
// 所以本修复**没有**省掉磁盘解析，也**没有**省掉 stat：命中路径每读仍做一次
// `QFileInfo` stat（见不变量①），而旧路径每次构造 QSettings 时 syncConfFile 内部同样
// 做一次 QFileInfo——两边各一次，未省。真正省掉的是每次调用的对象生命周期与簿记：
// `new QSettings`（QObject + d 指针）+ 全局锁 settingsGlobalMutex + ConfFileCache 探测
// + `QSettingsKey` 大小写折叠查表 + 析构里的 ref.deref()/unusedCache 回插
// （注意 `~QSettings` 只在 pendingChanges 时才 flush，qsettings.cpp:2732-2735，
//  只读实例本来就不会在析构写盘——旧说法把这条列成收益是错的）。
// 一句话：**收益真实但不大**，别按「消除了重复磁盘解析」宣称。
//
// 为什么不能「缓存 QSettings 实例 + 按值返回」：Qt 6.9 的 QSettings 继承 QObject 且被
// Q_DISABLE_COPY（qsettings.h:183）——既不可拷贝也不可移动。原 `AppSettings::ini()`
// 能按值返回纯靠 C++17 保证复制消除（prvalue 直接初始化调用方对象）；一旦改成「返回缓存里
// 那个已存在的实例」就需要真拷贝，直接编译不过。故本头缓存的是**解析产物**，不是对象。
//
// 为什么不能「简单 static 缓存」：README「运行时配置」节明确支持「其余手改配置文件即可」，
// 永不失效的缓存会让手改值永不生效——那是可观察的行为回归。
//
// 承重不变量（改本头前必读，红线测试 tests/tst_settingscache.cpp）：
// ① 缓存值 ≡ 现取 QSettings 的对应值。失效判据 = 文件身份三元组 (exists, mtime, size)，
//    与 Qt 自身 syncConfFile 的只读短路判据（size + lastModified，qsettings.cpp:1345-1347）
//    **同族且同精度**（Windows 下 Qt 的 fileTimeToQDateTime 也只到毫秒，
//    qfilesystemengine_win.cpp:1884-1891），故本缓存不引入 Qt 之外的新 staleness 类别。
//    任何「按时间 TTL 过期」的改法都会把「手改立即生效」变成「最多 N 秒后生效」——削弱契约。
//    代价要说清：命中路径**仍每读一次 stat**（identityOf），本头省的是对象与簿记，不是 IO。
// ② 三元组缺一不可：只看 mtime，同毫秒的等长改写会漏；只看 size，等长改写直接漏；
//    不看 exists，「文件从无到有」会被当成未变而永不读到。
// ③ 写侧必须**显式 invalidate**（这一格判据靠不住）：同毫秒 + 等长的改写，身份三元组
//    完全相同，只依赖判据就会一直交旧值——本进程自己的写绝不能这样，故 setValue 里显式失效。
//    「先 sync 后 invalidate」的先后只是纵深防御（零线程模型里两者之间不可能插入一次读），
//    别把它当成承重理由写进别处。
// ④ 键查表的折叠口径必须**等于所用后端的口径**（不是「一律不敏感」）：
//    IniFormat 是 Qt::CaseInsensitive（qsettings_p.h 的 IniCaseSensitivity），故走 ini 时
//    `value("sidebarvisible")` 能命中盘上的 `SidebarVisible`，缓存必须同样 toLower() 才等价；
//    而 json 后端注册为 Qt::CaseSensitive，缓存就必须**逐字**查，否则缓存命中、直取落空。
//    QHash 原生按精确串查，两种情形靠同一个 toLower() 是兜不住的——口径由 Reader 的
//    keyCaseSensitivity 入参决定，调用方按格式配对表（配错即破不变量①，tst_configmerge 钉这条）。
//    归一的代价：盘上若同时存在仅大小写不同的两键（`Foo=1` 与 `foo=2`），本缓存按 allKeys
//    顺序后者覆盖前者；QSettings 直查同样只认一个（同一 caseFold 键位），两边都不保证取哪条，
//    属未定义输入而非本头要兜的行为——全仓键串一律取自 AgentConst::k*Key / AppSettings 注释
//    清单的固定拼法，无大小写变体调用点（tst_settingscache 组 4 逐键对表把这条钉住）。
// ⑤ **装载失败不得提交身份**（评审 B1，缓存化新引入的失效模式）：文件被编辑器/杀软/同步盘
//    瞬时独占时 `QFile::open(ReadOnly)` 失败，Qt 走 `setStatus(AccessError); return;`
//    （qsettings.cpp:1391-1394）且**不更新 confFile->size**，于是 allKeys() 交出空表。
//    旧路径每次现构造，下一次读自然重试；缓存若把「空表 + 当时身份」一起提交，一次瞬时失败
//    就被放大成「直到配置文件再被改动前，所有键静默回落默认值」（apiToken 读空、上限回默认、
//    侧栏偏好被重置）。故 AccessError 且文件声称有字节时：保留既有表、退回未确定身份、下次重试。
//    FormatError **不**在此列——它交出的是能解析的部分，与直读逐键一致，该提交就提交
//    （缓存不该比直读更挑食）。
// ⑥ 零线程约定：Reader 内含可变缓存但无锁，依赖全仓「主线程事件驱动、不起线程」
//    （见 AGENTS.md 数据与路径节）。若将来引入工作线程读配置，必须先给 Reader 加锁，
//    而不是把缓存挪到线程局部。
// ============================================================================

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <utility> // std::move（构造入参搬移 / 装载结果整表交换）

namespace SettingsCache {

// 配置文件身份三元组。缺失文件也有确定值（false/0/0），故「不存在」态同样能进缓存——
// 启动时 config.json 还没被写过是常态；注意 Qt 自己的短路要求 size > 0，空/缺失文件
// 每次构造都会走一遍读盘分支，这一格反而是本缓存相对 Qt 的净增益。
struct Identity
{
    bool exists = false;
    qint64 mtimeMs = 0;
    qint64 sizeBytes = 0;
};

// 纯判据（零 IO，可穷举断言）：三元组逐字段相等才算同一身份。
// 刻意不写「mtime 差小于阈值算不变」这类容差——容差等于给手改加延迟。
inline bool sameIdentity(const Identity &a, const Identity &b)
{
    return a.exists == b.exists && a.mtimeMs == b.mtimeMs && a.sizeBytes == b.sizeBytes;
}

// 取身份：单个 QFileInfo 实例问三件事（Qt 内部懒 stat 后缓存于该实例），故这里只有一次 stat。
// mtime 无效（极端：文件系统不给时间）时记 0——与「不存在」的区分靠 exists 字段。
inline Identity identityOf(const QString &path)
{
    const QFileInfo info(path);
    Identity identity;
    identity.exists = info.exists();
    if (identity.exists)
    {
        const QDateTime modified = info.lastModified();
        identity.mtimeMs = modified.isValid() ? modified.toMSecsSinceEpoch() : 0;
        identity.sizeBytes = info.size();
    }
    return identity;
}

// 查表键归一化（不变量④单点）：口径必须与**该后端自己的**折叠规则一致，绝不能无条件 toLower()。
// IniFormat 是 CaseInsensitive（qsettings_p.h 的 IniCaseSensitivity），而自定义 json 后端在
// FluConfigStore::jsonFormat() 里注册成 **CaseSensitive**。若在这里硬写 toLower()，缓存会命中
// `app/apitoken` 而直取 QSettings 落空——正是本头反复警告的「同一把键两种答案」。
// 所以折叠口径由构造入参交出，由调用方按所用格式配对表（AppSettings::keyCaseSensitivity()）。
inline QString normalizedKey(const QString &key, Qt::CaseSensitivity caseSensitivity)
{
    return caseSensitivity == Qt::CaseInsensitive ? key.toLower() : key;
}

// 装载结果是否可信到可以提交身份（不变量⑤的判据单点，零 IO 可穷举断言）。
// loadedKeys==0 && fileHasBytes && AccessError 是唯一要拒绝的组合：
// · 空文件/缺失文件（fileHasBytes==false）交出空表是**正确**结果，必须提交，否则每读都重载；
// · FormatError 的部分解析结果与直读一致，必须提交（拒绝它会让坏文件把热读打回全量重装载）；
// · 只要拿到过键（loadedKeys>0），即便 status 非 NoError 也提交——那 Qt 是「尽力解析」，
//   与每次现取的行为逐键相同。
inline bool loadIsCommittable(QSettings::Status status, bool loadedKeysEmpty, bool fileHasBytes)
{
    if (!loadedKeysEmpty)
        return true;
    if (!fileHasBytes)
        return true;
    return status != QSettings::AccessError;
}

// 一个配置文件路径的读写门面：读带缓存，写带失效。
// 路径在构造期钉死——AppSettings 用存储根下唯一的 config.json（`LitePaths::configFilePath()`），
// 测试用 LITE_TEST_TMPROOT 下的临时配置文件。路径可注入正是本头能进 ctest 的全部原因
// （`AppSettings` 的门面绑死 QCoreApplication::applicationDirPath()，测试里无从构造，也就无从证伪）。
class Reader
{
public:
    // 构造期把路径归一成绝对形式（QFileInfo::absoluteFilePath 不碰磁盘，只按当前 cwd 解析）：
    // 身份 stat 与 QSettings 装载必须指向同一个文件，相对路径 + 运行期 cwd 变化会让两者分家。
    //
    // 格式默认 IniFormat，**但配置文件合流后宿主侧必须显式传 FluConfigStore::jsonFormat()**：
    // QConfFile 缓存键只有 absPath（qsettings.cpp:153-171），同一路径被两种格式打开时，先解析
    // 者的 readFunc 会独占那张共享表、后到的实例读到的是别人的产物。所以「同一个文件」在 Qt 里
    // 等价于「全仓同一个 Format 枚举值」——这不是风格约束，是缓存键结构决定的硬约束。
    // 折叠口径必须与 format 配对传入（见不变量④）：IniFormat 是 CaseInsensitive，
    // FluConfigStore 的 json 后端是 CaseSensitive。默认值取 IniFormat 的那一对，故只走 ini 的
    // 既有调用点零改动；换成别的格式就必须同时换口径，只换 format 不换 keyCaseSensitivity
    // 会让缓存与直取 QSettings 给出不同答案——这是本类唯一会静默改变读结果的错配。
    explicit Reader(QString path, QSettings::Format format = QSettings::IniFormat,
                    Qt::CaseSensitivity keyCaseSensitivity = Qt::CaseInsensitive)
        : m_path(QFileInfo(path).absoluteFilePath()), m_format(format), m_keyCase(keyCaseSensitivity)
    {
    }
    // 缓存实例不可复制：复制出第二份身份账会各持一份 mtime，失效判据当场失真
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    const QString &path() const { return m_path; }

    // 热读唯一入口：命中即零 QSettings 构造（仍有一次 stat，见不变量①的代价说明）。
    // key 缺失返回 fallback（与 QSettings::value 同语义——注意「键存在但值为空」返回空串
    // 而非 fallback，这两条在测试里分别钉死）。
    QVariant value(const QString &key, const QVariant &fallback = QVariant()) const
    {
        ensureLoaded();
        const auto it = m_cache.constFind(normalizedKey(key, m_keyCase));
        return it == m_cache.constEnd() ? fallback : it.value();
    }

    // 主动失效（写侧专用，见不变量③）：清表并回到「无身份」态，下次取值必重读磁盘。
    void invalidate() const
    {
        m_hasIdentity = false;
        m_cache.clear();
    }

    // 写单点：现构造局部 QSettings 写后即 sync 落盘（保住原「局部对象析构即 sync」语义），
    // 随后 invalidate()。显式失效是必需的而非锦上添花：同毫秒 + 等长的改写落在身份判据的
    // 盲区里（不变量③）。声明为 const 是刻意的——本类的可变部分（缓存/身份/装载计数）全是
    // mutable 逻辑状态，磁盘写入不改动任何成员语义状态，故 reader() 能安全交出 const 引用。
    // 返回值 = 本次写是否落到盘上（status()==NoError）；调用点现在都忽略它（既有语义就是
    // 静默失败），留着是为了让「写失败」这一格将来能被设置页拿来打日志，而不是又变成口头承诺。
    bool setValue(const QString &key, const QVariant &value) const
    {
        QSettings settings(m_path, m_format);
        settings.setValue(key, value); // 键原样写（不改大小写）：读侧归一化已覆盖大小写差异
        settings.sync();
        invalidate();
        return settings.status() == QSettings::NoError;
    }

    // 观测口（测试与诊断用，不参与任何业务判定）：实际发生过几次装载。
    // 有了它，「N 次热读只装载一次」才是可断言的事实，而不是对性能的口头承诺。
    // 装载**失败**（不变量⑤）也计数——那次 QSettings 构造确实发生了。
    int loadCount() const { return m_loads; }

    // 观测口：当前缓存的键数（normalizedKey 之后；仅当口径为 CaseInsensitive 时，大小写不同的
    // 两键才折叠成一个，CaseSensitive 后端下它们本就是两个不同的键，各自入表）。
    // 用途是整表对表：与现取 QSettings 的 allKeys().size() 比，能抓到「allKeys 里某个键
    // 没进缓存」这类形态漏——逐键断言的键清单是写死的，抓不到它。
    int cachedKeyCount() const
    {
        ensureLoaded();
        return static_cast<int>(m_cache.size());
    }

private:
    void ensureLoaded() const
    {
        const Identity current = identityOf(m_path);
        if (m_hasIdentity && sameIdentity(m_cachedIdentity, current))
            return; // 命中：不构造 QSettings、不查键（仍花了一次 stat）

        // 局部 QSettings：构造即走 Qt 的 ConfFileCache/syncConfFile。读侧从不 setValue，
        // 无 pendingChanges，析构不会 flush 写盘（qsettings.cpp:2732-2735）。
        const QSettings settings(m_path, m_format);
        const QStringList keys = settings.allKeys();

        // 不变量⑤：装载失败时**不提交身份、也不清既有表**——保留上一次成功装载的值，
        // 并把状态退回「身份未确定」，下一次取值重试。瞬时独占不该被放大成长期静默回落。
        if (!loadIsCommittable(settings.status(), keys.isEmpty(), current.sizeBytes > 0))
        {
            m_hasIdentity = false;
            ++m_loads;
            return;
        }

        // 整表重建（不是增量修补）：删除的键必须随之消失，否则「配置删不掉」。
        QHash<QString, QVariant> fresh;
        fresh.reserve(keys.size());
        for (const QString &key : keys)
            fresh.insert(normalizedKey(key, m_keyCase), settings.value(key));
        m_cache = std::move(fresh);

        m_cachedIdentity = current;
        m_hasIdentity = true;
        ++m_loads;
    }

    QString m_path;
    QSettings::Format m_format; // 构造期钉死，读写两条路共用同一个值（见构造函数注释）
    // 与 m_format 成对钉死：查表折叠口径必须等于该后端的注册口径，否则缓存与直取分家（不变量④）。
    Qt::CaseSensitivity m_keyCase;
    mutable QHash<QString, QVariant> m_cache; // 键一律 normalizedKey 后入表
    mutable Identity m_cachedIdentity;
    mutable bool m_hasIdentity = false;
    mutable int m_loads = 0;
};

} // namespace SettingsCache
