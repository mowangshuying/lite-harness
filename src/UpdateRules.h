#pragma once

// 自更新判据单源（header-only、仅 QtCore，纪律同 ProjectInstructions.h / MemoryRules.h）：
// 版本解析与比较、发布资产择优、digest 归一、zip 条目→安装树映射、换体计划与回滚、
// 残留清理计划、可自更新性判定。
//
// 为什么单独成头：IO 侧（QNetworkAccessManager / QProcess tar / QFile）链 Qt6::Network
// 因而进不了 ctest，而下面每条判据都是「跑得出但做错事」的高危面，错了不可挽回：
//   · 版本比较走字典序：`"s17.9" > "s17.11"` 为真，于是 17.11 的用户永远收不到 17.12 的更新，
//     或反向误判把旧版当新版——补丁位是本仓既定形态（s17.10/s17.11），这不是假想。
//   · zip 条目映射不设白名单：一个恶意/损坏的包能写 `../config/config.json` 直接毁掉数据根。
//     数据根不在 zip 里是事实，但「不在包里」不等于「进不来」，必须在映射层拒绝。
//   · 顶层目录名硬编码：包内根目录带版本号（lite-harness-s17.11-win64/），随版本变；
//     写死等于下次更新必然解错路径。
//   · 换体顺序写成「先写新再改旧」：Windows 下运行中的 exe 不可覆盖，必须先改名腾位；
//     而改名后仍在运行的镜像不可删除（实测），所以删除只能推迟到新实例启动时。
//   · 回滚缺失：改名到一半失败若不回滚，安装目录就处于半新半死的不可用状态。
//   · 开发态不拦：`build/bin` 里没有 Qt6*.dll（实测 0 个），一旦在开发树里执行换体，
//     发布包会盖进构建产物目录，把开发环境搅成不可预期状态。
//
// 本头零 IO、零平台调用：所有「盘上事实」由调用方以值或探针传入，因此可全量 ctest。

#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QVector>

namespace UpdateRules {

// ---- 版本 ----

// 解析版本串为数值段：剥可选前缀字母（s/v），按 '.' 分段取十进制。
// 形态为「主.补丁」两段，或「主.补丁.修订」三段（s17.13.1 起启用第三段）。
// **出参个数就是调用方声明的形态宽度**：给了 `micro` 才接受三段；只给两指出参时遇到三段
// 一律返回 false，而不是把第三段悄悄丢掉——静默截断会把 `s17.13.1` 读成 `s17.13`，
// 于是「已装 17.13.1」与「已装 17.13」在残留清理闸门里等价，那正是本闸门唯一要防的事。
// 段数 >3、任一段非纯数字、`+17.11` 这类 toInt 宽松形态同样拒。
inline bool parseVersion(const QString &text, int *major, int *patch, int *micro = nullptr)
{
    if (major == nullptr || patch == nullptr)
        return false;
    QString body = text.trimmed();
    if (body.isEmpty())
        return false;
    const QChar head = body.at(0);
    if (head == QLatin1Char('s') || head == QLatin1Char('S')
        || head == QLatin1Char('v') || head == QLatin1Char('V'))
        body = body.mid(1);
    const QStringList parts = body.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.size() != 2 && parts.size() != 3)
        return false;
    if (parts.size() == 3 && micro == nullptr)
        return false; // 调用方不读第三段 → 拒，绝不静默截断成两段
    bool okMajor = false;
    bool okPatch = false;
    bool okMicro = true;
    const int m = parts.at(0).toInt(&okMajor);
    const int p = parts.at(1).toInt(&okPatch);
    const int r = parts.size() == 3 ? parts.at(2).toInt(&okMicro) : 0;
    // toInt 对 "+3"/" 3" 等形态可能宽松通过，这里再逐字确认是纯数字
    for (const QString &seg : parts) {
        if (seg.isEmpty())
            return false;
        for (const QChar c : seg) {
            if (!c.isDigit())
                return false;
        }
    }
    if (!okMajor || !okPatch || !okMicro)
        return false;
    *major = m;
    *patch = p;
    if (micro != nullptr)
        *micro = r; // 两段形态下第三段按 0 计：s17.13 == s17.13.0
    return true;
}

