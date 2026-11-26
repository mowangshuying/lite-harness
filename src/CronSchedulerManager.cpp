// CronSchedulerManager —— lcc s12 cron_scheduler.py 的 lite 转译（偏差登记见头文件注释块）
#include "CronSchedulerManager.h"
#include "LiteLog.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>

#include <utility>

namespace {

// python str.isdigit 的 ASCII 近似（偏差登记：python 覆盖 Unicode 数字，lite 仅认 0-9）：
// 非空且全为 ASCII 数字。"" → false（同 python "".isdigit()=False）
bool asciiDigits(const QString &text)
{
    if (text.isEmpty())
        return false;
    for (const QChar &c : text)
    {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    }
    return true;
}

// 字段预处理（validateCron 与 cronMatches **单源共用**，防「校验放行、匹配不中」漂移）：
//   - `?`：Quartz 惯例的「不指定」，只出现在日/周位，等价 `*`。本仓不强制「日/周必居其一」，
//     与 Vixie 双 `*` 通道并存——存量 `* * * * * * *` 行为不变。
//   - 周名 SUN..SAT → 0..6（沿用本仓既有编号 0=周日）；月名 JAN..DEC → 1..12。
//     大小写不敏感；逗号/区间里的名字同样展开（MON-FRI → 1-5）。
// 非日/月/周位出现字母原样交回，由 validateCronField 判非法。
QString preprocessCronField(const QString &field, int index)
{
    QString out = field;
    const bool dayField = (index == 3);
    const bool dowField = (index == 5);
    const bool monthField = (index == 4);
    if ((dayField || dowField) && out == QLatin1String("?"))
        out = QLatin1String("*");

    struct Alias { const char *name; const char *value; };
    static const Alias dowNames[] = {
        {"SUN", "0"}, {"MON", "1"}, {"TUE", "2"}, {"WED", "3"},
        {"THU", "4"}, {"FRI", "5"}, {"SAT", "6"}
    };
    static const Alias monthNames[] = {
        {"JAN", "1"},  {"FEB", "2"},  {"MAR", "3"},  {"APR", "4"},
        {"MAY", "5"},  {"JUN", "6"},  {"JUL", "7"},  {"AUG", "8"},
        {"SEP", "9"},  {"OCT", "10"}, {"NOV", "11"}, {"DEC", "12"}
    };
    if (dowField)
    {
        for (const Alias &a : dowNames)
            out.replace(QLatin1String(a.name), QLatin1String(a.value), Qt::CaseInsensitive);
    }
    else if (monthField)
    {
        for (const Alias &a : monthNames)
            out.replace(QLatin1String(a.name), QLatin1String(a.value), Qt::CaseInsensitive);
    }
    return out;
}

// 单字段校验（lcc _validate_cron_field 逐字序）：合法返回空串，否则错误文本（文案逐字）
QString validateCronField(const QString &field, int min, int max)
{
    if (field == QLatin1String("*"))
        return QString();

    // "*/N"：N 必须是正整数（lcc 用 isdigit + int<=0 判断；错误文本带整个字段含 */ 前缀）
    if (field.startsWith(QLatin1String("*/")))
    {
        const QString step = field.mid(2);
        bool ok = false;
        const int value = step.toInt(&ok);
        if (!asciiDigits(step) || !ok || value <= 0)
            return QStringLiteral("Invalid step: %1").arg(field);
        return QString();
    }

    // 逗号列表：逐项递归，第一个错误即返回（lcc part.strip()）
    if (field.contains(QLatin1Char(',')))
    {
        const QStringList parts = field.split(QLatin1Char(','));
        for (const QString &part : parts)
        {
            const QString error = validateCronField(part.trimmed(), min, max);
            if (!error.isEmpty())
                return error;
        }
        return QString();
    }

    // 区间 a-b：只按第一个 '-' 切分（lcc split("-", 1)）——负号串如 "-5" 的 start 为空
    // → 非数字 → Invalid range（与 python 同路径）
    if (field.contains(QLatin1Char('-')))
    {
        const int idx = field.indexOf(QLatin1Char('-'));
        const QString start = field.left(idx);
        const QString end = field.mid(idx + 1);
        if (!asciiDigits(start) || !asciiDigits(end))
            return QStringLiteral("Invalid range: %1").arg(field);
        const int lo = start.toInt();
        const int hi = end.toInt();
        if (lo > hi)
            return QStringLiteral("Range start is greater than end: %1").arg(field);
        if (lo < min || hi > max)
            return QStringLiteral("Range %1 is outside [%2-%3]").arg(field).arg(min).arg(max);
        return QString();
    }

    if (!asciiDigits(field))
        return QStringLiteral("Invalid field: %1").arg(field);
    const int value = field.toInt();
    if (value < min || value > max)
        return QStringLiteral("Value %1 is outside [%2-%3]").arg(value).arg(min).arg(max);
    return QString();
}

// 落盘行解析（lcc CronJob(**item) 的折叠：键集/类型不符 → 错误文本，对应 python
// ValueError/TypeError → "skipped invalid saved job"）。校验序逐字对齐 lcc：
// 构造 → validate_cron → id 前缀 → prompt 非空
bool parseSavedJob(const QJsonValue &value, CronSchedulerManager::CronJob *job, QString *error)
{
    if (!value.isObject())
    {
        *error = QStringLiteral("saved job is not an object");
        return false;
    }
    const QJsonObject obj = value.toObject();

    // dataclass(**item)：键集必须与字段一一对应，多键缺键皆 TypeError
    static const QStringList requiredKeys = {
        QStringLiteral("id"), QStringLiteral("cron"), QStringLiteral("prompt"),
        QStringLiteral("recurring"), QStringLiteral("durable"),
        QStringLiteral("pending_delivery"), QStringLiteral("last_fired")
    };
    const QStringList keys = obj.keys();
    if (keys.size() != requiredKeys.size())
    {
        *error = QStringLiteral("unexpected or missing keys");
        return false;
    }
    for (const QString &key : keys)
    {
        if (!requiredKeys.contains(key))
        {
            *error = QStringLiteral("unexpected or missing keys");
            return false;
        }
    }

    const QJsonValue id = obj.value(QStringLiteral("id"));
    const QJsonValue cron = obj.value(QStringLiteral("cron"));
    const QJsonValue prompt = obj.value(QStringLiteral("prompt"));
    const QJsonValue recurring = obj.value(QStringLiteral("recurring"));
    const QJsonValue durable = obj.value(QStringLiteral("durable"));
    const QJsonValue pending = obj.value(QStringLiteral("pending_delivery"));
    const QJsonValue lastFired = obj.value(QStringLiteral("last_fired"));
    if (!id.isString() || !cron.isString() || !prompt.isString()
        || !recurring.isBool() || !durable.isBool() || !pending.isBool()
        || !(lastFired.isNull() || lastFired.isString()))
    {
        // python dataclass 不查类型但后续逻辑崩——lite 严格化折叠为跳过（登记偏差）
        *error = QStringLiteral("wrong field types");
        return false;
    }

    job->id = id.toString();
    job->cron = cron.toString();
    job->prompt = prompt.toString();
    job->recurring = recurring.toBool();
    job->durable = durable.toBool();
    job->pendingDelivery = pending.toBool();
    job->lastFired = lastFired.isNull() ? QString() : lastFired.toString(); // "" ≡ python None

    const QString cronError = CronSchedulerManager::validateCron(job->cron);
    if (!cronError.isEmpty())
    {
        *error = cronError;
        return false;
    }
    if (!job->id.startsWith(QLatin1String("cron_")))
    {
        *error = QStringLiteral("invalid job id");
        return false;
    }
    if (job->prompt.trimmed().isEmpty())
    {
        *error = QStringLiteral("prompt cannot be empty");
        return false;
    }
    return true;
}

} // namespace

