#pragma once

// ============================================================================
// 会话索引读写单源（header-only 静态方法集，无状态、无实例）
//
// 【本头为什么存在，以及它现在写进哪个文件】
// 索引原先是存储根下一个独立的 index.json，由本头用裸 QFile/QSaveFile 整表读写。
// 现在它并入唯一的配置文件 `<config>/config.json`，落在宿主键 `app/sessions` 下
// （盘上形态 `{"app":{"sessions":[{...},{...}]}}`），存储根本级从此只剩一份 json。
//
// 【改动的硬约束：读写必须整体换轨，不能一半新一半旧】
// 并入 config.json 后，同一个文件同时被三个 owner 写：宿主配置（AppSettings）、
// FluentUI（FluConfigUtils）、会话索引（本头）。Qt 6.9 源码实证多个 QSettings 实例
// 写同一路径不会互覆——QConfFile 缓存键只有 absPath，syncConfFile 写前先 refresh 再
// merge，全程 QLockFile 保护（对表断言见 tests/tst_configmerge 组 B）。
// 但这条保证**只对 QSettings 之间存在**：裸 QSaveFile 整文件重写不参与那张表的合并，
// 它会把另两个 owner 刚写进盘上的键一起覆盖掉。所以本头一律经 SettingsCache::Reader
// （内部就是一个配对好格式与折叠口径的 QSettings），**严禁再出现裸文件写**。
// 读侧同理走 Reader：它缓存的是解析产物，写后立即失效，与直取逐键一致。
//
// 【为什么入参是 Reader 而不是路径】
// 口径必须由 AppSettings::reader() 单点交出——格式（vendor 的 jsonFormat）与键折叠
// 口径（CaseSensitive）是**成对**绑定的，配错不会报错、只会让缓存与直取给出两个不同
// 答案（SettingsCache.h 不变量④）。若本头自己收一个路径再自己拼格式，就等于开了第二
// 处口径来源，正是那条不变量禁止的形态。测试要注入落点，就自己构造一个配对好的 Reader
// （见 tests/tst_sessionstore），生产调用点一律传 AppSettings::reader()。
//
// 【lastActiveMs 已随本次合并一并删除——不是遗漏，是刻意的】
// 原先每条会话带一个 lastActiveMs，由 AgentLoop::persistHistory() 在**每批历史落盘后**
// 续活一次（9 个调用点，回合级频率）。改造前那无所谓——它重写的是一个几百字节的独立
// 小文件。并入 config.json 后同样的频率意味着：每回合重写整份配置（含 apiToken 与
// FluentUI 全部键），且文件身份一变就把宿主与 FluentUI 两侧的读缓存互相打穿（这正是
// §9.10 问题② 花力气修掉的东西）。
// 全仓检索实证：**没有任何一处读取 lastActiveMs**——启动恢复排序用的是 createdMs
// （LiteHarness::restoreSessions）。一条无人消费的高频整表重写，删掉零功能损失，
// 因此这里删的是字段本身而不是给它降频：留着它只会被下一个人重新接上续活链。
// 若将来真要做「按最近使用排序」，正确信号是 sessions/<dataId>/history.json 的 mtime
// （它本来就是每回合被写的那个文件，读它不需要额外写配置）。
//
// 【坏形态一律降级为空索引，不报错】
// 用户手改 config.json 时把 sessions 写成对象（`{"dataId":...}` 而非 `[{...}]`）是
// 可预期的失误。vendor 的 flatten() 只对 object 递归，数组整体落成 QVariantList；
// 写成对象会得到 QVariantMap，本头认不出列表形态即回空索引——会话列表变空，但
// sessions/<dataId>/history.json 一个字节都没动，改回数组就全部回来。
// 实测（Qt 6.9）：qint64 毫秒时间戳与中文标题经这条路径往返逐字保真。
// ============================================================================

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "AppSettings.h"      // scopedKey()：宿主键组前缀单点（纯字符串，不触盘、不要求 app 实例）
#include "LiteLog.h"          // 索引回写失败的统一告警
#include "SettingsCache.h"    // Reader：本头唯一的读写通道

