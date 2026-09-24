#include "BackgroundTasksManager.h"

#include <QDebug>

QString BackgroundTasksManager::start(const QString &command, const QString &toolUseId,
                                      QString *error)
{
    // lcc start 的命令校验：必须为非空白字符串（ValueError 文案逐字移植）
    if (command.trimmed().isEmpty())
    {
        if (error)
            *error = QStringLiteral("Bash command cannot be empty");
        return QString();
    }

    ++m_counter;
    const QString taskId = QStringLiteral("bg_%1").arg(m_counter, 4, 10, QLatin1Char('0'));
    m_tasks.insert(taskId, Task{ toolUseId, command, QStringLiteral("running") });
    return taskId;
}

bool BackgroundTasksManager::shouldRunBackground(const QString &toolName, const QJsonObject &args)
{
    const QJsonValue flag = args.value(QStringLiteral("run_in_background"));
    return toolName == QStringLiteral("bash")
        && flag.type() == QJsonValue::Bool && flag.toBool();
}

void BackgroundTasksManager::recordResult(const QString &taskId, const QString &output,
                                          int exitCode, bool timedOut)
{
    const auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return; // lcc run() 的 get(task_id) is None 早退（任务已被收割）

    it->status = (exitCode == 0 && !timedOut) ? QStringLiteral("completed")
                                              : QStringLiteral("failed");
    m_results.insert(taskId, formatBashResult(output, exitCode, timedOut));
    m_ready.append(taskId);
    // lcc 在 collect() 打印本行；lite 按移植规格在记账时打印（有意偏差，见头文件注释）
    qDebug("[background] collected %1: %2", taskId, it->status);
}

QStringList BackgroundTasksManager::collect()
{
    QStringList notifications;
    const QStringList readyIds = m_ready;
    m_ready.clear();

    for (const QString &taskId : readyIds)
    {
        const QString result = m_results.take(taskId);
        const auto it = m_tasks.find(taskId);
        if (it == m_tasks.end())
            continue;
        const Task task = it.value();
        m_tasks.erase(it);

        // lcc collect 的通知模板逐字移植；result 截前 500 字符、无截断标记。
        // 单次 arg(a,b,c,d) 替换语义：值中的 '%' 不会被二次展开（同 makeSystemPrompt 纪律）
        notifications.append(QStringLiteral(
            "<task_notification>\n"
            "  <task_id>%1</task_id>\n"
            "  <status>%2</status>\n"
            "  <command>%3</command>\n"
            "  <result>%4</result>\n"
            "</task_notification>")
            .arg(taskId, task.status, task.command, result.left(500)));
    }
    return notifications;
}

QString BackgroundTasksManager::formatBashResult(const QString &output, int exitCode,
                                                 bool timedOut)
{
    // lcc: exit_code in (0, None) → 原样返回；None 态（超时/启动失败）在 lite 由 timedOut
    // 标志与调用点折叠表达（启动失败走 recordResult 的显式 Error 文本 + timedOut=false）
    if (timedOut || exitCode == 0)
        return output;
    return QStringLiteral("Error: command exited with status %1:\n%2").arg(exitCode).arg(output);
}