// 数值比较：负=左小、0=相等、正=左大。任一解析失败返回 0（当作「不是更新」，
// 宁可不更新也不基于垃圾串换体——fail-closed 与全仓「解析不出来回退更保守一侧」同调）。
// 逐段比到第三段：`s17.13.1 > s17.13`（第三段按 0 补）必须成立，否则刚发的补丁版
// 会被判成「不是更新」，用户停在 s17.13 上再也升不上来。
inline int compareVersions(const QString &left, const QString &right)
{
    int lm = 0, lp = 0, lr = 0, rm = 0, rp = 0, rr = 0;
    if (!parseVersion(left, &lm, &lp, &lr) || !parseVersion(right, &rm, &rp, &rr))
        return 0;
    if (lm != rm)
        return lm < rm ? -1 : 1;
    if (lp != rp)
        return lp < rp ? -1 : 1;
    if (lr != rr)
        return lr < rr ? -1 : 1;
    return 0;
}

// 是否该更新：远端 tag 严格大于本地版本串。
inline bool isUpdateAvailable(const QString &localVersion, const QString &remoteTag)
{
    return compareVersions(remoteTag, localVersion) > 0;
}

// ---- 发布资产 ----

// 从资产名清单里挑出本包应下的那一个：以 `-win64.zip` 结尾，且（若给了 tag）含该 tag。
// 多个命中时取字典序首个（确定性，不随 API 返回顺序漂移）；无命中返回空串。
// 刻意不做「取第一个 .zip」这种宽松匹配：将来 Release 若挂 mac/linux 资产，
// 宽松匹配会把错平台的包拖下来并在校验前就动手换体。
inline QString pickAssetName(const QStringList &names, const QString &tag)
{
    QStringList hits;
    for (const QString &name : names) {
        if (!name.endsWith(QStringLiteral("-win64.zip")))
            continue;
        if (!tag.isEmpty() && !name.contains(tag))
            continue;
        hits.append(name);
    }
    if (hits.isEmpty())
        return QString();
    hits.sort();
    return hits.first();
}

// digest 归一：API 给的是 `sha256:<hex>`，剥前缀并转小写；形态不对返回空串
// （空串在调用侧一律当「不可信、拒绝换体」，绝不因缺失而放行）。
inline QString normalizeDigest(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    const int colon = trimmed.indexOf(QLatin1Char(':'));
    QString hex = colon < 0 ? trimmed : trimmed.mid(colon + 1);
    hex = hex.toLower();
    if (hex.size() != 64)
        return QString();
    for (const QChar c : hex) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return QString();
    }
    return hex;
}

// 校验和比对：两者都必须是非空 64 位十六进制，任一为空即不等（fail-closed）。
inline bool digestMatches(const QString &computedHex, const QString &apiDigestHex)
{
    if (computedHex.isEmpty() || apiDigestHex.isEmpty())
        return false;
    return computedHex.compare(apiDigestHex, Qt::CaseInsensitive) == 0;
}

// ---- zip 条目 → 安装树映射 ----

// 允许被更新覆盖的顶层目录。白名单而非黑名单：数据根 `config/`、日志 `log/`
// 天然不在名单里，将来新增目录也不会意外被覆盖（黑名单必然漏）。
inline const QStringList &replaceableTopDirs()
{
    static const QStringList dirs = {
        QStringLiteral("bin"),
        QStringLiteral("plugins"),
        QStringLiteral("translations"),
    };
    return dirs;
}

// 单条目归一：反斜杠转正斜杠、剥 `./`、**含 `..` 段直接判非法**（zip-slip 防线）。
// 绝对路径与盘符形态同样拒绝——它们能把写入点带到安装树之外。
// 定义顺序在 derivePackageRoot 之前：inline 自由函数没有类成员那样的两阶段查找，
// 先定义者引用后定义者会当场编译失败（本头初稿就踩了这一下）。
inline QString normalizeEntryPath(const QString &entry)
{
    QString path = entry.trimmed();
    if (path.isEmpty())
        return QString();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1Char('/')))
        return QString();
    if (path.contains(QLatin1Char(':')))
        return QString(); // 盘符（"C:/..."）或任何冒号形态
    const QStringList rawSegments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (rawSegments.isEmpty())
        return QString();
    // `.` 段剥掉（tar 会产出 "./bin/x" 形态，留着会让映射对不上白名单而静默拒更新），
    // `..` 段直接判非法。注释与实现必须同条：本头初稿只写了拒 `..` 却声称剥 `./`，
    // 是 tst_updaterules 的「leading ./ stripped」断言当场抓出来的。
    QStringList segments;
    for (const QString &seg : rawSegments) {
        if (seg == QLatin1String("."))
            continue;
        if (seg == QLatin1String(".."))
            return QString();
        segments.append(seg);
    }
    if (segments.isEmpty())
        return QString();
    return segments.join(QLatin1Char('/'));
}

