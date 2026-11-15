#pragma once

// 自更新编排层：检查 → 下载 → 校验 → 解压 → 换体 → 重启 → 下次启动清理残留。
//
// 分工纪律（与 ProjectInstructions.h / AgentLoopInstructions.cpp 同构）：
// 所有「算得出但做错」的判断一律在 UpdateRules.h（纯头、可 ctest）；本类只做 IO、
// 状态推进与进程交接。本类链 Qt6::Network，因而**永不进 ctest 清单**——
// 若有人把判据写回这里，它就当场失去全部直测覆盖。
//
// 全链零嵌套事件循环：网络走 QNetworkReply 信号、解压走 QProcess::finished 信号，
// 没有 waitForFinished、没有局部 QEventLoop（本仓已把阻塞族整体删除，见 doc.md 第五部分）。
//
// 换体路线的三条实测依据（不是猜测，都是本机验过的）：
//   · 改名正在运行的 exe —— OK
//   · 在原路径写新文件 —— OK
//   · 删除改名后仍在运行的镜像 —— 拒绝访问
//   ⇒ 所以只能「改名腾位 → 写入新体 → 重启 → 由新实例删残留」，不能当场删。
// 而「装到同级新目录」那条更省事的路线已被实测否掉：configRoot = <exe>/../config，
// 换目录等于换数据根，用户会话/记忆/设置会当场「消失」。故必须原地换体。

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "UpdateRules.h"

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QTimer;
class QProcess;
class QCryptographicHash;
class QWidget; // requestRestartAfterSwap 的形参；定义在 .cpp 里 include（此处只需指针）

class UpdateManager : public QObject
{
    Q_OBJECT
public:
    // 状态机的阶段。UI 只按阶段渲染文案，不自行推断进度。
    enum class Stage {
        Idle,
        Checking,
        Downloading,
        Verifying,
        Extracting,
        ReadyToSwap,   // 包已落地并校验通过，尚未触碰安装树
        Swapping,
        Swapped,       // 换体已完成、marker 已在位，但尚未重启（用户可稍后正常重启生效）
        Restarting,
        Failed,
    };
    Q_ENUM(Stage)

    explicit UpdateManager(QObject *parent = nullptr);
    ~UpdateManager() override;

    // 产品身份单源：owner/repo 只在这里出现一次，URL 拼装经 UpdateRules::releaseApiUrl。
    static QString repoSlug();

    // 安装布局探测：三项事实只在这里探一次，两个出口共用同一份值——
    // 各写一遍就会有一处过期，而「能不能更新」与「为什么不能」必须来自同一次观测。
    struct LayoutFacts {
        bool hasQtRuntime = false;
        bool hasConfigRoot = false;
        bool writable = false;
    };
    static LayoutFacts probeLayout();
    static bool installLooksLikeRelease();
    static QString selfUpdateRefusal();

    // ① 检查：异步 GET releases/latest。结果经 checkFinished 交付。
    void checkLatest();

    // ②③④ 下载 + 校验 + 解压到暂存区。全程不触碰安装树，失败即弃，可随时 cancel()。
    void stageUpdate(const QString &assetUrl, const QString &apiDigest, qint64 declaredSize);

    // ⑤ 换体 + 写 marker。唯一不可逆步骤，调用方必须先拿到用户明确确认；内部 fail-closed：
    // 任一步失败即按逆序回滚，回滚成功后安装树回到换体前状态。
    // 刻意**不含重启**：重启必须走 requestRestartAfterSwap()，否则就绕过了主窗口的
    // 「任务仍在运行」退出守卫——那是丢未完成回合的数据丢失级事故，自更新不是豁免理由。
    bool applySwap(QString *error);

    // ⑥ 重启交接：复用 I18n::requestRestart 的同一条路径（先 window->close() 过守卫，
    // 守卫取消则不重启，此时换体已完成、marker 已在位，下次正常启动自然清理并生效）。
    // 与语言重启的唯一差异：退出码 932（区分两种自重启）+ 显式带 workingDirectory
    // （FluLogUtils 的日志路径是 `../log/` 相对 CWD，不指目录新实例会把日志写进别处）。
    bool requestRestartAfterSwap(QWidget *window);

    // ⑦ 下次启动时清残留（App.cpp 调用）。marker 无效或版本闸门不过 → 一个文件都不删。
    static void cleanupResidueAtStartup();

    Stage stage() const { return m_stage; }
    QString remoteTag() const { return m_remoteTag; }
    QString remoteAssetUrl() const { return m_assetUrl; }
    QString remoteDigest() const { return m_apiDigest; }
    qint64 remoteSize() const { return m_assetSize; }

    void cancel();

signals:
    // hasUpdate=false 且 error 为空 = 已是最新；error 非空 = 检查失败（与「没更新」必须可辨）。
    void checkFinished(bool hasUpdate, const QString &remoteTag, const QString &error);
    void stageChanged(UpdateManager::Stage stage, const QString &detail);
    void progressChanged(qint64 received, qint64 total);
    void staged(bool ok, const QString &error);

private:
    void setStage(UpdateManager::Stage stage, const QString &detail);
    QString updateDirPath() const;
    QString stagingDirPath() const;
    QString archivePath() const;
    QString markerPath() const;
    // 静态：probeLayout() 是静态口，装根路径也只能是静态的（它只依赖 applicationDirPath）。
    // 一实例一份的话，「能不能更新」与「为什么不能」就可能来自两次不同的观测。
    static QString installRootPath();

    void onArchiveWritten();   // 下载完成 → 进校验 → 通过后起解压
    void startExtraction();    // 清暂存 + 起 tar（信号驱动，不等它）
    void onExtractFinished(int exitCode); // tar 收口 → 枚举暂存 → 出计划

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;      // 在途请求（检查或下载），cancel() 的对象
    QFile *m_downloadFile = nullptr;       // 下载落盘句柄（仅下载期非空）
    QProcess *m_extractProcess = nullptr;  // tar 子进程（仅解压期非空）
    QTimer *m_idleTimer = nullptr;         // 下载静默看门狗（有进账即续命）
    QCryptographicHash *m_downloadHash = nullptr; // 边下边算的摘要（55MB 不二次全量读盘）
    QVector<UpdateRules::SwapItem> m_plan; // 解压后生成的换体计划
    QString m_packageRoot;
    QString m_remoteTag;
    QString m_assetUrl;
    QString m_apiDigest;
    qint64 m_assetSize = -1;
    Stage m_stage = Stage::Idle;
    bool m_cancelled = false;
};
