// 自更新编排层实现：网络、落盘、解压、换体、进程交接。
//
// 分工纪律：所有「算得出但做错」的判断一律在 UpdateRules.h（纯头、进 ctest）；本 TU 只做
// IO、状态推进与进程交接。把判据写回这里等于让它当场失去全部直测覆盖——本文件链
// Qt6::Network，按 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest。
//
// 全链零嵌套事件循环：网络走 QNetworkReply 信号、解压走 QProcess::finished 信号，
// 没有 waitForFinished、没有局部 QEventLoop（阻塞族已随异步化整体删除）。
//
// 换体路线的实测依据（本机验过，不是猜测）：
//   · 改名正在运行的 exe —— OK
//   · 在原路径写新文件 —— OK
//   · 删除改名后仍在运行的镜像 —— 拒绝访问
//   ⇒ 只能「改名腾位 → 写入新体 → 重启 → 由新实例删残留」。
//   「装到同级新目录」这条更省事的路线已被实测否掉：configRoot = <exe>/../config，
//   换目录等于换数据根，用户的会话/记忆/设置会当场「消失」，故必须原地换体。

#include "UpdateManager.h"

#include "LitePaths.h"
#include "LiteLog.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QWidget>
// 刻意显式 include：本文件此前只靠「被 SettingsPage.h 间接带进 QWidget」才编过，
// 那是隐式包含依赖——单独编译本 TU 当场炸 C2027（未定义类型 QWidget）。

namespace {

// 检查请求的总超时。API 调用是小请求，卡住不如报错——UI 上「检查中」无限转圈与
// 「已是最新」「网络不通」三者同形，是本功能最坏的观测面。
constexpr int kCheckTransferTimeoutMs = 15000;
// 下载静默超时：55MB 包不设总时限，但 30s 零字节进账即判链路已死。
constexpr int kDownloadIdleTimeoutMs = 30000;
// 换体互斥锁的陈死时限：崩溃残留不该永久堵死后续更新。
constexpr int kSwapLockStaleMs = 120000;
// 进程交接的退出码：931 已被语言切换自重启占用（gallery 惯例），更新用 932，
// 让日志与外部观察者能区分两种自重启。
constexpr int kRestartForUpdateExitCode = 932;

QString nativeToPortable(const QString &path)
{
    QString out = path;
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

QString joinRoot(const QString &root, const QString &rel)
{
    return root + QLatin1Char('/') + rel;
}

} // namespace

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
}

UpdateManager::~UpdateManager()
{
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
    delete m_downloadHash;
    m_downloadHash = nullptr;
    if (m_idleTimer) {
        m_idleTimer->stop();
        delete m_idleTimer;
        m_idleTimer = nullptr;
    }
}

QString UpdateManager::repoSlug()
{
    // 产品身份单源：owner/repo 只在这里出现一次，URL 一律经 UpdateRules::releaseApiUrl 拼。
    return QStringLiteral("mowangshuying/lite-harness");
}

QString UpdateManager::updateDirPath() const
{
    return LitePaths::updateDirFor(LitePaths::configRoot());
}

QString UpdateManager::stagingDirPath() const
{
    return LitePaths::updateStagingDirFor(LitePaths::configRoot());
}

QString UpdateManager::archivePath() const
{
    return LitePaths::updateArchivePathFor(LitePaths::configRoot());
}

QString UpdateManager::markerPath() const
{
    return LitePaths::updateMarkerPathFor(LitePaths::configRoot());
}

QString UpdateManager::installRootPath()
{
    // 安装根 = exe 目录的上一级（发布包布局 <root>/bin、<root>/config、
    // <root>/plugins、<root>/translations）。
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/.."));
}

UpdateManager::LayoutFacts UpdateManager::probeLayout()
{
    // 三项事实只在这里探一次，两个出口共用同一份值。
    LayoutFacts facts;
    const QString exeDir = QCoreApplication::applicationDirPath();
    // 发布包实测 bin/ 里有 Qt6Core.dll；开发态 build/bin 实测 0 个 Qt6*.dll（windeployqt
    // 只在 CPack staging 里跑）。这条探测就是开发态与发布包的区分判据。
    facts.hasQtRuntime = QFileInfo::exists(exeDir + QStringLiteral("/Qt6Core.dll"));
    facts.hasConfigRoot = QFileInfo(LitePaths::configRoot()).isDir();
    facts.writable = QFileInfo(installRootPath()).isWritable();
    return facts;
}

bool UpdateManager::installLooksLikeRelease()
{
    const LayoutFacts facts = probeLayout();
    return UpdateRules::canSelfUpdate(facts.hasQtRuntime, facts.hasConfigRoot, facts.writable);
}