// 从 zip 条目清单推导包内根目录（形如 `lite-harness-s17.11-win64/`）。
// 全部条目必须共享同一个首段，否则返回空串（多根=包结构异常，拒绝继续）。
// 刻意不硬编码根目录名：它带版本号，随每次发布而变，写死等于下次更新必然解错路径。
inline QString derivePackageRoot(const QStringList &entries)
{
    QString root;
    for (const QString &entry : entries) {
        const QString normalized = normalizeEntryPath(entry);
        if (normalized.isEmpty())
            return QString();
        const int slash = normalized.indexOf(QLatin1Char('/'));
        if (slash <= 0)
            return QString(); // 根下裸文件：不是本仓包形态
        const QString first = normalized.left(slash);
        if (root.isEmpty())
            root = first;
        else if (root != first)
            return QString(); // 多根，拒绝
    }
    return root;
}

// 条目 → 安装根相对路径。返回空串 = 拒绝该条目。
// rootPrefix 为包内根（derivePackageRoot 的结果）；剥掉它后首段必须落在白名单里。
// 目录条目（以 '/' 结尾）与空余段一律拒绝——换体只处理文件。
inline QString mapEntryToInstallRel(const QString &entry, const QString &packageRoot)
{
    const QString normalized = normalizeEntryPath(entry);
    if (normalized.isEmpty())
        return QString();
    if (normalized.endsWith(QLatin1Char('/')))
        return QString();
    if (packageRoot.isEmpty())
        return QString();
    const QString prefix = packageRoot + QLatin1Char('/');
    if (!normalized.startsWith(prefix))
        return QString();
    const QString rel = normalized.mid(prefix.size());
    if (rel.isEmpty())
        return QString();
    const int slash = rel.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString(); // 根下裸文件（剥掉包根后仍无目录层）
    const QString top = rel.left(slash);
    if (!replaceableTopDirs().contains(top))
        return QString();
    return rel;
}

// 备份文件的后缀单源：换体计划写入、marker 识别、下次启动清理三处都走这一个串。
// 定义必须在 planSwap 之前——inline 自由函数没有类成员那样的两阶段查找，先引用后定义
// 会当场编译失败（本头已在 normalizeEntryPath 上踩过同一形态的坑，这里一并防住）。
inline QString backupSuffix()
{
    return QStringLiteral(".update-backup");
}

// 目录条目判定：以原始串（含反斜杠形态）的尾部分隔符为准。归一化会丢掉尾斜杠，
// 所以「是不是目录」必须在归一**之前**问——否则 `root/bin/` 归一成 `root/bin` 后
// 既映射不出安装路径、又看起来像个非法条目，校验循环会把正常包判成坏包。
inline bool isDirectoryEntry(const QString &entry)
{
    const QString trimmed = entry.trimmed();
    if (trimmed.isEmpty())
        return false;
    return trimmed.endsWith(QLatin1Char('/')) || trimmed.endsWith(QLatin1Char('\\'));
}

// ---- 换体计划 ----

struct SwapItem
{
    QString stagedSource; // 相对**暂存根**的新文件路径（含包内版本化根目录那一段）
    QString liveTarget;   // 相对**安装根**的现行文件路径
    QString backupPath;   // 现行文件改名后的残留，相对安装根
};

