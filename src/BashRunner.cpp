#include "BashRunner.h"

#include "AgentConstants.h" // 超时毫秒/超时文案/输出截断上限单源

#include <QProcess>
#include <QTimer>

namespace BashRunner {

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
    process->start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
    return process;
}

} // namespace BashRunner
