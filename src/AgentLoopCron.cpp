// 定时任务（lcc s12）：schedule_cron / cancel_cron / list_crons 三个 handler 与空闲边界交付。
// 台账读写与 cron 语法匹配在 CronSchedulerManager，本文件只做宿主侧接线。

#include "AgentLoop.h"


QString AgentLoop::runScheduleCron(const QJsonObject &args)
{
    // lcc run_schedule_cron：错误串前缀 Error:；成功文本为完整 prompt（非 [:60]，
    // 截断只出现在台账日志与清单行）；recurring/durable 缺省 true（lcc 函数签名默认参数）
    const QString cron = args.value(QStringLiteral("cron")).toString();
    const QString prompt = args.value(QStringLiteral("prompt")).toString();
    const bool recurring = args.value(QStringLiteral("recurring")).toBool(true);
    const bool durable = args.value(QStringLiteral("durable")).toBool(true);

    CronSchedulerManager::CronJob job;
    const QString error = m_cron.scheduleJob(cron, prompt, recurring, durable, &job);
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    return QStringLiteral("Scheduled %1: %2 -> %3").arg(job.id, cron, prompt);
}

QString AgentLoop::runCancelCron(const QJsonObject &args)
{
    // lcc run_cancel_cron 直通 cancel_job（"Cancelled x" / "Job x not found" 原样返回）
    return m_cron.cancelJob(args.value(QStringLiteral("job_id")).toString());
}

QString AgentLoop::runListCrons()
{
    return m_cron.listCrons();
}

void AgentLoop::tryDeliverCron()
{
    // 空闲边界交付（lcc loop.py wait_for_cli_event 仅在等待输入时消费 cron_queue——
    // 运行中永不注入，到期任务在队列里等；无 busy 注入是 s12 定案行为）
    if (m_running)
        return;

    // at-least-once 两段式（lcc 31a99d1 run_delivery 转译）：空批不动台账；回调返回
    // false（宿主拒收）→ runDelivery 内部 restore 回队待下个空闲 tick 重试；true →
    // 本批转入在途，ack 推迟至回合终局 finalizeInFlightDelivery 收口
    m_cron.runDelivery([this](const QList<CronSchedulerManager::CronJob> &fired) {
        // 双形态文本（lcc deliver：history 逐任务 append "[Scheduled] {prompt}"，
        // _run_turn 用原文 "\n" join——lite 合并为单条消息，lcc N 条 → lite 1 条，登记偏差）
        QStringList displayParts;
        QStringList requestParts;
        for (const CronSchedulerManager::CronJob &job : fired)
        {
            displayParts << QStringLiteral("[Scheduled] %1").arg(job.prompt);
            requestParts << job.prompt;
        }

        // 直连同栈：emit 返回时宿主 run() 已置位 m_running（或已走 error 链拒绝）
        emit scheduledUserMessage(displayParts.join(QLatin1Char('\n')),
                                  requestParts.join(QLatin1Char('\n')));

        // lcc deliver 在回合执行前打印 delivered——时序保持一致
        for (const CronSchedulerManager::CronJob &job : fired)
            qInfo().noquote() << QStringLiteral("[cron] delivered %1: %2").arg(job.id, job.prompt.left(60));
        return m_running; // 宿主是否接管 = 本批交付成功与否
    });
}