// 生成换体计划。输入是包内归一条目路径清单（如 `lite-harness-s17.12-win64/bin/x.exe`），
// 逐条过 mapEntryToInstallRel：映射不出来的条目**整批作废**（返回空计划）而不是跳过——
// 半份计划换体会留下「一部分文件是新的、一部分是旧的」的不可预期运行态，
// 那比不更新糟糕得多。目录条目由调用方先用 isDirectoryEntry 滤掉再进来。
// 计划确定性：同输入同计划，回滚与清理才能按同一顺序反着走。
inline QVector<SwapItem> planSwap(const QStringList &packageEntryPaths, const QString &packageRoot)
{
    QVector<SwapItem> plan;
    QStringList seenTargets;
    for (const QString &entry : packageEntryPaths) {
        if (isDirectoryEntry(entry))
            continue;
        const QString rel = mapEntryToInstallRel(entry, packageRoot);
        if (rel.isEmpty()) {
            return QVector<SwapItem>(); // 任一条目落不进白名单 → 整批拒绝
        }
        if (seenTargets.contains(rel))
            continue; // 同一目标重复：取首个，不重复改名
        seenTargets.append(rel);
        SwapItem item;
        item.stagedSource = normalizeEntryPath(entry);
        item.liveTarget = rel;
        item.backupPath = rel + backupSuffix();
        plan.append(item);
    }
    return plan;
}

// 回滚顺序：已改名的条目按**逆序**改回（后进先出），避免中途失败留下交叉依赖。
inline QStringList rollbackOrder(const QVector<SwapItem> &plan)
{
    QStringList out;
    for (int i = plan.size() - 1; i >= 0; --i)
        out.append(plan.at(i).backupPath);
    return out;
}

// 清理顺序：新实例启动时删残留，先删备份文件、再删暂存目录（暂存目录可能含同名副本）。
inline QStringList cleanupOrder(const QVector<SwapItem> &plan)
{
    QStringList out;
    for (const SwapItem &item : plan)
        out.append(item.backupPath);
    return out;
}

// ---- 待切换标记（marker）：下次启动据此删残留 ----
//
// 这是本头唯一「内容来自磁盘、后果是删文件」的面，必须最严：
// marker 损坏/被篡改/被旧版本写歪时，若照着里面的路径直接删，就等于
// 「用一个 JSON 决定删哪些文件」——任意路径删除事故。因此解析层不只取值，
// 还要逐条重新过一遍与写入时同源的白名单判据（isDeletableBackupPath），
// 任何一条不过就整体作废（valid=false → 调用方一个文件都不删，只删 marker 本身）。

// 待切换标记的结构版本：字段形态变更时上调，旧版本 marker 一律作废（不猜语义）。
// 注：备份后缀 `backupSuffix()` 的定义在上面的换体计划段（planSwap 要用它，inline 自由
// 函数必须先定义后使用），此处只引用、不再重复定义。
inline int markerSchemaVersion()
{
    return 1;
}

struct Marker
{
    bool valid = false;
    QString fromVersion;
    QString toVersion;
    QString packageRoot;
    QStringList backups; // 安装根相对的残留备份路径
};

// 备份路径可删性判据：必须是干净相对路径、落在可替换顶层目录内、且以备份后缀结尾。
// 三条缺一不删——「以 .update-backup 结尾」看着已够，但攻击面正是有人往 config/
// 里塞一个同名后缀的文件；顶层目录白名单才是挡住它的那道。
inline bool isDeletableBackupPath(const QString &installRelPath)
{
    const QString normalized = normalizeEntryPath(installRelPath);
    if (normalized.isEmpty())
        return false;
    if (!normalized.endsWith(backupSuffix()))
        return false;
    const QString bare = normalized.left(normalized.size() - backupSuffix().size());
    const int slash = bare.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return false;
    return replaceableTopDirs().contains(bare.left(slash));
}

// 序列化（写入侧唯一出口，与 parseMarker 成对往返）。
inline void writeMarker(QJsonObject *object, const QString &fromVersion, const QString &toVersion,
                        const QString &packageRoot, const QStringList &backups)
{
    if (object == nullptr)
        return;
    QJsonObject marker;
    marker[QStringLiteral("schema")] = markerSchemaVersion();
    marker[QStringLiteral("from")] = fromVersion;
    marker[QStringLiteral("to")] = toVersion;
    marker[QStringLiteral("packageRoot")] = packageRoot;
    marker[QStringLiteral("backups")] = QJsonArray::fromStringList(backups);
    *object = marker;
}