QString UpdateManager::selfUpdateRefusal()
{
    const LayoutFacts facts = probeLayout();
    return UpdateRules::refusalReason(facts.hasQtRuntime, facts.hasConfigRoot, facts.writable);
}

void UpdateManager::setStage(UpdateManager::Stage stage, const QString &detail)
{
    m_stage = stage;
    emit stageChanged(stage, detail);
}

void UpdateManager::cancel()
{
    m_cancelled = true;
    if (m_reply)
        m_reply->abort();
    if (m_extractProcess && m_extractProcess->state() != QProcess::NotRunning)
        m_extractProcess->kill();
    setStage(Stage::Idle, QStringLiteral("cancelled"));
}

// ---------------------------------------------------------------- ① 检查

void UpdateManager::checkLatest()
{
    if (!installLooksLikeRelease()) {
        // 开发态/只读安装直接拒，并给出可辨原因——不能让它长得像「没有新版本」。
        emit checkFinished(false, QString(), selfUpdateRefusal());
        return;
    }
    const QString url = UpdateRules::releaseApiUrl(repoSlug());
    if (url.isEmpty()) {
        emit checkFinished(false, QString(), QStringLiteral("bad_repo_slug"));
        return;
    }

    m_cancelled = false;
    setStage(Stage::Checking, QString());

    QNetworkRequest req{QUrl(url)};
    // UA 是硬要求（本机实测：不带 UA 被 GitHub 直接拒答，而该失败在 UI 上与
    // 「已是最新」「网络不通」三者同形）。
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  UpdateRules::userAgentFor(QCoreApplication::applicationVersion()));
    req.setTransferTimeout(kCheckTransferTimeoutMs);

    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const bool cancelled = m_cancelled;
        reply->deleteLater();
        if (cancelled)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = QStringLiteral("network: %1").arg(reply->errorString());
            setStage(Stage::Failed, detail);
            emit checkFinished(false, QString(), detail);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            setStage(Stage::Failed, QStringLiteral("bad_json"));
            emit checkFinished(false, QString(), QStringLiteral("bad_json"));
            return;
        }
        const QJsonObject release = doc.object();
        m_remoteTag = release.value(QStringLiteral("tag_name")).toString();
        const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();

        // 只取 win64 资产（判据单源纯头）：将来 Release 若挂 mac/linux 包，
        // 「取第一个 zip」会把错平台的包拖下来并在校验前就动手。
        QStringList names;
        for (const QJsonValue &v : assets)
            names.append(v.toObject().value(QStringLiteral("name")).toString());
        const QString assetName = UpdateRules::pickAssetName(names, m_remoteTag);
        if (assetName.isEmpty()) {
            setStage(Stage::Failed, QStringLiteral("no_win64_asset"));
            emit checkFinished(false, m_remoteTag, QStringLiteral("no_win64_asset"));
            return;
        }
        QJsonObject chosen;
        for (const QJsonValue &v : assets) {
            const QJsonObject asset = v.toObject();
            if (asset.value(QStringLiteral("name")).toString() == assetName) {
                chosen = asset;
                break;
            }
        }
        m_assetUrl = chosen.value(QStringLiteral("browser_download_url")).toString();
        // digest 缺失即拒绝：没有校验和就不换体（纯头 digestMatches 的 fail-closed）。
        m_apiDigest = UpdateRules::normalizeDigest(chosen.value(QStringLiteral("digest")).toString());
        m_assetSize = UpdateRules::assetSizeOrUnknown(
            static_cast<qint64>(chosen.value(QStringLiteral("size")).toDouble()));
        if (m_apiDigest.isEmpty() || m_assetUrl.isEmpty()) {
            setStage(Stage::Failed, QStringLiteral("no_digest"));
            emit checkFinished(false, m_remoteTag, QStringLiteral("no_digest"));
            return;
        }

        const bool hasUpdate = UpdateRules::isUpdateAvailable(
            QCoreApplication::applicationVersion(), m_remoteTag);
        setStage(Stage::Idle, hasUpdate ? QStringLiteral("update_available")
                                        : QStringLiteral("up_to_date"));
        emit checkFinished(hasUpdate, m_remoteTag, QString());
    });
}

// ---------------------------------------------------- ②③④ 下载 / 校验 / 解压