CronSchedulerManager::CronSchedulerManager(std::function<QString()> workDirSink)
    : m_workDirSink(std::move(workDirSink))
{
}

void CronSchedulerManager::start()
{
    // lcc start_runtime_threads 幂等：已启动直接返回
    if (m_runtimeStarted)
        return;
    loadDurableJobs();
    m_lastPollSec = -1; // 新装载台账的补匹配基准归零：避免跨目录沿用旧会话时刻
    m_runtimeStarted = true;
}

void CronSchedulerManager::stop()
{
    m_runtimeStarted = false; // lcc stop_runtime：仅复位标志，台账与队列留存
}

bool CronSchedulerManager::isRuntimeStarted() const
{
    return m_runtimeStarted;
}

QString CronSchedulerManager::validateCron(const QString &expression)
{
    // lcc: expression.strip().split()——任意空白分隔，simplified+SkipEmptyParts 等价
    const QStringList fields =
        expression.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() != 7)
        return QStringLiteral("Expected 7 fields, got %1").arg(fields.size());

    // 7 字段：second minute hour day-of-month month day-of-week year
    // 字段名与取值域（顺序即报错前缀）；周沿用 lite 既有 0-6 约定（0=周日），
    // 年取 [1970-2099]（与 Quartz 同域，且覆盖本机可预见运行期）
    static const char *names[7] = { "second", "minute", "hour", "day-of-month",
                                    "month", "day-of-week", "year" };
    static const int limits[7][2] = { {0, 59}, {0, 59}, {0, 23}, {1, 31},
                                      {1, 12}, {0, 6}, {1970, 2099} };
    for (int i = 0; i < 7; ++i)
    {
        // 先过 preprocessCronField（`?` 归一 / 周·月英文名展开），与 cronMatches 严格同源
        const QString error = validateCronField(preprocessCronField(fields[i], i),
                                                limits[i][0], limits[i][1]);
        if (!error.isEmpty())
            return QStringLiteral("%1: %2").arg(QLatin1String(names[i]), error);
    }
    return QString();
}

