// CronSchedulerManager —— lcc s12 cron_scheduler.py 的 lite 转译（偏差登记见头文件注释块）
#include "CronSchedulerManager.h"

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
    if (fields.size() != 5)
        return QStringLiteral("Expected 5 fields, got %1").arg(fields.size());

    // 字段名与取值域（lcc field_rules 逐字：顺序即报错前缀）
    static const char *names[5] = { "minute", "hour", "day-of-month", "month", "day-of-week" };
    static const int limits[5][2] = { {0, 59}, {0, 23}, {1, 31}, {1, 12}, {0, 6} };
    for (int i = 0; i < 5; ++i)
    {
        const QString error = validateCronField(fields[i], limits[i][0], limits[i][1]);
        if (!error.isEmpty())
            return QStringLiteral("%1: %2").arg(QLatin1String(names[i]), error);
    }
    return QString();
}

bool CronSchedulerManager::cronFieldMatches(const QString &field, int value)
{
    if (field == QLatin1String("*"))
        return true;

    // "*/N"：value % N == 0。偏差登记：lcc 坏 N 抛异常由 poll 捕获，lite 静默 false
    if (field.startsWith(QLatin1String("*/")))
    {
        const QString step = field.mid(2);
        if (!asciiDigits(step))
            return false;
        const int n = step.toInt();
        if (n <= 0)
            return false; // lcc 此处 ZeroDivisionError（崩该任务），lite 折叠不命中
        return value % n == 0;
    }

    // 逗号列表：任一命中即真，全不中为假（647b22e 关键修复，勿回退）
    if (field.contains(QLatin1Char(',')))
    {
        const QStringList parts = field.split(QLatin1Char(','));
        for (const QString &part : parts)
        {
            if (cronFieldMatches(part.trimmed(), value))
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
    const QStringList fields =
        expression.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() != 5)
        return false;

    // lcc cron_weekday = datetime.weekday() 周一0 → (weekday()+1)%7：周一1…周六6、周日0；
    // Qt dayOfWeek() 周一1…周日7 → %7 恰得 周一1…周六6、周日0——两映射等价
    const int minute = moment.time().minute();
    const int hour = moment.time().hour();
    const int dayOfMonth = moment.date().day();
    const int month = moment.date().month();
    const int dayOfWeek = moment.date().dayOfWeek() % 7;

    if (!cronFieldMatches(fields[0], minute) || !cronFieldMatches(fields[1], hour)
        || !cronFieldMatches(fields[3], month))
        return false;

    const QString &day = fields[2];
    const QString &dow = fields[4];
    if (day == QLatin1String("*") && dow == QLatin1String("*"))
        return true;
    if (day == QLatin1String("*"))
        return cronFieldMatches(dow, dayOfWeek);
    if (dow == QLatin1String("*"))
        return cronFieldMatches(day, dayOfMonth);
    // 双限定：日与周取 OR（Vixie cron 经典语义，lcc 注释明示）
    return cronFieldMatches(day, dayOfMonth) || cronFieldMatches(dow, dayOfWeek);
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

    qInfo().noquote() << QStringLiteral("[cron] scheduled %1: %2 -> %3").arg(id, cron, prompt.left(60));
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

    qInfo().noquote() << QStringLiteral("[cron] cancelled %1").arg(id);
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
    // lcc poll_due_jobs：marker 到分钟粒度；双闸（pendingDelivery 未清 或 本分钟已发）防重
    const QString marker = moment.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QList<CronJob> snapshot = m_jobs.values(); // 快照遍历：enqueue 改值不扰动迭代
    for (const CronJob &job : snapshot)
    {
        if (job.pendingDelivery || job.lastFired == marker)
            continue;
        if (!cronMatches(job.cron, moment))
            continue;

        if (enqueueDueJob(job.id, marker))
            qInfo().noquote() << QStringLiteral("[cron] due %1: %2").arg(job.id, job.prompt.left(60));
        else
            qCritical().noquote() << QStringLiteral("[cron] could not enqueue %1: failed to save scheduled_tasks.json").arg(job.id);
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
        {
            bool queued = false;
            for (const CronJob &entry : m_queue)
            {
                if (entry.id == change.before.id)
                {
                    queued = true;
                    break;
                }
            }
            if (!queued)
                m_queue.append(change.before);
        }
    }
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
            qWarning().noquote() << QStringLiteral("[cron] skipped invalid saved job: %1").arg(error);
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
        {
            bool queued = false;
            for (const CronJob &entry : m_queue)
            {
                if (entry.id == job.id)
                {
                    queued = true;
                    break;
                }
            }
            if (!queued)
                m_queue.append(job);
        }
        ++loaded;
    }

    if (loaded > 0)
        qInfo().noquote() << QStringLiteral("[cron] loaded %1 durable job(s)").arg(loaded);
}
