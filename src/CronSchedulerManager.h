#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

/**
 * CronSchedulerManager —— 持久化定时任务台账（对齐 lcc s12 cron_scheduler.py）
 *
 * 纯数据管理器：不建线程——本仓全事件驱动、零线程先例。1s 轮询节拍由宿主
 * AgentLoop 的 QTimer 驱动（tick 内调 pollDueJobs + 交付），本类负责 cron 表达式
 * 校验/匹配、任务台账、到期队列收割与 scheduled_tasks.json 持久化。
 * 存储：<workDir>/.lite-harness/scheduled_tasks.json（仿 s07 技能/s10 任务的
 * .lite-harness 中间目录约定，保存时惰性 mkpath）。
 *
 * 与 lcc 的有意偏差（移植转译，登记在案）：
 *   - threading.Thread 1s 轮询 → 宿主 QTimer（AgentLoop m_cronTick）；
 *   - threading.RLock 三把锁 → 仅主线程访问、无锁（同 m_messages 纪律）；
 *   - secrets.token_hex(4) → QRandomGenerator 8 位小写十六进制（格式等价）；
 *   - lcc 交付失败靠调用方 try/except 后 restore 重投（at-least-once）→
 *     lite 以 runDelivery/finalizeInFlightDelivery 两段式对齐同一语义（lcc 31a99d1），
 *     仅存形态差：python 同步回调包整回合、异常 re-raise → lite 回调 bool 返回成败、
 *     ack 推迟至宿主回合终局（成功/停止/流错误/轮次上限）收口；
 *   - lcc log.py 彩色 ANSI 与 blank_before 排版 → lite qDebug/qInfo 系列忽略（观感偏差）；
 *   - 多个会话页同 workDir 各持一套台账时可能双触发——用户已接受的已知偏差；
 *   - 装载无剪枝：pending 的一次性任务重启后照常入队重投（逐字对齐 lcc
 *     load_durable_jobs 的 at-least-once 语义；移植规格书"装载即剪枝"一句与
 *     参照文件相悖，按规格书自订的"参照文件为准"裁决不剪枝——登记偏差）；
 *   - save 失败 lcc 裸抛异常 → lite 折叠为错误文本/回滚布尔（executeTool
 *     "一切失败皆字符串"纪律，同 s10 任务图先例）；
 *   - python isdigit 覆盖 Unicode 数字 → lite 仅认 ASCII 数字（观感差异可忽略）；
 *   - QJsonObject 键按字典序落盘（python asdict 保持声明序）——纯观感；
 *   - setWorkDir 触发的 stop→start 重载在 lcc 无对应物（python 每进程单 Env），
 *     lite 使 load 幂等：同 id 已登记则整行跳过、入队先查重。
 */
class CronSchedulerManager
{
public:
    // 一条定时任务（lcc CronJob dataclass；lastFired 空串 ≡ python None，JSON 落 null）
    struct CronJob
    {
        QString id;               // cron_ + 8 位小写十六进制
        QString cron;             // 5 字段 cron 表达式
        QString prompt;           // 到点注入的提示词
        bool recurring = true;    // true=周期；false=一次性（送达后从台账移除）
        bool durable = true;      // true=落盘持久（跨重启）；false=仅本进程
        bool pendingDelivery = false; // 已入队待交付（at-least-once 标记）
        QString lastFired;        // 最近触发分钟 "yyyy-MM-dd HH:mm"（空=从未）
    };

    // workDirSink 惰性取宿主当前工作目录（仿 CompactManager/MemoryManager/BackgroundTasks 注入法）
    explicit CronSchedulerManager(std::function<QString()> workDirSink);

    // 运行时开关（lcc start_runtime_threads/stop_runtime）：start 幂等——已启动直接返回，
    // 否则先装载 durable 任务再置位；stop 仅复位标志（台账与队列原样留存）
    void start();
    void stop();
    bool isRuntimeStarted() const;

    // 5 字段 cron 校验（lcc validate_cron 逐字）：合法返回空串，否则 "<字段名>: <错误>"。
    // 字段名 minute / hour / day-of-month / month / day-of-week；越界 [0-59]/[0-23]/[1-31]/[1-12]/[0-6]
    static QString validateCron(const QString &expression);

    // 单字段匹配（lcc _cron_field_matches）：* / */N / a,b,c / a-b / 精确值。
    // 偏差：lcc 对坏输入（*/0、非数字）抛异常由 poll 逐任务捕获，lite 静默 false；
    // 逗号列表全不中返回 false（647b22e 关键修复，勿回退为"默认命中"）
    static bool cronFieldMatches(const QString &field, int value);

