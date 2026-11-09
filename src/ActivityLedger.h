#pragma once

// 会话每日 token 消耗账本纯头（header-only，仿 UsageTokens.h / CompactTokens.h 先例，
// 零 CMake 改动即入 ctest）：单会话 daily.json 读写、跨会话聚合、色阶分档、窗口截断。
//
// 为什么单独成头：本头是**新会话页热力图**（ContributionHeatmap）的数据侧，消费方是
// LiteHarness（缓存）+ NewChatPage（渲染）。判据族——坏 JSON 降级 / 缺文件返空 / 日期键
// 归一 / 跨会话求和 / 覆写往返 / valid=false 不落地 / 阈值分档边界——全是**算术与解析**，
// 与「谁在读写」无关；而 AgentLoop TU 链 QOpenAi/FluentUI，按 CMakeLists.txt「仅允许
// QtCore-only 引擎进测试清单」的硬纪律进不了 ctest。抽成纯头后 tst_activityledger 直接
// include 即测真实编译产物。
//
// 覆盖面边界（务必如实理解，勿高估这个数字）：账本只统计**主循环与队友回合**两条
// ChatStream 路径的 usage 回读；压缩摘要 / 记忆链 / workflow 的 agent() runner 走
// QOpenAi::AsyncRequest，该类型没有 usage 通道（QOpenAi.h 只有 ChatStream::usageReceived），
// 服务端回不回用量都不落地。故热力图是**部分口径**，不是真实总开销；UI 侧必须在 tooltip
// 里写明这条边界（承 UsageTokens.h 同一条纪律）。
//
// 落盘时机与原子性：AgentLoop 每次 usage 回读即读改写本会话 daily.json（QSaveFile 原子），
// 频率=每回合 1 次，IO 成本可忽略。会话级 JSON 覆写不存在膨胀问题（不同于 jsonl 追加），
// 无需清理策略。改造前无账本，热力图上线首段全空——用户裁决接受空窗、不做历史回填。
//
// 日期键口径：QDate 本地日（QDate::currentDate()），ISO 格式 yyyy-MM-dd。跨午夜归「usage
// 到达那一刻」的日期，不做时区调整——简单、符合直觉、无 DST 边界问题。

#include <QDate>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSaveFile>
#include <QString>

#include "UsageTokens.h"

namespace ActivityLedger {

// 单会话单日累计（JSON 落盘形态的五项 + calls）。
struct DayTotal {
    qint64 prompt = 0;
    qint64 completion = 0;
    qint64 cached = 0;
    qint64 reasoning = 0;
    int calls = 0;

    // 当日「总处理量」，用于色阶分档：prompt + completion。
    // 刻意不减 cached——cached 是 prompt 的子集（服务端已按全价报进 prompt），
    // 减掉会低估实际处理量；成本口径另在 tooltip 里显示 uncached = prompt - cached。
    qint64 totalProcessed() const { return prompt + completion; }
};

// ---- 色阶分档（5 档固定阈值）----
// 0=无数据 / 1=<5k / 2=<20k / 3=<50k / 4=<100k / 5=≥100k
// 固定阈值 vs 分位数自适应：固定跨会话可比、无漂移；分位数好看但同一用户不同时期色阶
// 会漂，用户读到「今天颜色浅」会误判为「今天用得少」（其实是历史多所以阈值抬高了）。
inline int levelForTokens(qint64 total)
{
    if (total <= 0)     return 0;
    if (total < 5000)   return 1;
    if (total < 20000)  return 2;
    if (total < 50000)  return 3;
    if (total < 100000) return 4;
    return 5;
}

// ---- 日期键格式单源 ----
inline QString dateKey(const QDate &date)
{
    return date.toString(Qt::ISODate);
}

inline QDate dateFromKey(const QString &key)
{
    // 严格 ISO yyyy-MM-dd：长度 10、位置 4/7 是 '-'、其余全数字。
    // Qt::ISODate 本身对分隔符与位宽宽容（"2026/11/07"、"2026-1-1" 都解析成功），本头不采——
    // 手改文件的斜杠或单位数写法若被静默接受、下次覆写会归一到 dash 双位形态，用户看不出
    // 自己改了什么；严格判据让「坏键跳过、好键照常读回」这条降级路径可预测。
    if (key.size() != 10)
        return QDate();
    if (key.at(4) != QLatin1Char('-') || key.at(7) != QLatin1Char('-'))
        return QDate();
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7)
            continue;
        if (!key.at(i).isDigit())
            return QDate();
    }
    const int year  = key.left(4).toInt();
    const int month = key.mid(5, 2).toInt();
    const int day   = key.mid(8, 2).toInt();
    // QDate(y,m,d) 对越界参数直接构造出无效日期（Qt6 无静态 isValidDate），
    // 用它做二次校验即可拒掉 "2026-13-45" 这类形态合规、语义非法的串。
    const QDate parsed(year, month, day);
    return parsed.isValid() ? parsed : QDate();
}

