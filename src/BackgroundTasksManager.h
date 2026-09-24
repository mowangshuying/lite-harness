#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * BackgroundTasksManager —— 后台 bash 任务台账（对齐 lcc s11 background_tasks_manager.py）
 *
 * 纯数据管理器：不建线程、不碰进程——本仓全事件驱动、零线程先例。进程由宿主
 * AgentLoop 的 QProcess 异步链实际驱动，结束回调经 recordResult() 落账；本类只
 * 负责登记任务、存结果、收割时渲染 <task_notification> 文本。
 *
 * 与 lcc 的有意偏差（移植转译，登记在案）：
 *   - threading.Lock / daemon Thread → 仅主线程访问、无锁（同 AgentLoop m_messages 纪律）；
 *   - start() 同步回滚线程启动失败 → QProcess 启动失败（FailedToStart）只能经
 *     errorOccurred 异步得知，start 本身恒成功登记；
 *   - shell_processes 集合与进程组清理 → 由宿主 m_activeProcesses 统一登记、
 *     stop()/析构 kill；
 *   - lcc 在 collect() 打印 "collected"，lite 按移植规格在 recordResult() 记账时打印。
 */
class BackgroundTasksManager
{
public:
    // 一条任务记录（lcc tasks[task_id] dict；tool_use_id 仅登记用，通知文本不含它，与 lcc 一致）
    struct Task
    {
        QString toolUseId;
        QString command;
        QString status; // running | completed | failed
    };

    // 登记一条后台任务并返回任务 id（bg_0001 起自增；lcc start）。
    // command 为空白 → 返回空串并经 *error 给出 "Bash command cannot be empty"（lcc 文案逐字）
    QString start(const QString &command, const QString &toolUseId, QString *error = nullptr);

    // 是否应转后台（lcc should_run_background）：工具为 bash 且 run_in_background 是严格
    // JSON 布尔 true——字符串 "true"/数字 1 不算（对齐 python "is True"），静默走前台
    static bool shouldRunBackground(const QString &toolName, const QJsonObject &args);

    // 进程结束记账（lcc run() 尾部）：退出码 0 且未超时 → completed，否则 failed；
    // results 存 formatBashResult 产物，入收割队列并打印记账行（lcc "collected" 文案）
    void recordResult(const QString &taskId, const QString &output, int exitCode, bool timedOut);

    // 收割（lcc collect）：快照并清空 ready 队列，逐条 pop task/result，
    // 渲染 <task_notification> 列表（result 取前 500 字符、无截断标记——lcc 文案逐字）
    QStringList collect();

    // lcc format_bash_result 等价：超时（对应 lcc exit_code None）或退出码 0 → 原样返回；
    // 否则前缀 "Error: command exited with status %1:\n%2"。主循环前台路径共用
    static QString formatBashResult(const QString &output, int exitCode, bool timedOut);

private:
    QHash<QString, Task> m_tasks;
    QHash<QString, QString> m_results;
    QStringList m_ready;
    int m_counter = 0;
};