bool CronSchedulerManager::cronFieldMatches(const QString &field, int value, int fieldMin)
{
    if (field == QLatin1String("*"))
        return true;

    // "*/N"：Vixie 语义 */N ≡ min-max/N，步进锚在**域下界**而非 0。锚错会让下界非 0 的
    // 字段整体偏移：日位 */10 应命中 1/11/21/31，按 value%N 却命中 10/20/30——「每 N 天」
    // 差一天。月位 */3、年位 */N 同病。下界 0 的字段（秒/分/时/周）两种锚法逐字等价。
    if (field.startsWith(QLatin1String("*/")))
    {
        const QString step = field.mid(2);
        if (!asciiDigits(step))
            return false;
        const int n = step.toInt();
        if (n <= 0)
            return false; // lcc 此处 ZeroDivisionError（崩该任务），lite 折叠不命中
        if (value < fieldMin)
            return false; // 锚差为负时 % 的符号随被除数，域外值一律不命中
        return (value - fieldMin) % n == 0;
    }

    // 逗号列表：任一命中即真，全不中为假（647b22e 关键修复，勿回退）
    if (field.contains(QLatin1Char(',')))
    {
        const QStringList parts = field.split(QLatin1Char(','));
        for (const QString &part : parts)
        {
            // fieldMin 必须透传：`1,*/5` 里的步长项同属该字段值域，丢了锚点又退回 0 起算
            if (cronFieldMatches(part.trimmed(), value, fieldMin))
                return true;
        }
        return false;
    }

    if (field.contains(QLatin1Char('-')))
    {
        const int idx = field.indexOf(QLatin1Char('-'));
        const QString start = field.left(idx);
        const QString end = field.mid(idx + 1);
        if (!asciiDigits(start) || !asciiDigits(end))
            return false; // lcc int() 抛错，lite 折叠
        return start.toInt() <= value && value <= end.toInt();
    }

    if (!asciiDigits(field))
        return false; // lcc int() 抛错，lite 折叠
    return field.toInt() == value;
}