// ---- 累加一条快照到当日账 ----
// 与 UsageTokens::Totals::add 同判据：invalid 快照直接忽略，绝不把「没数据」算成「0 消耗」。
inline void accumulateSnapshot(DayTotal &day, const UsageTokens::Snapshot &snapshot)
{
    if (!snapshot.valid)
        return;
    day.prompt += snapshot.prompt;
    day.completion += snapshot.completion;
    day.cached += snapshot.cached;
    day.reasoning += snapshot.reasoning;
    ++day.calls;
}

// ---- 读单会话 daily.json ----
// 文件缺失 / 打不开 / 坏 JSON / 顶层非对象 / 键非 ISO 日期 / 值非对象 / 数值字段为负
// 一律降级为空表或跳过该条，绝不抛、绝不猜。这是新会话页启动扫盘的容错底线——
// 一个坏会话文件不该把整个热力图打挂。
inline QHash<QDate, DayTotal> readDaily(const QString &path)
{
    QHash<QDate, DayTotal> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return result;

    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QDate date = dateFromKey(it.key());
        if (!date.isValid())
            continue;
        if (!it.value().isObject())
            continue;
        const QJsonObject obj = it.value().toObject();
        DayTotal day;
        day.prompt     = qint64(obj.value(QStringLiteral("prompt")).toDouble());
        day.completion = qint64(obj.value(QStringLiteral("completion")).toDouble());
        day.cached     = qint64(obj.value(QStringLiteral("cached")).toDouble());
        day.reasoning  = qint64(obj.value(QStringLiteral("reasoning")).toDouble());
        day.calls      = int(obj.value(QStringLiteral("calls")).toDouble());
        // 脏数据（手改文件、断电半写）负值归零，不污染聚合与色阶
        if (day.prompt     < 0) day.prompt     = 0;
        if (day.completion < 0) day.completion = 0;
        if (day.cached     < 0) day.cached     = 0;
        if (day.reasoning  < 0) day.reasoning  = 0;
        if (day.calls      < 0) day.calls      = 0;
        result.insert(date, day);
    }
    return result;
}

// ---- 原子写 ----
// 无效日期键跳过；失败返 false（不抛）。QSaveFile 保证覆写原子性——半写文件
// 下次读会走「坏 JSON → 空表」降级路径，不会污染聚合。
inline bool writeDaily(const QString &path, const QHash<QDate, DayTotal> &data)
{
    QJsonObject root;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (!it.key().isValid())
            continue;
        QJsonObject obj;
        obj.insert(QStringLiteral("prompt"),     double(it.value().prompt));
        obj.insert(QStringLiteral("completion"), double(it.value().completion));
        obj.insert(QStringLiteral("cached"),     double(it.value().cached));
        obj.insert(QStringLiteral("reasoning"),  double(it.value().reasoning));
        obj.insert(QStringLiteral("calls"),      it.value().calls);
        root.insert(dateKey(it.key()), obj);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

// ---- 跨会话求和 ----
// 用于新会话页显示全局热力图；LiteHarness 侧维护 QHash<dataId, QHash<date, DayTotal>>
// 每会话一份，聚合按需算（会话数十几个、日期数千把，成本可忽略）。
inline QHash<QDate, DayTotal> aggregate(const QList<QHash<QDate, DayTotal>> &perSession)
{
    QHash<QDate, DayTotal> result;
    for (const auto &session : perSession) {
        for (auto it = session.constBegin(); it != session.constEnd(); ++it) {
            DayTotal &day = result[it.key()];
            day.prompt     += it.value().prompt;
            day.completion += it.value().completion;
            day.cached     += it.value().cached;
            day.reasoning  += it.value().reasoning;
            day.calls      += it.value().calls;
        }
    }
    return result;
}

// ---- 窗口截断 ----
// 保留 [from, to] 闭区间内的日期。热力图渲染只关心最近 371 天（53 列 × 7 行满矩形，
// 末格恒为今天），读侧一次性截断，
// 免得 paintEvent 每格判一次。
inline QHash<QDate, DayTotal> windowFilter(const QHash<QDate, DayTotal> &data,
                                            const QDate &from, const QDate &to)
{
    QHash<QDate, DayTotal> result;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (it.key() >= from && it.key() <= to)
            result.insert(it.key(), it.value());
    }
    return result;
}

} // namespace ActivityLedger