// 解析（读取侧唯一入口）：结构版本、版本形态、packageRoot、每一条 backup 全过才 valid。
inline Marker parseMarker(const QJsonObject &object)
{
    Marker marker;
    if (object.value(QStringLiteral("schema")).toInt() != markerSchemaVersion())
        return marker; // 结构版本不符：语义未知，整体作废
    marker.fromVersion = object.value(QStringLiteral("from")).toString();
    marker.toVersion = object.value(QStringLiteral("to")).toString();
    marker.packageRoot = object.value(QStringLiteral("packageRoot")).toString();
    int m1 = 0, p1 = 0, r1 = 0, m2 = 0, p2 = 0, r2 = 0;
    if (!parseVersion(marker.toVersion, &m2, &p2, &r2))
        return marker; // 目标版本都读不出：这不是我们的 marker
    if (!marker.fromVersion.isEmpty() && !parseVersion(marker.fromVersion, &m1, &p1, &r1))
        return marker; // from 允许缺省，但给了就必须是合法形态
    if (normalizeEntryPath(marker.packageRoot + QStringLiteral("/bin/x")).isEmpty())
        return marker; // packageRoot 自身必须是个干净单段目录名
    const QJsonArray backups = object.value(QStringLiteral("backups")).toArray();
    if (backups.isEmpty())
        return marker; // 空清单的 marker 没有存在意义，视为异常
    for (const QJsonValue &value : backups) {
        const QString rel = value.toString();
        if (rel.isEmpty() || !isDeletableBackupPath(rel))
            return marker; // 任一条不可删 → 整体作废，绝不部分照办
        marker.backups.append(rel);
    }
    marker.valid = true;
    return marker;
}

// 残留清理的版本闸门：只有「当前运行的版本 == marker 声称已换上的版本」时才删备份。
// 备份文件是**旧版可执行体的唯一副本**：若换体没生效（写盘失败、用户手动回滚、
// 装了第二个旧包）就照 marker 删备份，等于把还能跑的那份删了、留下换了一半的现场。
// 版本对不上 → 一律不删、保留 marker、留给人工处置。
inline bool shouldCleanResidue(const QString &markerToVersion, const QString &currentVersion)
{
    int m1 = 0, p1 = 0, r1 = 0, m2 = 0, p2 = 0, r2 = 0;
    if (!parseVersion(markerToVersion, &m1, &p1, &r1))
        return false;
    if (!parseVersion(currentVersion, &m2, &p2, &r2))
        return false;
    // 三段逐段等值。第三段必须参与：`s17.13` 与 `s17.13.1` 是两个不同的换体目标，
    // 只比前两段就会在「换到 .1 没生效」时把旧体唯一副本删掉。
    return m1 == m2 && p1 == p2 && r1 == r2;
}

// ---- 端点字符串（拼错即 404/403，且失败只表现为「没有更新」，故必须钉死） ----

// Releases API 地址。ownerRepo 由调用方给（本仓 slug 是产品身份，不该散在 URL 字面量里）。
inline QString releaseApiUrl(const QString &ownerRepo)
{
    const QString slug = ownerRepo.trimmed();
    if (slug.isEmpty() || !slug.contains(QLatin1Char('/')))
        return QString(); // 形态不对就返回空，让调用方报错，而不是发一个必 404 的请求
    return QStringLiteral("https://api.github.com/repos/") + slug + QStringLiteral("/releases/latest");
}

// User-Agent。GitHub REST API 缺 UA 一律拒答（本机实测：不带 UA 的请求直接失败，
// 带 UA 才 200），而这个失败在 UI 上和「网络不通」「没有新版本」长得一模一样。
inline QString userAgentFor(const QString &version)
{
    return QStringLiteral("lite-harness/") + (version.isEmpty() ? QStringLiteral("unknown") : version)
        + QStringLiteral(" (self-update-check)");
}

// 从 API 响应里取包体字节数（用于下载进度与「下载完整性初筛」）。取不到返回 -1。
inline qint64 assetSizeOrUnknown(qint64 declaredSize)
{
    return declaredSize > 0 ? declaredSize : -1;
}

// progressPercentOrUnknown 的整数路径安全上界：超过它 `received * 100` 就是有符号溢出。
// 单源在这里而不是散在函数体里写裸数字——函数体与注释必须引用同一个数。
constexpr qint64 kPercentSafeMax = 9223372036854775807LL / 100; // = INT64_MAX / 100

