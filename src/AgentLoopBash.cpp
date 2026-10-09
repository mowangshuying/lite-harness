// bash 工具链：异步执行（QProcess 信号驱动，零线程）与后台任务结果收割注入。
// 危险命令判定与超时/截断文案单源见 BashRunner + AgentLoopPermission.cpp。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "ToolNames.h"
#include "BashRunner.h"

#include <QProcess>
#include <QDebug>

void AgentLoop::executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args,
                                 bool background, const QString &taskId)
{
    const QString command = args.value(QStringLiteral("command")).toString();

    // 安全检查：危险命令黑名单（同步短路，不启动进程）。
    // s01 内部黑名单与 s03 deny 列表双层防御保留；该输出按 handler 产出对待
    // （对齐 lcc run_bash 的返回文案），同样触发 PostToolUse。
    // 后台模式跳过（lcc 黑名单在前台 run_bash 内、后台分支绕过；且配对已由占位闭合，
    // 此处绝不可再走 onToolFinished）。判定与文案单源于 BashRunner（与子代理共用）
    if (!background)
    {
        const QString danger = BashRunner::dangerWarning(command, AgentLoopDetail::bashDenyList()); // lcc fddb23e 单源列表（G4）
        if (!danger.isEmpty())
        {
            triggerPostToolUseHooks(toolCall, danger);
            onToolFinished(toolCall, ToolNames::BASH, command, danger);
            return;
        }
    }

    // 120 秒超时：kill 后 finished 信号触发，靠标志区分"超时被杀" vs "正常结束"。
    // shared_ptr 捕获（MINOR-2 修复）：若进程从未启动/不发 finished，超时闭包与
    // 标志随最后一个捕获者释放，不再裸 new/delete 泄漏
    auto timedOut = std::make_shared<bool>(false);

    // 建进程/登记/挂超时/PowerShell 启动整段与子代理共用 BashRunner::start（原三处逐字复制）；
    // 差异（输出汇、钩子时序、取消语义）全部留在下方回调里。"先 connect 后 start"的
    // 原时序由 BashRunner::start 内部的 arm 钩子（start 前调用）保证
    if (background)
    {
        // 后台收口（lcc run() 的转译）：不 onToolFinished、不触发钩子，仅记账。
        // Qt 在 FailedToStart 的 errorOccurred 之后仍会发 finished，recorded 门闩保证恰好记一次
        auto recorded = std::make_shared<bool>(false);

        // Gate③ MINOR-3：派发时刻解析租约 cwd（lcc background_tasks_manager.py
        // :140-158——start(block, cwd=...) 派发时经 current_cwd 解析、记入 task、
        // 透传 Popen cwd=cwd or workDirPath）。leadToolCwd() 在本同步调用点求值
        // =派发时刻语义；进程生命周期内 cwd 不再漂移（与 lcc 一致）
        BashRunner::start(command, leadToolCwd(), this, &m_activeProcesses, timedOut,
                          [this, taskId, timedOut, recorded](QProcess *process) {
            connect(process, &QProcess::errorOccurred, this,
                    [this, process, taskId, recorded](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *recorded)
                    return;
                *recorded = true;
                // lcc run() 的 except 分支文案形态：Error: {异常}: {消息} → Qt 无异常，取 errorString
                m_backgroundTasks.recordResult(
                    taskId, QStringLiteral("Error: %1").arg(process->errorString()), -1, false);
            });

            connect(process, &QProcess::finished, this,
                    [this, process, taskId, timedOut, recorded](int exitCode, QProcess::ExitStatus) {
                m_activeProcesses.removeAll(process);
                if (!*recorded)
                {
                    *recorded = true;
                    m_backgroundTasks.recordResult(
                        taskId, BashRunner::finalizeOutput(process, *timedOut), exitCode, *timedOut);
                }
                process->deleteLater();
            });
        });

        // lcc 在 Popen 成功后打印；QProcess 启动是异步的，无法同步检测启动失败——
        // 登记后乐观打印，失败随后经 errorOccurred 补记（已知偏差）
        qDebug().noquote() << QStringLiteral("[bg] started %1 %2").arg(taskId, command.left(60));
        return;
    }

    // 防双收口门闩（照后台分支 recorded 写法）：Qt 文档口径 FailedToStart 的
    // errorOccurred 之后仍会发 finished，handled 保证 onToolFinished 恰好走一次
    auto handled = std::make_shared<bool>(false);

    // s13 租约感知 cwd（挂载十四）：前台 bash 的执行目录走 leadToolCwd()——Lead 持租约
    // 且任务绑定 worktree 时进 worktree（lcc _run_base current_cwd 对 lead bash 同生效）；
    // 无租约回落 m_workDir，与 s13 前逐字节一致。Gate③ MINOR-3 落地：后台分支同切
    // leadToolCwd()（派发时刻求值，lcc 同型，见上方注释）——原「保守不切」偏差关闭。
    BashRunner::start(command, leadToolCwd(), this, &m_activeProcesses, timedOut,
                      [this, toolCall, command, timedOut, handled](QProcess *process) {
        // 启动失败显式收口（挂起审计防御加固，对齐后台分支的 errorOccurred 防线）：
        // 原前台只连 finished、依赖"FailedToStart 后仍发 finished"的隐式行为收口，
        // 且届时会把空输出+退出码 0 当正常结果交还模型；此处直接以明确错误文本终结
        // 工具调用，保证工具结果必达、m_running 回合不悬停。进程清理仍留给 finished
        // 分支（同款时序：errorOccurred 先于 finished，removeAll/deleteLater 单点执行）
        connect(process, &QProcess::errorOccurred, this,
                [this, process, toolCall, command, handled](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || *handled)
                return;
            *handled = true;
            const QString output = QStringLiteral("Error: bash 启动失败：powershell.exe 无法启动（%1）")
                                       .arg(process->errorString());
            // PostToolUse 钩子与正常分支同时序（handler 产出后、回填前）
            triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, ToolNames::BASH, command, output);
        });

        connect(process, &QProcess::finished, this,
                [this, process, toolCall, command, timedOut, handled](int exitCode, QProcess::ExitStatus) {
            m_activeProcesses.removeAll(process);

            // FailedToStart 已由 errorOccurred 显式收口：不再二次 onToolFinished，只销毁进程
            if (*handled)
            {
                process->deleteLater();
                return;
            }
            *handled = true;

            // finalizeOutput：超时→Timeout 文案（不读缓冲）；否则截断+空兜底（与后台/子代理同口径）
            const QString base = BashRunner::finalizeOutput(process, *timedOut);
            // 前台对齐 lcc s11 run_bash 重构（共用 run_bash_process + format_bash_result）：
            // 非零退出码前缀 "Error: command exited with status N:"；超时仍用现有 Timeout 文案
            const QString output =
                *timedOut ? base
                          : BackgroundTasksManager::formatBashResult(base, exitCode, false);
            process->deleteLater();

            // PostToolUse 钩子（lcc s04）：bash handler 产出后、回填前触发
            triggerPostToolUseHooks(toolCall, output);

            onToolFinished(toolCall, ToolNames::BASH, command, output);
        });
    });
}

