#pragma once

// 会话索引轻量工具（header-only 静态方法集，无状态、无实例）：
// 负责 <workDir>/.lite-harness/index.json 的读写与条目 upsert，供 LiteHarness 启动恢复
// 与新建会话登记、AgentLoop 落盘时刷新 lastActiveMs 使用。单条会话的 history.json 由
// AgentLoop 自行读写（路径派生自 sessionDataRoot），本类不介入，职责仅止于全局索引。
// 复用 CronSchedulerManager durable 落盘纪律：mkpath + QSaveFile + Indented + commit。

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>

class SessionStore
{
public:
    // 全局存储根：<workDir>/.lite-harness（索引与 skills 均在此，会话数据在其下 sessions/<id>）
    static QString rootDirFor(const QString &workDir)
    {
        return QDir(workDir).filePath(QStringLiteral(".lite-harness"));
    }
    static QString indexFilePath(const QString &root)
    {
        return root + QStringLiteral("/index.json");
    }

    // 读索引：缺失/损坏/非数组 → 返回空数组（调用方据此判空，不作错误处理）
    static QJsonArray loadIndex(const QString &root)
    {
        QFile file(indexFilePath(root));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QJsonArray();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isArray())
            return QJsonArray();
        return doc.array();
    }

    // 写索引：先建目录再原子提交，失败返回 false（与 CronSchedulerManager saveDurableJobs 同纪律）
    static bool saveIndex(const QString &root, const QJsonArray &entries)
    {
        QDir().mkpath(QFileInfo(indexFilePath(root)).absolutePath());
        QSaveFile file(indexFilePath(root));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        return file.commit();
    }

    // 登记/刷新单条会话索引项：命中 dataId 就地更新（title/model/workDir 非空才覆盖）并刷新
    // lastActiveMs；未命中则追加新项（createdMs/lastActiveMs 取当前时刻）。title/model 传空表示不改动
    static void upsertEntry(const QString &root, const QString &dataId, const QString &title,
                            const QString &model, const QString &workDir)
    {
        if (dataId.isEmpty())
            return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QJsonArray entries = loadIndex(root);
        for (int i = 0; i < entries.size(); ++i)
        {
            QJsonObject obj = entries.at(i).toObject();
            if (obj.value(QStringLiteral("dataId")).toString() != dataId)
                continue;
            if (!title.isEmpty())
                obj[QStringLiteral("title")] = title;
            if (!model.isEmpty())
                obj[QStringLiteral("model")] = model;
            if (!workDir.isEmpty())
                obj[QStringLiteral("workDir")] = workDir;
            obj[QStringLiteral("lastActiveMs")] = now;
            entries[i] = obj;
            saveIndex(root, entries);
            return;
        }
        QJsonObject entry;
        entry[QStringLiteral("dataId")] = dataId;
        entry[QStringLiteral("title")] = title;
        entry[QStringLiteral("model")] = model;
        entry[QStringLiteral("workDir")] = workDir;
        entry[QStringLiteral("createdMs")] = now;
        entry[QStringLiteral("lastActiveMs")] = now;
        entries.append(entry);
        saveIndex(root, entries);
    }

private:
    SessionStore() = delete; // 纯静态工具，禁止实例化
};
