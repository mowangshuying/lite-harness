#pragma once

// 运行时路径锚点单源头（header-only，仅 QtCore，零 IO）。
//
// 收口理由：改造前同一个逻辑存储区被三套锚点切开——settings.ini 钉 applicationDirPath()、
// 会话索引 index.json 钉进程 CWD（SessionStore::rootDirFor(QDir::currentPath())）、会话数据
// 与 skills 钉 m_workDir，FluentUI 配置还另走 CWD 相对的 ../config。启动目录一变，
// 「索引看得见、数据看不见」或反之，一个区域散在三个根上。本头把锚点收敛为一个：
//
//   <exe 目录>/../config            ← 唯一存储根（configRoot）
//   ├── settings.ini                用户配置（P1 仅搬家，仍 IniFormat；P2 与 FluentUI 配置合流）
//   ├── config.ini                  FluentUI 配置（App.cpp 恒注入本根，不再 CWD 相对）
//   ├── index.json                  会话索引
//   └── .lite-harness/              会话存储区
//       ├── skills/<name>/SKILL.md  技能库（全局共享，不再按 workDir 隔离）
//       └── sessions/<dataId>/      history.json + AgentConstants.h 登记的叶子目录
//
// 锚点选 exe 目录而非 CWD/workDir 的判据：发布包布局本就是 <pkg>/bin + <pkg>/config，
// 开发态是 build/bin + build/config——两处都已在盘上存在，零新增形态；且 exe 目录与
// settings.ini 既有锚点同根，引导期（QApplication 构造前）也能用 argv[0] 反推同一个根。
//
// 纪律：本头只做字符串派生，不 mkpath、不判存在、不读配置——落盘时机归各消费点
// （SessionStore 写索引前 mkpath、FluConfigStore::ensureDirFor 写文件前 mkpath）。
// `*For` 后缀是纯口（入参为上一级的返回值，零全局依赖，可进 ctest 直测）；
// 无参口是生产便捷口，内部现取 applicationDirPath()，无 QCoreApplication 实例当场炸——
// 与 AppSettings::reader() 同口径：静默拿到空串会把错路径永久钉进 static，比炸更糟。
#include <QCoreApplication>
#include <QDir>
#include <QString>

class LitePaths
{
public:
    LitePaths() = delete; // 纯静态锚点，禁止实例化

    // ---- 存储根 ----
    // 纯口：由 exe 目录反推存储根。入参空/全空白 → 返回空串（调用方自行决定回退，本头不猜）。
    // cleanPath 负责折叠 ".." 与统一分隔符，产出绝对形态（D:/pkg/config 而非 D:/pkg/bin/../config）。
    static QString configRootFor(const QString &exeDir)
    {
        if (exeDir.trimmed().isEmpty())
            return QString();
        return QDir::cleanPath(exeDir + QStringLiteral("/../config"));
    }

    // 生产口：存储根单源。无实例时 applicationDirPath() 返回空串 + qWarning
    // （qcoreapplication.cpp:2380-2386），configRootFor 随即给出空串——与其让空根
    // 顺着 static 缓存钉死，不如当场炸，与 AppSettings::reader() 的断言同一条理由。
    static QString configRoot()
    {
        Q_ASSERT_X(QCoreApplication::instance() != nullptr,
                   "LitePaths::configRoot",
                   "the storage root is derived from applicationDirPath(); call this only "
                   "after a QCoreApplication instance exists");
        const QString root = configRootFor(QCoreApplication::applicationDirPath());
        Q_ASSERT_X(!root.isEmpty(), "LitePaths::configRoot",
                   "applicationDirPath() yielded an empty string; the storage root is underivable");
        return root;
    }

    // ---- 存储根下的固定文件 ----
    static QString settingsFilePathFor(const QString &root)
    {
        return joined(root, QStringLiteral("settings.ini"));
    }
    static QString indexFilePathFor(const QString &root)
    {
        return joined(root, QStringLiteral("index.json"));
    }

    // ---- 会话存储区（.lite-harness 中间层）----
    static QString storeRootFor(const QString &root)
    {
        return joined(root, QStringLiteral(".lite-harness"));
    }
    static QString sessionsRootFor(const QString &root)
    {
        return joined(storeRootFor(root), QStringLiteral("sessions"));
    }
    // 技能库：全局共享一套，不随 workDir 隔离（用户裁决）。
    static QString skillsDirFor(const QString &root)
    {
        return joined(storeRootFor(root), QStringLiteral("skills"));
    }
    // 单会话数据根。空 dataId → 回退会话存储区本身，与改造前
    // AgentLoop::sessionDataRoot() 的「无 ID = 未隔离」语义逐字对齐（删除护栏依赖这条回退
    // 形态判「未隔离」，见 LiteHarness::destroySessionPage）。
    static QString sessionDataRootFor(const QString &root, const QString &dataId)
    {
        if (dataId.isEmpty())
            return storeRootFor(root);
        return joined(sessionsRootFor(root), dataId);
    }

    // ---- 无参便捷口（生产调用点用这一组）----
    static QString settingsFilePath() { return settingsFilePathFor(configRoot()); }
    static QString indexFilePath() { return indexFilePathFor(configRoot()); }
    static QString storeRoot() { return storeRootFor(configRoot()); }
    static QString sessionsRoot() { return sessionsRootFor(configRoot()); }
    static QString skillsDir() { return skillsDirFor(configRoot()); }
    static QString sessionDataRoot(const QString &dataId)
    {
        return sessionDataRootFor(configRoot(), dataId);
    }

private:
    // 拼接并归一：root 为空时炸——空根拼出来的 "/settings.ini" 在 Windows 下解析成
    // 「当前驱动器根」，是改造前那条已知错路径，本头不再提供抵达它的路径。
    static QString joined(const QString &root, const QString &leaf)
    {
        Q_ASSERT_X(!root.isEmpty(), "LitePaths::joined",
                   "the storage root must not be empty; derive it via configRoot()/configRootFor()");
        return QDir::cleanPath(root + QLatin1Char('/') + leaf);
    }
};