void UpdateManager::stageUpdate(const QString &assetUrl, const QString &apiDigest, qint64 declaredSize)
{
    m_apiDigest = UpdateRules::normalizeDigest(apiDigest.isEmpty() ? m_apiDigest : apiDigest);
    m_assetUrl = assetUrl.isEmpty() ? m_assetUrl : assetUrl;
    m_assetSize = declaredSize > 0 ? declaredSize : m_assetSize;
    if (m_apiDigest.isEmpty() || m_assetUrl.isEmpty()) {
        setStage(Stage::Failed, QStringLiteral("no_digest"));
        emit staged(false, QStringLiteral("no_digest"));
        return;
    }

    QDir().mkpath(updateDirPath());
    QFile::remove(archivePath()); // 上次失败的残档不得参与本次校验

    m_cancelled = false;
    m_downloadHash = new QCryptographicHash(QCryptographicHash::Sha256);
    m_downloadFile = new QFile(archivePath());
    if (!m_downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString detail = QStringLiteral("open_failed: %1").arg(m_downloadFile->errorString());
        delete m_downloadFile;
        m_downloadFile = nullptr;
        delete m_downloadHash;
        m_downloadHash = nullptr;
        setStage(Stage::Failed, detail);
        emit staged(false, detail);
        return;
    }

    setStage(Stage::Downloading, QString());

    QNetworkRequest req{QUrl(m_assetUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  UpdateRules::userAgentFor(QCoreApplication::applicationVersion()));
    m_reply = m_net->get(req);

    // 静默看门狗：大文件不设总时限，但链路死掉必须能收口。
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(kDownloadIdleTimeoutMs);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        if (m_reply)
            m_reply->abort();
    });
    m_idleTimer->start();

    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (m_idleTimer)
            m_idleTimer->start(); // 有进账即续命
        emit progressChanged(received, total > 0 ? total : m_assetSize);
    });
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_downloadFile || !m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        if (chunk.isEmpty())
            return;
        if (m_downloadHash)
            m_downloadHash->addData(chunk); // 边下边算：55MB 不二次全量读盘
        if (m_downloadFile->write(chunk) != chunk.size()) {
            m_downloadFile->close();
            if (m_reply)
                m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() { onArchiveWritten(); });
}

void UpdateManager::onArchiveWritten()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const bool cancelled = m_cancelled;
    if (reply)
        reply->deleteLater();
    if (m_idleTimer) {
        m_idleTimer->stop();
        m_idleTimer->deleteLater();
        m_idleTimer = nullptr;
    }
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
    if (cancelled) {
        QFile::remove(archivePath());
        setStage(Stage::Idle, QStringLiteral("cancelled"));
        return;
    }
    if (reply && reply->error() != QNetworkReply::NoError) {
        const QString detail = QStringLiteral("download: %1").arg(reply->errorString());
        QFile::remove(archivePath());
        delete m_downloadHash;
        m_downloadHash = nullptr;
        setStage(Stage::Failed, detail);
        emit staged(false, detail);
        return;
    }

    setStage(Stage::Verifying, QString());
    const QString computed = m_downloadHash
        ? QString::fromLatin1(m_downloadHash->result().toHex())
        : QString();
    delete m_downloadHash;
    m_downloadHash = nullptr;

    if (!UpdateRules::digestMatches(computed, m_apiDigest)) {
        // 校验不过：删档、绝不碰安装树。这是唯一能挡住「包被截断/被替换」的一步。
        QFile::remove(archivePath());
        setStage(Stage::Failed, QStringLiteral("digest_mismatch"));
        emit staged(false, QStringLiteral("digest_mismatch"));
        return;
    }

    startExtraction();
}

void UpdateManager::startExtraction()
{
    // 清暂存 → tar 解包。tar.exe 是 Windows 自带 bsdtar（本机实测在位且能列 zip 内容）。
    QDir staging(stagingDirPath());
    if (staging.exists())
        staging.removeRecursively();
    QDir().mkpath(stagingDirPath());

    m_extractProcess = new QProcess(this);
    connect(m_extractProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                if (m_extractProcess) {
                    m_extractProcess->deleteLater();
                    m_extractProcess = nullptr;
                }
                onExtractFinished(code);
            });
    connect(m_extractProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_extractProcess) {
            m_extractProcess->deleteLater();
            m_extractProcess = nullptr;
        }
        setStage(Stage::Failed, QStringLiteral("tar_spawn_failed"));
        emit staged(false, QStringLiteral("tar_spawn_failed"));
    });
    setStage(Stage::Extracting, QString());
    m_extractProcess->start(QStringLiteral("tar"),
                            {QStringLiteral("-xf"), archivePath(),
                             QStringLiteral("-C"), stagingDirPath()});
}