bool CronSchedulerManager::cronMatches(const QString &expression, const QDateTime &moment)
{
    const QStringList raw =
        expression.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (raw.size() != 7)
        return false;

    // 与 validateCron 同源预处理：两处不同源就会出现「登记成功、永不触发」的静默失效，
    // 比直接报错坏得多（用户以为设上了，实际那条表达式从没命中过）
    QStringList fields;
    fields.reserve(7);
    for (int i = 0; i < 7; ++i)
        fields.append(preprocessCronField(raw[i], i));

    // 7 字段：second minute hour day-of-month month day-of-week year
    // 秒/分/时/月/年 AND 通道；日与周见下（Vixie 双限定 OR）
    // 周编号 = dayOfWeek()%7：Qt Mon=1..Sun=7 → cron Sun=0..Sat=6（0=周日）
    const int second = moment.time().second();
    const int minute = moment.time().minute();
    const int hour = moment.time().hour();
    const int dayOfMonth = moment.date().day();
    const int month = moment.date().month();
    const int dayOfWeek = moment.date().dayOfWeek() % 7;
    const int year = moment.date().year();

    // 每枚字段带上自己的值域下界，*/N 才有正确的锚（详见 cronFieldMatches 注释）
    if (!cronFieldMatches(fields[0], second, 0) || !cronFieldMatches(fields[1], minute, 0)
        || !cronFieldMatches(fields[2], hour, 0) || !cronFieldMatches(fields[4], month, 1)
        || !cronFieldMatches(fields[6], year, 1970))
        return false;

    const QString &day = fields[3];
    const QString &dow = fields[5];
    if (day == QLatin1String("*") && dow == QLatin1String("*"))
        return true;
    if (day == QLatin1String("*"))
        return cronFieldMatches(dow, dayOfWeek, 0);
    if (dow == QLatin1String("*"))
        return cronFieldMatches(day, dayOfMonth, 1);
    // 双限定：日与周取 OR（Vixie cron 经典语义，lcc 注释明示）
    return cronFieldMatches(day, dayOfMonth, 1) || cronFieldMatches(dow, dayOfWeek, 0);
}

QString CronSchedulerManager::newCronId(QString *error)
{
    // lcc _new_cron_id：secrets.token_hex(4) → 8 位小写十六进制；≤100 次去重尝试
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const quint32 raw = QRandomGenerator::global()->generate();
        const QString id =
            QStringLiteral("cron_") + QString::number(raw, 16).rightJustified(8, QLatin1Char('0'));
        if (!m_jobs.contains(id))
            return id;
    }
    if (error)
        *error = QStringLiteral("could not allocate a cron job id");
    return QString();
}

QString CronSchedulerManager::scheduleJob(const QString &cron, const QString &prompt,
                                          bool recurring, bool durable, CronJob *out)
{
    const QString invalid = validateCron(cron);
    if (!invalid.isEmpty())
        return invalid;
    if (prompt.trimmed().isEmpty())
        return QStringLiteral("prompt cannot be empty");

    QString allocError;
    const QString id = newCronId(&allocError);
    if (id.isEmpty())
        return allocError;

    CronJob job;
    job.id = id;
    job.cron = cron;
    job.prompt = prompt;
    job.recurring = recurring;
    job.durable = durable;
    m_jobs.insert(id, job); // lcc：先登记，durable 落盘失败再回滚摘除

    if (durable && !saveDurableJobs())
    {
        m_jobs.remove(id);
        return QStringLiteral("failed to save scheduled_tasks.json"); // lcc 抛错，折叠偏差
    }

    LOG_INFO << QStringLiteral("[cron] scheduled %1: %2 -> %3").arg(id, cron, prompt.left(60));
    if (out)
        *out = job;
    return QString();
}

QString CronSchedulerManager::cancelJob(const QString &id)
{
    if (!m_jobs.contains(id))
        return QStringLiteral("Job %1 not found").arg(id);

    const CronJob removed = m_jobs.take(id);
    const QList<CronJob> queueSnapshot = m_queue; // 回滚用（lcc 队列同 id 条目一并移除）
    QList<CronJob> kept;
    for (const CronJob &entry : m_queue)
    {
        if (entry.id != id)
            kept.append(entry);
    }
    m_queue = kept;

    if (removed.durable && !saveDurableJobs())
    {
        m_jobs.insert(id, removed);
        m_queue = queueSnapshot;
        return QStringLiteral("failed to save scheduled_tasks.json"); // lcc 抛错，折叠偏差
    }

    LOG_INFO << QStringLiteral("[cron] cancelled %1").arg(id);
    return QStringLiteral("Cancelled %1").arg(id);
}

