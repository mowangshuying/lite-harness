#pragma once

// BashRunner —— bash 工具链执行段单源（AgentLoop::executeBashAsync 前台/后台两分支与
// SubAgent::executeBashAsync 三处曾经整函数级复制：危险黑名单短路文案、QProcess 异步启动、
// 超时 singleShot kill、输出截断与 "(no output)" 兜底，收敛于此）。
// 行为差异（输出汇、取消短路、钩子时序、父对象归属）留在各调用方，本模块不感知；
// 全部常量取自 AgentConstants.h，禁止新增裸字面量。
// 全仓零线程约定：本模块只做事件驱动簿记，不建线程、不阻塞。

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;
class QObject;

namespace BashRunner {

// 危险命令黑名单检查（大小写不敏感 contains，与既有三方调用口径一致）：
// 命中返回 "Error: Dangerous command blocked: <command>"（文案逐字符=原两处实现），
// 未命中返回空串。denyList 由调用方注入（AgentLoop 单源列表），避免对 AgentLoop 的反向依赖。
QString dangerWarning(const QString &command, const QStringList &denyList);

// 输出收尾（lcc [:50000] 截断语义）：超长截断，截断后为空则置 "(no output)"。
// bash 完成回调与 read_file 共用同一兜底口径。
QString truncateOutput(QString text);

// 进程结束后的输出提取：超时分支直接返回 kBashTimeoutError（不读缓冲——
// kill 后残留输出计入结果会掩盖超时事实，原实现即如此）；正常分支
// readAllStandardOutput → fromLocal8Bit → truncateOutput。
QString finalizeOutput(QProcess *process, bool timedOut);

// 创建并异步启动 cmd.exe /c <command>（原三处逐行相同的启动段单源）：
//   new QProcess(parent)（父子归属随宿主销毁）→ MergedChannels（stderr 并入 stdout，
//   lcc stderr=STDOUT 等价）→ setWorkingDirectory(workDir) → 登记进 *activeList
//   供宿主 stop()/cancel()/析构统一 kill → singleShot(kBashTimeoutMs, process, ...)
//   超时置 *timedOut 并 kill（timer 以 process 为 context，进程销毁即自动失效，
//   timedOut 用 shared_ptr 保证回调触发瞬间仍可读）。
// 时序与既有实现逐点一致：建进程 → 挂超时 → arm(process)（调用方在此 connect
// finished/errorOccurred，必须先于 start 完成，消除"信号先于连接到达"竞态窗口）→ start。
QProcess *start(const QString &command, const QString &workDir, QObject *parent,
                QList<QProcess *> *activeList, const std::shared_ptr<bool> &timedOut,
                const std::function<void(QProcess *)> &arm);

} // namespace BashRunner
