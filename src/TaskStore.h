#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

/**
 * TaskStore —— 任务图文件存储（lcc s10 TaskManager 移植；重构第三轮自 AgentLoop 拆出）
 *
 * 纯存储管理器（仿 CronSchedulerManager/MemoryManager/CompactManager 成员对象先例）：
 * 存储 <会话数据根>/.task/task_<hex8>.json（一任务一文件，每操作直读盘无缓存）。
 * sessionRootSink 惰性取宿主会话数据根（AgentLoop::sessionDataRoot，含 .lite-harness
 * 中间层与按会话隔离的 sessions/<id> 段）——不缓存根路径：setWorkDir 切根后自然生效，
 * 与原内联实现每次调用现取 sessionDataRoot() 的行为逐点等价。
 *
 * 拆分动因：原实现以“SkillManager 档不建类文件”内联在 AgentLoop 后段（约 580 行），
 * 与 LLM 主循环零耦合（无 GUI 依赖、无成员写入——控制台 print → qDebug().noquote()，
 * s04 承接 lcc 控制台输出的移植先例），是 AgentLoop 上帝类的首要拆分对象。
 * 本类对 AgentLoop 零反向依赖：六个工具 handler 为纯文本进出（QJsonObject → QString）。
 *
 * 异常纪律（逐字迁移，各方法注释登记 lcc 偏差）：内核 bool + 错误出参保持 lcc 抛错语义，
 * 六个 run_* 处理器把一切失败折叠为错误字符串直接作为工具输出（lcc 裸抛崩主循环，
 * 对齐 executeTool“一切失败皆字符串”纪律；错误字符串不加 'Error:' 前缀，内核文案逐字即工具输出）。
 */
class TaskStore
{
public:
    // sessionRootSink 惰性取宿主会话数据根（仿 CronSchedulerManager 等注入法，见类头注释）
    explicit TaskStore(std::function<QString()> sessionRootSink);

    // 六个工具 handler（mainToolHandlers 表路由同步执行；权限规则不涵盖任务图 → 无权限卡；
    // 钩子文案零改动——toolUseInfo 不加任务图分支，对齐 lcc s10 hooks.py 字节不变）
    QString runCreateTask(const QJsonObject &args) const;
    QString runUpdateTask(const QJsonObject &args) const;
    QString runListTasks() const;
    QString runGetTask(const QJsonObject &args) const;
    QString runClaimTask(const QJsonObject &args) const;
    QString runCompleteTask(const QJsonObject &args) const;

private:
    // 一条任务记录（python dataclass 移植；原为 AgentLoop 私有嵌套结构体，随本轮拆分迁入本类。
    // 声明序即 taskToJsonText 的键输出序，勿调整）
    struct Task
    {
        QString id;
        QString subject;
        QString description;
        QString status;
        // python 的 owner: str | None 两态 → owned + owner（owned=false ≡ None；文案中呈现 'None'）
        bool owned = false;
        QString owner;
        // 创建时间戳（lcc c3fe3f2 对齐）：对应 python float epoch 秒、create() 时 datetime.now().timestamp()；
        // 声明序在 owner 之后、blockedBy 之前，taskToJsonText 手工拼行需按此声明序输出该键
        double timestamp = 0.0;
        QStringList blockedBy;
    };

    // 内核方法（对应 lcc TaskManager 各方法；全部 const：仅读写磁盘，不改动 TaskStore 自身状态）
    QString taskRootDir() const;
    bool taskFilePath(const QString &taskId, QString *path, QString *error) const;
    bool taskExists(const QString &taskId, bool *exists, QString *error) const;
    bool loadTask(const QString &taskId, Task *task, QString *error) const;
    bool saveTask(const Task &task, QString *error) const;
    bool createTask(const QString &subject, const QString &description, Task *task, QString *error) const;
    // 环检测 DFS（lcc _depends_on）：load 失败容错跳过（状态文件 :109 裁决；与 incompleteDependencies
    // 的“坏依赖计为未完”容错方向相反——lcc 特性原样复刻，勿统一）
    bool dependsOn(const QString &startId, const QString &targetId, bool *depends, QString *error) const;
    bool updateTaskDependencies(const QString &taskId, const QJsonArray &addBlockedBy,
                                Task *updated, QString *error) const;
    bool listTasks(QVector<Task> *tasks, QString *error) const;
    QStringList incompleteDependencies(const Task &task) const;
    bool canStart(const QString &taskId, bool *startable, QString *error) const;
    // 状态机：业务性失败（状态不符/被阻塞）按 lcc 以文本形式经 result 返回（非 *error）；
    // 读盘/校验类失败经 *error 返回，由 run_* 折叠为工具输出
    bool claimTask(const QString &taskId, const QString &owner, QString *result, QString *error) const;
    bool completeTask(const QString &taskId, const QString &owner, QString *result, QString *error) const;
    // asdict + json.dumps(indent=2) 的等价：键序按 Task 声明序手工输出（id/subject/description/status/owner/timestamp/blockedBy）
    QString taskToJsonText(const Task &task) const;

    std::function<QString()> m_sessionRootSink; // 宿主会话数据根惰性获取（见类头注释）
};