void UpdateManager::onExtractFinished(int exitCode)
{
    QFile::remove(archivePath()); // 已解出，原档不留（它是最占盘的那份）
    if (exitCode != 0) {
        const QString detail = QStringLiteral("tar_exit_%1").arg(exitCode);
        setStage(Stage::Failed, detail);
        emit staged(false, detail);
        return;
    }

    // 枚举暂存 → 推导包内根 → 生成换体计划。判据全在纯头：任一条目落不进白名单，
    // planSwap 返回空计划，这里整体拒绝——半份计划换体留下「一部分新一部分旧」的运行态，
    // 比不更新糟糕得多。
    // 相对路径必须经 QDir::relativeFilePath 求：QDirIterator::filePath(const QString &)
    // 的入参是「相对迭代器当前目录的文件名」，把绝对暂存根塞进去得到的是垃圾路径
    // （本文件初稿就如此，后果是计划恒空、表现为「永远更新失败」）。
    QStringList stagedRelPaths;
    const QDir stagingRootIt(stagingDirPath());
    QDirIterator it(stagingDirPath(), QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        stagedRelPaths.append(nativeToPortable(stagingRootIt.relativeFilePath(it.filePath())));
    }
    m_packageRoot = UpdateRules::derivePackageRoot(stagedRelPaths);
    m_plan = UpdateRules::planSwap(stagedRelPaths, m_packageRoot);
    if (m_packageRoot.isEmpty() || m_plan.isEmpty()) {
        QDir(stagingDirPath()).removeRecursively();
        setStage(Stage::Failed, QStringLiteral("package_layout_rejected"));
        emit staged(false, QStringLiteral("package_layout_rejected"));
        return;
    }

    setStage(Stage::ReadyToSwap, QString());
    emit staged(true, QString());
}

// ------------------------------------------------------ ⑤⑥ 换体 / 进程交接

bool UpdateManager::applySwap(QString *error)
{
    // 失败信息一律先落本地变量，再按指针非空才写出——直接写 *error 会在调用方
    // 传 nullptr 时解引用空指针（本文件初稿就踩了这一下）。
    auto fail = [&](const QString &reason) -> bool {
        if (error)
            *error = reason;
        setStage(Stage::Failed, reason);
        return false;
    };

    if (m_plan.isEmpty())
        return fail(QStringLiteral("not_staged"));
    if (!installLooksLikeRelease())
        return fail(selfUpdateRefusal());

    // 跨进程互斥：本仓没有单实例护栏（实测两个实例可同时运行），并发换体会互相踩。
    QDir().mkpath(updateDirPath());
    QLockFile lock(joinRoot(updateDirPath(), QStringLiteral("swap.lock")));
    lock.setStaleLockTime(kSwapLockStaleMs);
    if (!lock.tryLock(0))
        return fail(QStringLiteral("another_instance_swapping"));

    const QString installRoot = installRootPath();
    const QString stagingRoot = stagingDirPath();

    QVector<UpdateRules::SwapItem> renamed; // 已改名 → 回滚要改回
    QVector<UpdateRules::SwapItem> written; // 已写入 → 回滚要删掉

    auto rollback = [&]() {
        // 逆序：先删新写入的，再把备份改回原位。顺序反了会把新体当旧体还原。
        for (int i = written.size() - 1; i >= 0; --i)
            QFile::remove(joinRoot(installRoot, written.at(i).liveTarget));
        for (const QString &backupRel : UpdateRules::rollbackOrder(renamed)) {
            const QString original =
                backupRel.left(backupRel.size() - UpdateRules::backupSuffix().size());
            QFile::rename(joinRoot(installRoot, backupRel), joinRoot(installRoot, original));
        }
    };

    setStage(Stage::Swapping, QString());

    // 第一步：全部改名腾位。实测运行中的 exe 可改名，所以这步不要求先退出。
    for (const UpdateRules::SwapItem &item : m_plan) {
        const QString live = joinRoot(installRoot, item.liveTarget);
        if (!QFileInfo::exists(live))
            continue; // 新增文件：无旧体可让位
        const QString backup = joinRoot(installRoot, item.backupPath);
        QFile::remove(backup); // 上次崩溃留下的同名备份先清掉，否则改名必失败
        if (!QFile::rename(live, backup)) {
            rollback();
            return fail(QStringLiteral("rename_failed: %1").arg(item.liveTarget));
        }
        renamed.append(item);
    }

    // 第二步：把新体写进原位。
    for (const UpdateRules::SwapItem &item : m_plan) {
        const QString dst = joinRoot(installRoot, item.liveTarget);
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(joinRoot(stagingRoot, item.stagedSource), dst)) {
            rollback();
            return fail(QStringLiteral("copy_failed: %1").arg(item.liveTarget));
        }
        written.append(item);
    }

    // 第三步：marker 必须在换体成功之后写——它描述的是「实际存在的残留」。
    // 提前写会在回滚后留下指向不存在备份的 marker；而写不进去时不回滚
    // （新体已在位，回滚反而把用户推回更差的状态），只留痕。
    QStringList backupRels;
    for (const UpdateRules::SwapItem &item : renamed)
        backupRels.append(item.backupPath);
    QJsonObject markerJson;
    UpdateRules::writeMarker(&markerJson, QCoreApplication::applicationVersion(), m_remoteTag,
                             m_packageRoot, backupRels);
    QFile markerFile(markerPath());
    if (!markerFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_WARN << "[update] marker write failed; backups will linger:" << markerFile.errorString();
    } else {
        markerFile.write(QJsonDocument(markerJson).toJson(QJsonDocument::Compact));
        markerFile.close();
    }

    QDir(stagingRoot).removeRecursively();

    // 换体到此为止，**不在此处重启**：重启必须走 requestRestartAfterSwap()，
    // 否则会绕过主窗口 closeEvent 的「任务仍在运行」守卫，把未完成回合当场丢掉。
    setStage(Stage::Swapped, QString());
    LOG_INFO << "[update] swapped to" << m_remoteTag << "; restart pending";
    return true;
}