bool CronSchedulerManager::enqueueDueJob(const QString &id, const QString &marker)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return false;

    CronJob job = it.value();
    const bool oldPending = job.pendingDelivery;
    const QString oldFired = job.lastFired;
    job.pendingDelivery = true;
    job.lastFired = marker;
    it.value() = job;

    // lcc _enqueue_due_job：durable 先落盘，失败回滚两字段并抛（lite 折叠 false，不入队）
    if (job.durable && !saveDurableJobs())
    {
        auto back = m_jobs.find(id);
        if (back != m_jobs.end())
        {
            CronJob restore = back.value();
            restore.pendingDelivery = oldPending;
            restore.lastFired = oldFired;
            back.value() = restore;
        }
        return false;
    }

    m_queue.append(job);
    return true;
}

void CronSchedulerManager::pollDueJobs(const QDateTime &moment)
{
    // 补匹配走 epoch 整秒轴，刻意不用带亚秒余数的墙钟时刻作基准：QTimer 1s 节拍的相邻
    // 两拍墙钟差可略小于 1s（上拍被事件循环延迟、本拍追平），若 from = 上拍时刻 + 1s，
    // 则 from > moment → 本轮零检查，而被跨过的那枚整秒因基准已推进到本拍永不回补——
    // 秒位限定表达式（如 "0 * * * * * *"）会整周期漏触发，实测稳态下漏 99.99% 的秒。
    // 整秒轴令每枚 epoch 秒至多查一次且不漏：from = 上次已查秒 + 1。
    const qint64 curSec = moment.toSecsSinceEpoch();
    qint64 fromSec = curSec; // 首拍/时钟回拨：只查当前秒
    if (m_lastPollSec >= 0 && m_lastPollSec < curSec)
    {
        fromSec = m_lastPollSec + 1;
        if (curSec - m_lastPollSec > 60) // 封顶 60s，防长阻塞/睡眠后成批爆发（既有取舍）
            fromSec = curSec - 60;
    }
    m_lastPollSec = curSec;

    const QList<CronJob> snapshot = m_jobs.values(); // 快照遍历：enqueue 改值不扰动迭代
    for (const CronJob &job : snapshot)
    {
        if (job.pendingDelivery) // 上轮未交付：整轮跳过，等终局 ack/restore 再触发
            continue;
        for (qint64 s = fromSec; s <= curSec; ++s)
        {
            const QDateTime t = QDateTime::fromSecsSinceEpoch(s);
            if (!cronMatches(job.cron, t))
                continue;
            const QString marker = t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            if (job.lastFired == marker) // 同一秒重复轮询只发一次
                continue;
            if (enqueueDueJob(job.id, marker))
                LOG_INFO << QStringLiteral("[cron] due %1: %2").arg(job.id, job.prompt.left(60));
            else
                qCritical().noquote() << QStringLiteral("[cron] could not enqueue %1: failed to save scheduled_tasks.json").arg(job.id);
            break; // 一轮最多入队一次：成功则 pendingDelivery 挡后续秒，失败也留待下轮
        }
    }
}

QList<CronSchedulerManager::CronJob> CronSchedulerManager::consumeQueue()
{
    QList<CronJob> fired = m_queue;
    m_queue.clear();
    return fired;
}

bool CronSchedulerManager::hasQueue() const
{
    return !m_queue.isEmpty();
}