// 收割后台任务通知并注入会话（lcc loop.py inject_background_results 的 OpenAI 形态转译：
// lcc content block 列表 → lite 扁平字符串拼接，同 s05 todo 提醒形态；一次性消费，空则不动作）
void AgentLoop::injectBackgroundResults()
{
    const QStringList notifications = m_backgroundTasks.collect();
    if (notifications.isEmpty())
        return;

    const QString joined = notifications.join(QLatin1Char('\n'));

    if (!m_messages.isEmpty()
        && m_messages.back().value(QStringLiteral("role")).toString() == QStringLiteral("user"))
    {
        // lcc：末条是 user 消息则把通知并入其 content 尾部（块列表 extend → 字符串以空行分隔拼接）
        QJsonObject &tail = m_messages.back();
        tail[QStringLiteral("content")] =
            tail.value(QStringLiteral("content")).toString() + QStringLiteral("\n\n") + joined;
    }
    else
    {
        // lcc：否则新增一条 user 消息（批尾 tool 结果 flush 后走此分支）
        QJsonObject injected;
        injected[QStringLiteral("role")] = QStringLiteral("user");
        injected[QStringLiteral("content")] = joined;
        m_messages.append(injected);
    }

    qDebug().noquote() << QStringLiteral("[bg] notifications\n%1").arg(joined);
}