// 下载进度的**分母判据**：返回 0~100 的百分比，未知分母返回 -1（形态同 assetSizeOrUnknown）。
// 抽进本头而不是留在卡里，理由有三条，每条都是「写错不报错、只把 UI 骗了」：
//   · 未知分母必须有一个诚实出口。上面那个函数刻意把「取不到大小」留成 -1 而不是 0，
//     就是为了不在进度里渲染出一个看起来合理的假数字。若这里把 -1/0 当分母去除，
//     要么除零、要么被钳成 100%——用户会据此判断「已经下完了」，而实际还在下。
//   · 环与文案必须同源。说明位文案（`正在下载 %1… 47%`）与进度环吃的是同一个
//     QNetworkReply::downloadProgress，两处各判一次「有没有分母」就会分叉：
//     文案给百分比而环在空转，或反之。单源在这条判据上，不在渲染层。
//   · `received > total` 要钳到 100 而不是照算。Content-Length 中途变化或多算字节时，
//     137% 这种值画在环上是几何无意义的（弧长溢出整圆）。
// 顺序是判据的一部分：先判 `received >= total` 再乘 100。反过来写会在
// received 接近 INT64_MAX 时溢出，把一个正常完成的下载显示成一个奇怪的百分比。
// 但「先钳位」只挡住了 `received >= total` 那一半：`received < total` 且
// received > INT64_MAX/100（约 9.2e16 字节 = 92 PB）时，`received * 100` 仍是有符号
// 溢出（UB）。这种包不存在，但**判据不该靠「调用方给不出这种数」来保证安全**——
// 本头其余每条判据的立场都是「输入来自外部，畸形输入的后果由本头承担」，
// 所以这里按量级分路，把溢出面彻底消掉，而不是只在注释里登记它。
// 分路阈值取 INT64_MAX/100：低于它，整数路径精确；高于它，改用 double——
// 此时包体已是 92 PB 量级，双精度 53 位尾数的相对误差（约 1e-16）远低于 1 个百分点，
// 而换来的是「绝无未定义行为」。
// double 分支必须再钳一次 99：received < total 在数学上保证结果 < 100，但浮点舍入
// 可能把 `total-1 / total` 舍成 100.0，那正好是这条判据存在的理由所要防的事——
// 一个「看起来已经下完」的假 100%。
inline int progressPercentOrUnknown(qint64 received, qint64 total)
{
    if (total <= 0 || received < 0)
        return -1;                  // 分母未知 / 无进账：交给不定态渲染，不编造百分比
    if (received >= total)
        return 100;                 // 下完了：钳到整圆，不画 137% 的溢出弧
    if (received <= kPercentSafeMax)
        return static_cast<int>(received * 100 / total); // 整数路径，逐位精确
    const double percent = static_cast<double>(received) * 100.0 / static_cast<double>(total);
    return percent >= 100.0 ? 99 : static_cast<int>(percent);
}

// ---- 可自更新性 ----

// 布局判定入参：由 IO 侧探测后以值传入（本头不 stat 任何东西）。
//   hasQtRuntimeNextToExe —— 发布包实测 bin/ 里有 Qt6*.dll；开发态 build/bin 实测 0 个。
//   hasConfigRoot         —— <exe>/../config 存在（数据根在位，换体不会碰它但必须同卷）。
//   isWritableInstallRoot —— 安装根可写（Program Files 下只读时不该给用户「点更新」）。
inline bool canSelfUpdate(bool hasQtRuntimeNextToExe, bool hasConfigRoot, bool isWritableInstallRoot)
{
    return hasQtRuntimeNextToExe && hasConfigRoot && isWritableInstallRoot;
}

// 拒绝原因（供 UI 如实显示，不笼统报「更新失败」）。返回空串 = 可以更新。
inline QString refusalReason(bool hasQtRuntimeNextToExe, bool hasConfigRoot,
                             bool isWritableInstallRoot)
{
    if (!hasQtRuntimeNextToExe)
        return QStringLiteral("dev_layout");      // 开发态构建树，禁止换体
    if (!hasConfigRoot)
        return QStringLiteral("no_config_root");  // 非常规安装，数据根位置不明
    if (!isWritableInstallRoot)
        return QStringLiteral("read_only_install");
    return QString();
}

} // namespace UpdateRules