class SessionStore
{
public:
    // 索引在配置文件里的裸键名；消费点一律经 AppSettings::value()/setValue() 或本头，
    // 组前缀由 scopedKey() 单点拼上（散到调用点就会在盘上开出第二个顶层 sessions）。
    static QString sessionsKey()
    {
        return QStringLiteral("sessions");
    }

    static QString scopedSessionsKey()
    {
        return AppSettings::scopedKey(sessionsKey());
    }

    // 读索引：缺失 / 非列表形态 / 坏文件 → 一律空表（调用方据此判空，不作错误处理）。
    // 判据只认 QVariantList：json 后端的数组正是这个形态（QJsonValue::toVariant() 的产物，
    // 不是 QStringList——同一条坑见 AgentConst::normalizedText 的注释）。
    static QVariantList loadIndex(const SettingsCache::Reader &reader)
    {
        const QVariant stored = reader.value(scopedSessionsKey());
        if (stored.metaType() != QMetaType::fromType<QVariantList>())
            return QVariantList(); // 缺失（invalid）或被手改成对象/标量：降级，不猜
        return stored.toList();
    }

    // 写索引：整表回写。落盘与缓存失效在 Reader::setValue 里成对发生（不变量③）。
    static bool saveIndex(const SettingsCache::Reader &reader, const QVariantList &entries)
    {
        return reader.setValue(scopedSessionsKey(), entries);
    }

    // 登记/刷新单条会话索引项：命中 dataId 就地更新（title/model/workDir 非空才覆盖），
    // 未命中则追加新项（createdMs 取当前时刻）。
    static void upsertEntry(const SettingsCache::Reader &reader, const QString &dataId,
                            const QString &title, const QString &model, const QString &workDir)
    {
        if (dataId.isEmpty())
            return;
        QVariantList entries = loadIndex(reader);
        for (int i = 0; i < entries.size(); ++i)
        {
            QVariantMap obj = entries.at(i).toMap();
            if (obj.value(QStringLiteral("dataId")).toString() != dataId)
                continue;
            if (!title.isEmpty())
                obj.insert(QStringLiteral("title"), title);
            if (!model.isEmpty())
                obj.insert(QStringLiteral("model"), model);
            if (!workDir.isEmpty())
                obj.insert(QStringLiteral("workDir"), workDir);
            entries[i] = obj;
            warnIfSaveFailed(saveIndex(reader, entries), QStringLiteral("upsert(更新)"));
            return;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("dataId"), dataId);
        entry.insert(QStringLiteral("title"), title);
        entry.insert(QStringLiteral("model"), model);
        entry.insert(QStringLiteral("workDir"), workDir);
        entry.insert(QStringLiteral("createdMs"), QDateTime::currentMSecsSinceEpoch());
        entries.append(entry);
        warnIfSaveFailed(saveIndex(reader, entries), QStringLiteral("upsert(追加)"));
    }

    // 删除单条会话索引项：命中 dataId 则移出并回写，返回 true；未命中返回 false（不写盘）。
    // 仅管索引条目，会话数据目录 sessions/<id>（含 history.json）由调用方决定是否清理。
    static bool removeEntry(const SettingsCache::Reader &reader, const QString &dataId)
    {
        if (dataId.isEmpty())
            return false;
        QVariantList entries = loadIndex(reader);
        for (int i = 0; i < entries.size(); ++i)
        {
            if (entries.at(i).toMap().value(QStringLiteral("dataId")).toString() != dataId)
                continue;
            entries.removeAt(i);
            warnIfSaveFailed(saveIndex(reader, entries), QStringLiteral("remove"));
            return true;
        }
        return false;
    }