void CronSchedulerManager::acknowledgeCronJobs(const QList<CronJob> &fired)
{
    struct Change
    {
        CronJob before; // 变更前的完整副本（回滚直接恢复）
        bool removed = false;
    };
    QList<Change> changes;
    bool anyDurable = false;

    for (const CronJob &delivered : fired)
    {
        auto it = m_jobs.find(delivered.id);
        if (it == m_jobs.end())
            continue; // 交付期间已被 cancelJob 摘除（lcc current is None → skip）

        CronJob current = it.value();
        Change change;
        change.before = current;
        if (current.recurring)
        {
            current.pendingDelivery = false; // 周期任务：清标记留表待下轮
            it.value() = current;
        }
        else
        {
            m_jobs.erase(it); // 一次性任务：送达即退役（Qt6 QHash 迭代器删除用 erase）
            change.removed = true;
        }
        if (current.durable)
            anyDurable = true;
        changes.append(change);
    }

    if (anyDurable && !saveDurableJobs())
    {
        // lcc save 失败抛异常、run_delivery 的 except 路径恢复现场——lite 折叠为原地回滚：
        // 恢复全部变更前状态，再把不在队列中的变更任务重新入队（去重）
        for (const Change &change : changes)
            m_jobs.insert(change.before.id, change.before);
        for (const Change &change : changes)
            enqueueUnique(change.before);
    }
}

void CronSchedulerManager::enqueueUnique(const CronJob &job)
{
    // 先到先得：同 id 已在队即跳过（重复入队会导致同任务双交付），否则追加队尾
    for (const CronJob &entry : m_queue)
    {
        if (entry.id == job.id)
            return;
    }
    m_queue.append(job);
}

void CronSchedulerManager::restoreCronJobs(const QList<CronJob> &fired)
{
    // lcc restore_cron_jobs：仅恢复 pendingDelivery 并按 id 去重回队，不清 lastFired、不落盘
    QSet<QString> queuedIds;
    for (const CronJob &entry : m_queue)
        queuedIds.insert(entry.id);

    for (const CronJob &delivered : fired)
    {
        auto it = m_jobs.find(delivered.id);
        if (it == m_jobs.end())
            continue;
        CronJob current = it.value();
        current.pendingDelivery = true;
        it.value() = current;
        if (!queuedIds.contains(current.id))
        {
            m_queue.append(current);
            queuedIds.insert(current.id);
        }
    }
}

// 交付编排（lcc 31a99d1 run_delivery 转译）：收割→回调→按回调结果收尾。
// 偏差登记：lcc 回调抛异常时 restore 后 re-raise；lite 无异常链，回调以 bool 返回
// 成败（宿主拒收=false），false 分支等价 restore，true 转入在途待终局确认。
bool CronSchedulerManager::runDelivery(const std::function<bool(const QList<CronJob> &)> &deliver)
{
    const QList<CronJob> fired = consumeQueue();
    if (fired.isEmpty())
        return false; // 空批不动台账（lcc run_delivery 早退）

    if (!deliver(fired))
    {
        restoreCronJobs(fired); // 拒收回队，下个空闲 tick 重试（at-least-once）
        return false;
    }
    m_inFlight = fired; // ack 推迟到回合终局 finalizeInFlightDelivery
    return true;
}

// 回合终局收口（lcc run_delivery 成功路径的 acknowledge 时机）：幂等，无在途 no-op
void CronSchedulerManager::finalizeInFlightDelivery(bool success)
{
    if (m_inFlight.isEmpty())
        return;
    const QList<CronJob> fired = m_inFlight;
    m_inFlight.clear();
    if (success)
        acknowledgeCronJobs(fired);
    else
        restoreCronJobs(fired); // 回合失败（停止/流错误/轮次上限）→ 回队重投
}

