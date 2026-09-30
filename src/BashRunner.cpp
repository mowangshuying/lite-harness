#include "BashRunner.h"

#include "AgentConstants.h" // 超时毫秒/超时文案/输出截断上限单源

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace BashRunner {

namespace {

// Resolve the host shell to an absolute path when possible. CreateProcess only
// auto-searches %WINDIR% and %WINDIR%\System32 (not the WindowsPowerShell\v1.0
// subdirectory), so a bare "powershell.exe" relies entirely on PATH: launch
// contexts with a scrubbed PATH fail with FailedToStart (error 2).
QString resolveShell()
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!exe.isEmpty())
        return exe;
    QString windir = qEnvironmentVariable("WINDIR");
    if (windir.isEmpty())
        windir = qEnvironmentVariable("SystemRoot");
    if (windir.isEmpty())
        windir = QStringLiteral("C:/Windows");
    const QString candidate
        = windir + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    if (QFileInfo::exists(candidate))
        return candidate;
    return QStringLiteral("powershell.exe"); // last resort: previous bare-name behavior
}

} // namespace

QString dangerWarning(const QString &command, const QStringList &denyList)
{
    // 大小写不敏感 contains：与主循环/权限门/子代理三方既有匹配口径一致
    for (const QString &danger : denyList)
    {
        if (command.contains(danger, Qt::CaseInsensitive))
            return QStringLiteral("Error: Dangerous command blocked: %1").arg(command);
    }
    return QString();
}

QString truncateOutput(QString text)
{
    if (text.length() > AgentConst::kOutputCharLimit)
        text = text.left(AgentConst::kOutputCharLimit); // 硬截断（lcc [:50000] 等价）
    if (text.isEmpty())
        text = QStringLiteral("(no output)"); // 空输出兜底文案（截断后为空同样兜底，原实现即如此）
    return text;
}

QString finalizeOutput(QProcess *process, bool timedOut)
{
    // 超时分支不读缓冲：kill 之后的残留输出一律以超时文案呈现（原三处实现同口径）
    if (timedOut)
        return AgentConst::kBashTimeoutError;
    return truncateOutput(QString::fromLocal8Bit(process->readAllStandardOutput()));
}

QProcess *start(const QString &command, const QString &workDir, QObject *parent,
                QList<QProcess *> *activeList, const std::shared_ptr<bool> &timedOut,
                const std::function<void(QProcess *)> &arm)
{
    auto *process = new QProcess(parent); // 随宿主析构自动清理（父子关系兜底）
    process->setProcessChannelMode(QProcess::MergedChannels); // stderr 并入 stdout（lcc stderr=STDOUT）
    process->setWorkingDirectory(workDir);
    if (activeList)
        activeList->append(process); // 登记簿：宿主 stop()/cancel()/析构统一 kill

    // 超时 kill：timer 以 process 为 context——进程先销毁则 timer 自动失效，无悬挂回调；
    // timedOut 为 shared_ptr 标志，finished 回调据此区分"超时被杀"与"自然退出"
    QTimer::singleShot(AgentConst::kBashTimeoutMs, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });

    arm(process); // 调用方先 connect finished/errorOccurred（保持原"连接早于启动"时序）
    // 宿主壳 = Windows PowerShell 5.1（本仓仅 Windows）：-NoProfile 跳配置文件防启动干扰、
    // -NonInteractive 禁交互提示挂死；command 经 QProcess 按 Windows argv 规则整段传参，
    // PS 收到原文再按脚本解析——bash 风格命令失败重试的根因修复，与工具描述声明同源
    process->start(resolveShell(),
                   {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                    QStringLiteral("-Command"), command});
    return process;
}

} // namespace BashRunner