    // 一次性迁移：把旧存储根下的 index.json 搬进 config.json 的 app/sessions。
    // 为什么这次**要**迁移，而存储根统一那批明确不迁移：那批搬的是「旧锚点下的历史数据」，
    // 不搬只是留孤儿目录；这批若不搬，用户下次启动会话列表直接空掉——数据明明就在同一个
    // 存储根里，只是换了个键，属可避免的功能倒退。
    // 判据与顺序（不可颠倒）：
    //   ① 旧文件不存在 → 无事可做，返回 0（绝大多数启动走这条，零成本）。
    //   ② 目标键已是列表 → 说明早已迁移过，此时**只删旧文件、绝不回写**：否则会把用户
    //      迁移后新增的会话用旧表覆盖掉（旧文件此时是残留，不是数据源）。
    //   ③ 旧文件读不出数组 → 保留它并告警，下次启动再试；不删、不写空表（写空表等于删数据）。
    //   ④ 写成功且回读条数一致 → 才删旧文件。删早了而写没落盘，索引就两头都没有了。
    static int migrateLegacyIndex(const SettingsCache::Reader &reader, const QString &legacyPath)
    {
        QFile legacy(legacyPath);
        if (!legacy.exists())
            return 0;

        const QVariant existing = reader.value(scopedSessionsKey());
        if (existing.metaType() == QMetaType::fromType<QVariantList>())
        {
            // ②：目标已有列表形态的值（含空表——空表说明用户真的删光了会话），旧文件是残留
            if (!legacy.remove())
                LOG_WARN << "SessionStore: 旧索引残留未能删除: " << legacyPath;
            return 0;
        }

        if (!legacy.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            LOG_WARN << "SessionStore: 旧索引打不开，暂不迁移: " << legacyPath;
            return -1;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(legacy.readAll());
        legacy.close();
        if (!doc.isArray())
        {
            LOG_WARN << "SessionStore: 旧索引不是数组，保留原文件待人工处理: " << legacyPath;
            return -1;
        }

        QVariantList entries;
        for (const QJsonValue &v : doc.array())
        {
            if (!v.isObject())
                continue;
            const QJsonObject obj = v.toObject();
            QVariantMap mapped;
            // 旧条目里的 lastActiveMs 不搬（本头顶部已记：无人消费，且它是高频重写的根源）
            mapped.insert(QStringLiteral("dataId"), obj.value(QStringLiteral("dataId")).toString());
            mapped.insert(QStringLiteral("title"), obj.value(QStringLiteral("title")).toString());
            mapped.insert(QStringLiteral("model"), obj.value(QStringLiteral("model")).toString());
            mapped.insert(QStringLiteral("workDir"), obj.value(QStringLiteral("workDir")).toString());
            const qint64 created = static_cast<qint64>(obj.value(QStringLiteral("createdMs")).toDouble());
            mapped.insert(QStringLiteral("createdMs"),
                          created > 0 ? created : QDateTime::currentMSecsSinceEpoch());
            entries.append(mapped);
        }

        if (!saveIndex(reader, entries))
        {
            LOG_WARN << "SessionStore: 旧索引迁移写盘失败，保留原文件下次重试: " << legacyPath;
            return -1;
        }
        // ④：回读确认条数一致才删旧文件——「写了但没落到盘」与「落错文件」都在这一步被拦住
        if (loadIndex(reader).size() != entries.size())
        {
            LOG_WARN << "SessionStore: 迁移回读条数不符，保留原文件: " << legacyPath;
            return -1;
        }
        if (!legacy.remove())
            LOG_WARN << "SessionStore: 迁移已完成但旧文件未能删除: " << legacyPath;
        LOG_INFO << "SessionStore: 已迁移旧会话索引 " << entries.size() << " 条进 config.json";
        return entries.size();
    }

    // 索引回写失败的统一告警（第六轮审计 C8）：仅告警不改控制流——history.json 会话主副本
    // 不受影响，索引只影响启动恢复清单；失败多为暂态（占用/权限），下次登记会顺带重写整表。
    static void warnIfSaveFailed(bool saved, const QString &op)
    {
        if (!saved)
            LOG_WARN << "SessionStore: " << op << " 写索引失败（config.json 键 " << scopedSessionsKey() << "）";
    }

private:
    SessionStore() = delete; // 纯静态工具，禁止实例化
};