    // 整表达式与时刻匹配（lcc _cron_matches）：分/时/月 AND；日与周——
    // 双 * 恒真；单 * 只看限定侧；双限定取 OR（Vixie cron 语义）。
    // 周编号 = dayOfWeek()%7：Qt Mon=1..Sun=7 → cron Sun=0..Sat=6（与 lcc 的 (weekday()+1)%7 等价）
    static bool cronMatches(const QString &expression, const QDateTime &moment);

    // 登记任务（lcc schedule_job）：校验失败/空 prompt/存储失败 → 返回错误文本（存储失败已回滚登记）；
    // 成功 → 空串并经 *out 回填登记副本（id 供调用方展示）。durable 落盘失败折叠为文本（登记偏差）
    QString scheduleJob(const QString &cron, const QString &prompt, bool recurring, bool durable,
                        CronJob *out = nullptr);

    // 取消（lcc cancel_job）：不存在返回 "Job <id> not found"；成功返回 "Cancelled <id>"
    // 并同步清到期队列快照；落盘失败回滚台账与队列后返回错误文本（lcc 抛错，折叠偏差）
    QString cancelJob(const QString &id);

    // 轮询（lcc poll_due_jobs，宿主 tick 调用）：分钟标记 "yyyy-MM-dd HH:mm"；
    // 跳过 pendingDelivery 或 lastFired==marker（同分钟双拍只发一次）；
    // 命中 → 入队并落 lastFired/pendingDelivery（durable 先落盘，失败回滚该两字段不入队）
    void pollDueJobs(const QDateTime &moment);

    // 收割到期队列（lcc consume_cron_queue）：take-all 快照并清空
    QList<CronJob> consumeQueue();
    // 队列是否非空（lcc has_cron_queue）
    bool hasQueue() const;

    // 交付确认（lcc acknowledge_cron_jobs）：周期任务清 pendingDelivery 留表待下轮；
    // 一次性任务从台账移除。有 durable 变更则落盘，失败整体回滚并重新入队（lcc 抛错，折叠偏差）
    void acknowledgeCronJobs(const QList<CronJob> &fired);

    // 交付失败回滚（lcc restore_cron_jobs）：恢复 pendingDelivery 并重新入队（去重），
    // **不清** lastFired——at-least-once：宿主拒收时下个 tick 直接重试，不等下一整分钟；不落盘
    void restoreCronJobs(const QList<CronJob> &fired);

    // 交付编排（lcc 31a99d1 run_delivery 转译）：收割→回调→按回调结果收尾——
    // 空批返回 false 不动台账；回调 false（宿主拒收）→ restoreCronJobs 回队待重试、返回 false；
    // 回调 true → 本批转入在途（m_inFlight），返回 true——ack 推迟到回合终局 finalizeInFlightDelivery
    bool runDelivery(const std::function<bool(const QList<CronJob> &)> &deliver);
    // 回合终局收口（lcc run_delivery 的 finally 语义）：在途批 success→acknowledge，否则 restore；
    // 无在途 no-op（幂等——多终局点重复调用安全）
    void finalizeInFlightDelivery(bool success);

    // 台账清单文本（lcc run_list_crons → list_cron_jobs 仅登记表现状快照，队列⊆台账恒成立）：
    // 空 → "No cron jobs."；否则每行 "%1: %2 -> %3 [%4, %5]"
    // （id, cron, prompt 前 60 字符, recurring/one-shot, durable/session），\n 连接
    QString listCrons() const;

private:
    // 落盘文件路径：m_workDirSink() + "/.lite-harness/scheduled_tasks.json"
    QString durableFilePath() const;
    // 全量重写 durable 任务（lcc _save_durable_jobs，QSaveFile 原子写，键名逐字：
    // id/cron/prompt/recurring/durable/pending_delivery/last_fired）；失败返回 false 供调用方回滚
    bool saveDurableJobs();
    // 从落盘文件装载（lcc load_durable_jobs）：缺文件静默；坏行跳过并 qInfo（文案逐字）；
    // pendingDelivery 的照常入队（lcc 无剪枝——见头部偏差登记）
    void loadDurableJobs();
    // 到期入队（lcc _enqueue_due_job）：置 pendingDelivery/lastFired，durable 落盘失败回滚两字段
    // 返回 false（不入队）；成功入队返回 true
    bool enqueueDueJob(const QString &id, const QString &marker);
    // 分配任务 id（lcc _new_cron_id）：≤100 次去重尝试；失败返回空串并经 *error 给
    // "could not allocate a cron job id"
    QString newCronId(QString *error);

    std::function<QString()> m_workDirSink;
    QHash<QString, CronJob> m_jobs; // 台账（lcc scheduled_jobs；lite 无插入序——遍历序偏差，仅观感）
    QList<CronJob> m_queue;         // 到期待交付队列（lcc cron_queue）
    QList<CronJob> m_inFlight;      // 已交付待确认（lcc run_delivery 局部 fired 的成员化，两段式）
    bool m_runtimeStarted = false;  // lcc _runtime_started
};