QString CronSchedulerManager::listCrons() const
{
    if (m_jobs.isEmpty())
        return QStringLiteral("No cron jobs.");

    QStringList lines;
    for (auto it = m_jobs.constBegin(); it != m_jobs.constEnd(); ++it)
    {
        const CronJob &job = it.value();
        lines << QStringLiteral("%1: %2 -> %3 [%4, %5]")
                     .arg(job.id, job.cron, job.prompt.left(60),
                          job.recurring ? QStringLiteral("recurring") : QStringLiteral("one-shot"),
                          job.durable ? QStringLiteral("durable") : QStringLiteral("session"));
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonArray CronSchedulerManager::ledgerSnapshot() const
{
    QStringList ids = m_jobs.keys();
    ids.sort();
    QJsonArray rows;
    for (const QString &id : ids) {
        // constFind 而非 value()：QHash::value(key) **按值返回**，绑到 const& 只是延长临时
        // 对象寿命，整份 CronJob（多个 QString + QDate）仍被深拷一遍。ids 刚由 keys() 取得，
        // 键必在。本函数挂在每会话每秒一次的 cronTick 上（AgentLoop.cpp），N 作业即 N 次/秒。
        const auto jobIt = m_jobs.constFind(id);
        if (jobIt == m_jobs.constEnd())
            continue; // 防御性早退，理论不可达；避免任何重入下解引用 constEnd 的悬垂迭代器
        const CronJob &job = *jobIt;
        QJsonObject row;
        row[QStringLiteral("id")] = job.id;
        row[QStringLiteral("cron")] = job.cron;
        row[QStringLiteral("prompt")] = job.prompt;
        row[QStringLiteral("recurring")] = job.recurring;
        row[QStringLiteral("durable")] = job.durable;
        row[QStringLiteral("pendingDelivery")] = job.pendingDelivery;
        row[QStringLiteral("lastFired")] = job.lastFired;
        rows.append(row);
    }
    return rows;
}

QString CronSchedulerManager::durableFilePath() const
{
    // sink 为宿主会话数据根（含 .lite-harness 中间层/会话段），本处仅拼文件名
    return m_workDirSink() + QStringLiteral("/scheduled_tasks.json");
}

bool CronSchedulerManager::saveDurableJobs()
{
    QJsonArray jobs;
    for (auto it = m_jobs.constBegin(); it != m_jobs.constEnd(); ++it)
    {
        const CronJob &job = it.value();
        if (!job.durable)
            continue;
        QJsonObject entry;
        entry[QStringLiteral("id")] = job.id;
        entry[QStringLiteral("cron")] = job.cron;
        entry[QStringLiteral("prompt")] = job.prompt;
        entry[QStringLiteral("recurring")] = job.recurring;
        entry[QStringLiteral("durable")] = job.durable;
        entry[QStringLiteral("pending_delivery")] = job.pendingDelivery;
        entry[QStringLiteral("last_fired")] =
            job.lastFired.isEmpty() ? QJsonValue::Null : QJsonValue(job.lastFired);
        jobs.append(entry);
    }

    const QString path = durableFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath()); // 惰性建 .lite-harness 中间目录
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(jobs).toJson(QJsonDocument::Indented));
    return file.commit();
}

void CronSchedulerManager::loadDurableJobs()
{
    const QString path = durableFilePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return; // 缺文件静默（lcc: if not path.exists(): return）

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray())
    {
        qCritical().noquote() << QStringLiteral("[cron] could not load scheduled_tasks.json: %1").arg(
              parseError.error == QJsonParseError::NoError
                  ? QStringLiteral("file is not a JSON array")
                  : parseError.errorString());
        return;
    }

    int loaded = 0;
    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array)
    {
        CronJob job;
        QString error;
        if (!parseSavedJob(value, &job, &error))
        {
            LOG_WARN << QStringLiteral("[cron] skipped invalid saved job: %1").arg(error);
            continue;
        }

        // setWorkDir 的 stop→start 重载在 lcc 无对应物（python 每进程单 Env）：
        // 同 id 已登记则整行跳过，保证同目录重启与换目录合并均幂等不双计（登记偏差）
        if (m_jobs.contains(job.id))
            continue;

        m_jobs.insert(job.id, job);
        // lcc 原样：无剪枝——pending 的一次性任务照常入队重投（at-least-once）；
        // 规格书"装载即剪枝"与参照文件相悖，按"参照文件为准"裁决不剪枝（登记偏差）
        if (job.pendingDelivery)
            enqueueUnique(job);
        ++loaded;
    }

    if (loaded > 0)
        LOG_INFO << QStringLiteral("[cron] loaded %1 durable job(s)").arg(loaded);
}