bool UpdateManager::requestRestartAfterSwap(QWidget *window)
{
    // 复用 I18n::requestRestart 的同一条路径：先让主窗口 closeEvent 的守卫过一遍
    // （「任务仍在运行」确认框 + 逐页 stop()）。自更新不是这条守卫的豁免理由。
    if (!window)
        return false;
    if (!window->close())
        return false; // 用户取消：换体已完成、marker 在位，下次正常启动自然生效并清残留

    // 刻意带 workingDirectory：FluLogUtils 的日志路径是 `../log/` 相对 CWD，
    // 不指目录的话新实例会把日志写进别处（本机实测踩过）。
    const bool launched = QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        QCoreApplication::arguments().mid(1),
        QCoreApplication::applicationDirPath());
    if (!launched) {
        // 换体已成功，只是没能拉起新实例：不能谎报成「更新失败」让用户以为要重装。
        LOG_WARN << "[update] swap ok but relaunch spawn failed; user must restart manually";
        return false;
    }
    // 末窗关闭已同步触发 quit()（exitCode=0），而 exit() 先同步改写 exitCode、
    // 事件循环处理 Quit 时才读取——故 932 覆盖 0，与语言重启的 931 同款语义。
    setStage(Stage::Restarting, QString());
    LOG_INFO << "[update] exiting for restart into" << m_remoteTag;
    qApp->exit(kRestartForUpdateExitCode);
    return true;
}

// ---------------------------------------------------- ⑦ 下次启动清理残留

void UpdateManager::cleanupResidueAtStartup()
{
    const QString markerFile = LitePaths::updateMarkerPathFor(LitePaths::configRoot());
    if (!QFileInfo::exists(markerFile))
        return; // 常态：没有 marker，零开销

    QFile file(markerFile);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN << "[update] marker unreadable, leaving residue alone:" << file.errorString();
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        LOG_WARN << "[update] marker is not a json object; keeping residue for manual review";
        return;
    }
    const UpdateRules::Marker marker = UpdateRules::parseMarker(doc.object());
    if (!marker.valid) {
        // 判据不过 → 一个文件都不删。备份是旧版唯一副本，宁留不误删。
        LOG_WARN << "[update] marker rejected by UpdateRules::parseMarker; residue kept";
        return;
    }
    const QString current = QCoreApplication::applicationVersion();
    if (!UpdateRules::shouldCleanResidue(marker.toVersion, current)) {
        LOG_WARN << "[update] running" << current << "but marker claims" << marker.toVersion
                 << "; backups kept (they are the only working copy)";
        return;
    }

    const QString installRoot = installRootPath();
    int removed = 0;
    for (const QString &rel : marker.backups) {
        const QString abs = joinRoot(installRoot, rel);
        if (QFile::remove(abs))
            ++removed;
        else
            LOG_WARN << "[update] could not remove residue (still locked? retry next start):" << abs;
    }
    // 暂存与原档一并收尾；marker 最后删——中途崩溃则下次重来（幂等）。
    QDir(LitePaths::updateStagingDirFor(LitePaths::configRoot())).removeRecursively();
    QFile::remove(LitePaths::updateArchivePathFor(LitePaths::configRoot()));
    QFile::remove(markerFile);
    if (removed > 0)
        LOG_INFO << "[update] cleaned" << removed << "residue files after upgrade to" << current;
}
