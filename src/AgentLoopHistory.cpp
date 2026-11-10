// 会话历史持久化：history.json 落盘与磁盘恢复（保留 [0] system、回填孤儿 tool_call 占位、恢复模型）。
// 本函数**不再**碰会话索引——索引已并入 config.json，续活链随 lastActiveMs 一并删除（理由见末尾注释）。

#include "AgentLoop.h"
#include "LiteLog.h"

#include "AgentLoopInternal.h"
#include "CompactTranscript.h"
#include "LitePaths.h" // activityDailyPathFor 锚点单源（会话每日消耗账本落点）

#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QPair>
#include <QSaveFile>
#include <QJsonArray>

// 会话历史落盘（按会话隔离，路径派生自 sessionDataRoot）：仅写纯 wire 消息（除 [0] 系统消息），
// 展示层元数据不入库。落盘后**不再**触碰会话索引——原先这里刷一次 lastActiveMs 续活，
// 索引并入 config.json 后该字段连同续活链一并删除（判据见本函数末尾注释）。
void AgentLoop::persistHistory()
{
    // 回退路径（无 ID）与空历史：不落盘，避免污染全局 .lite-harness 目录
    if (m_sessionDataId.isEmpty() || m_messages.size() <= 1)
        return;

    QJsonArray conversation;
    for (int i = 1; i < m_messages.size(); ++i)
        conversation.append(m_messages.at(i));

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("model")] = m_model;
    root[QStringLiteral("messages")] = conversation;

    const QString path = QDir(sessionDataRoot()).filePath(QStringLiteral("history.json"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    // 失败不再静默 return：会话历史丢失属数据完整性事故，必须留痕（路径 + 原因）便于排查
    if (!file.open(QIODevice::WriteOnly))
    {
        LOG_WARN << QStringLiteral("[history] persistHistory 打开失败 %1: %2")
                                   .arg(path, file.errorString());
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        LOG_WARN << QStringLiteral("[history] persistHistory 提交失败 %1: %2")
                                   .arg(path, file.errorString());
        return;
    }

    // 此处**不再**续活会话索引（原 SessionStore::touchEntry 已随索引并入 config.json 一并删除）。
    // 理由：索引现在住在 config.json 的 app/sessions 里，「每批落盘刷一次 lastActiveMs」等于
    // **每回合重写整份配置**（含 apiToken 与 FluentUI 全部键），且文件身份一变就把宿主侧
    // SettingsCache 与 FluentUI 两侧的读缓存互相打穿——正是 §9.10 问题② 花力气修掉的那格。
    // 而全仓检索实证：lastActiveMs **没有任何读取方**（启动恢复排序用 createdMs，
    // 见 LiteHarness::restoreSessions），所以删的是字段本身而非降频——留着它只会被下一个人
    // 重新接上续活链。将来若真要做「按最近使用排序」，正确信号是这里刚写的 history.json 的
    // mtime（它本来就是每回合被写的文件，读它不需要额外写配置）。
}

// 从磁盘恢复历史到 m_messages：保留 [0] 系统消息，追加落盘消息，恢复模型。
// 关键正确性约束：assistant.tool_calls 声明的每个调用都必须紧跟其 tool 结果消息，否则
// 下一轮请求会被上游 400（tool_call_id 无匹配）。故采用"逐段 flush"策略——遍历落盘数组
// 时记录每个 assistant 声明的 tool_call_id 与已出现的 tool 结果，遇到"下一段非 tool 消息或
// 数组结束"即为该 assistant 的边界，为缺失结果的 id 就地补占位 tool 消息。
bool AgentLoop::loadSavedHistory(QString *error)
{
    if (m_sessionDataId.isEmpty())
    {
        if (error)
            *error = tr("会话未分配数据目录 ID，无法定位历史文件");
        return false;
    }

    const QString path = QDir(sessionDataRoot()).filePath(QStringLiteral("history.json"));
    // 文件不存在 = 全新会话（非错误），静默返回
    if (!QFile::exists(path))
        return false;

    QFile rf(path);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (error)
            *error = tr("无法打开历史文件：%1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        if (error)
            *error = tr("历史文件解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QString model = root.value(QStringLiteral("model")).toString();
    if (!model.isEmpty())
        m_model = model;

    const QJsonArray conversation = root.value(QStringLiteral("messages")).toArray();
    QVector<QJsonObject> rebuilt;
    if (!m_messages.isEmpty())
        rebuilt.append(m_messages.first()); // [0] 系统消息占位，稍后按恢复内容重建

    QSet<QString> presentToolIds;                       // 历史里已存在的 tool 结果 id
    QHash<QString, QPair<QString, QString>> nameArgs;   // tool_call_id -> {工具名, 参数串}
    QStringList pendingAssistantCallIds;                // 当前 assistant 声明、等待其 tool 结果 flush 的 id

    // 补占位：把 pendingAssistantCallIds 中尚未落结果的 id 生成占位 tool 消息并清空
    auto flushOrphans = [&]() {
        for (const QString &id : std::as_const(pendingAssistantCallIds))
        {
            if (presentToolIds.contains(id))
                continue;
            // C 类禁翻区（第十一轮 F3a）：本串落盘 history.json 并回灌 LLM 上下文，
            // 若走 tr() 英文界面下会把译文污染进模型输入——一律恒中文源，不进翻译
            rebuilt.append(AgentLoopDetail::makeToolResult(
                id, QStringLiteral("(恢复：工具结果不可用)")));
            presentToolIds.insert(id);
        }
        pendingAssistantCallIds.clear();
    };

    for (const QJsonValue &value : conversation)
    {
        const QJsonObject obj = value.toObject();
        const QString role = obj.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("assistant"))
        {
            flushOrphans(); // 新 assistant 边界：先补齐上一段 assistant 的缺失结果
            const QJsonArray toolCalls = obj.value(QStringLiteral("tool_calls")).toArray();
            for (const QJsonValue &c : toolCalls)
            {
                const QJsonObject co = c.toObject();
                const QString id = co.value(QStringLiteral("id")).toString();
                const QJsonObject fn = co.value(QStringLiteral("function")).toObject();
                nameArgs.insert(id, qMakePair(fn.value(QStringLiteral("name")).toString(),
                                              fn.value(QStringLiteral("arguments")).toString()));
                pendingAssistantCallIds.append(id);
            }
            rebuilt.append(obj);
        }
        else if (role == QLatin1String("tool"))
        {
            presentToolIds.insert(obj.value(QStringLiteral("tool_call_id")).toString());
            rebuilt.append(obj);
        }
        else
        {
            flushOrphans(); // user/其它角色边界：同样先补齐前段 assistant 的缺失结果
            rebuilt.append(obj);
        }
    }
    flushOrphans(); // 数组结束：处理最后一段 assistant

    m_messages = rebuilt;
    // 会话回合数播种（底部状态条「第 N 轮」）：重启恢复若不播种，恢复出来的会话会一律
    // 显示「第 1 轮」——那是撒谎（明明已经聊了十轮）。以重建后的历史里 role=user 条数为准，
    // 下一句用户发言经 run() ++ 即接上实时计数。
    // 已知偏差（刻意接受，不为本读数改 history.json schema 加计数字段）：压缩改写历史后
    // 存量 user 条数少于真实开过的回合数，此时读数偏小——口径边界已写进 getter 头注释与
    // UI tooltip；孤儿占位只补 tool 消息、不产 user，故不影响本计数
    m_userTurns = 0;
    for (const QJsonObject &m : std::as_const(rebuilt))
        if (m.value(QStringLiteral("role")).toString() == QLatin1String("user"))
            ++m_userTurns;
    rebuildSystemPromptMessage(); // 按恢复后的 workDir/会话根/技能目录重建 [0]（修1 静态化）
    // 修4：恢复会话 = 历史整体改写，prompt_tokens 锚作废（回退本地全量估算，下次 usage 重锚）
    m_tokenAnchor = -1;
    m_historyRewrittenSinceAnchor = true;
    return true;
}

// 会话重启的只读回放序列：把压缩摘要消息沿 <会话根>/.transcripts 的转写链
// 展开成被它替换掉的前序消息（判据与降级语义单源 CompactTranscript.h）。
// 红线：本函数只读——不写回 m_messages，故展开内容不进请求上下文、不进 token 计量、
// 不随 persistHistory 落盘；压缩后的历史仍是唯一发送源。失败一律保留原摘要消息。
QVector<QJsonObject> AgentLoop::replayHistoryMessages() const
{
    if (m_messages.isEmpty())
        return QVector<QJsonObject>();
    return CompactTranscript::expand(m_messages.mid(1), sessionDataRoot());
}

// 会话每日 token 消耗落账（新会话页热力图数据源，见 ActivityLedger.h）：
// 承 adoptUsageAnchor 的 usage 快照，读改写 <会话根>/.activity/daily.json，成功即 emit
// activitySessionChanged(m_sessionDataId)。卫兵与 persistHistory 同——无 dataId 不落账
// （未隔离会话不污染全局 .lite-harness 目录）。invalid 快照**在本函数入口即早退**：
// 判据仍单源在 UsageTokens::fromUsage（prompt>0），与 accumulateSnapshot / Totals::add 同一条，
// 本函数不另立判据、只是把它前移到 IO 之前——账本内容逐字节不变（无效快照本就被
// accumulateSnapshot 忽略、零值日又被回滚删除），省掉的纯粹是一次「读出原样、写回原样」
// 的空转原子重写 + 一次全量合并广播 + 一次热力图重绘。对不吐 usage 的兼容端点，
// 这是每个请求一次的主线程 IO，不是可忽略的常数。
//
// 落盘失败仅 LOG_WARN、不打断回合、不 emit——消耗账本是观测面而非承重数据，一次失败
// 至多让热力图少一格读数，不影响 history.json 与会话恢复。半写文件下次读会走
// ActivityLedger::readDaily 的「坏 JSON → 空表」降级路径，不污染聚合。
void AgentLoop::appendActivityLedger(const UsageTokens::Snapshot &snapshot)
{
    if (m_sessionDataId.isEmpty())
        return;
    // invalid 快照早退（判据单源 UsageTokens::fromUsage，与 accumulateSnapshot 同一条）：
    // 省下「读出原样、写回原样」的一次原子重写 + 全量合并广播 + 热力图重绘。
    if (!snapshot.valid)
        return;

    const QString path = LitePaths::activityDailyPathFor(sessionDataRoot());
    QDir().mkpath(QFileInfo(path).absolutePath()); // 目录首用自愈（仿 AgentConstants 惯例）

    auto data = ActivityLedger::readDaily(path);
    const QDate today = QDate::currentDate();
    ActivityLedger::DayTotal &day = data[today];
    ActivityLedger::accumulateSnapshot(day, snapshot);
    // 无效快照：accumulateSnapshot 不动 day，但 data[today] 已插入默认值 DayTotal{}。
    // 若不回滚，daily.json 会多出「零值日」条目、下次读回聚合出「有账但全 0」的假形态。
    // 判据：calls==0 即从未累加过，直接删除该键。
    if (day.calls == 0 && day.prompt == 0 && day.completion == 0)
        data.remove(today);

    if (!ActivityLedger::writeDaily(path, data)) {
        LOG_WARN << QStringLiteral("[activity] daily.json 落账失败 %1").arg(path);
        return;
    }
    emit activitySessionChanged(m_sessionDataId);
}
