#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

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
 * 对齐 executeTool“一切失败皆字符串”纪律；错误字符串统一加 'Error: ' 前缀——B1 成败判定
 * 单源 AgentLoop::isToolFailure 的文案族，内核原文案作为前缀后的主体保留逐字）。
 *
 * ── s13 Agent Teams 扩展（Lane A：租约台账 + worktree 绑定；lcc s13 34775c8 task_manager.py）──
 *
 * 向后兼容铁律：现有六个 run_* 工具 handler 对「单代理（无团队）」场景的行为保持不变——
 * 五层保障：
 *   ① 构造函数第二参数 workDirSink 带默认值 nullptr，AgentLoop.cpp 单 lambda 构造点零改动可编译；
 *   ② 原 claimTask/completeTask 内核改名 claimTaskUnleased/completeTaskUnleased（签名与文案逐字
 *      不变，含 'Claimed <id> <subject>' 无括号旧文案），runClaimTask/runCompleteTask 继续走它们；
 *   ③ loadTask 对存量旧任务文件（缺 worktree 键）读回视为 null——旧文件照常可读（照 lcc :24-35
 *      dataclass 默认值语义）；
 *   ④ taskToJsonText 新文件恒写 worktree 键（未绑定落 null），键数公式随之放宽但保持
 *      “多杂键即判 Invalid” 的 lite 从严纪律；
 *   ⑤ 全部 s13 回调默认空 = 透传放行 / cwd 回落（workDirSink → sessionRootSink），
 *      单代理场景不经租约路径即零行为漂移；Lead 侧走租约路径时 owner 一律用保留键 "agent"
 *      （lcc assignments 键 'agent'=Lead 预留，与队友名互斥）。
 *
 * 与 lcc 的偏差登记（本扩展共七条，均在对应实现处复注）：
 *   D9  lcc task_manager.py :96-131 的 fcntl/msvcrt 跨进程文件锁整体不移植——lite 单宿主进程，
 *       内存台账足够（决策 D9）；每次操作直读磁盘 + QSaveFile 原子写纪律不变。
 *   D7  租约台账与换工版本号均为纯内存态（lcc assignments/assignment_versions 同款），
 *       进程重启即作废 = fail-closed，由宿主经 release_teammate_assignment 路径清理磁盘残留。
 *   偏3 租约版 claim 成功文案按 s13 逐字 'Claimed <id> (<subject>)'（带括号）；无括号旧文案只
 *       存在于遗留非租约路径。承重契约：lcc spawn/pull 通道以 startswith("Claimed ") 判成功
 *       （agent_teams_manager 跨模块字符串契约），两种形态均满足该前缀。
 *   偏4 lite 两个释放点（release_completed_assignment / release_teammate_assignment）均不递增
 *       换工版本、不触发 advanced 回调（lcc :411/:429 两处都 advance_assignment_version——
 *       lane 有意偏离：版本只在 claim 换工时递增，释放后 claim 门③自然放行，无需另推版本）。
 *       gate① M6 复注钉牢：**fix-4 的 plan gate 复位与 work_version 陈旧推进只挂
 *       onAssignmentReleased**，不得假设「释放伴随版本递增/advanced」（组4/组6 钉桩断言为证）。
 *   偏5 onAssignmentAdvanced 回调签名多带 taskId（lcc on_assignment_advanced(owner) 仅 owner）。
 *   偏6 planGateCheck 按 (owner, taskId) 双值判定（lcc plan_gate_check(owner) 仅 owner——
 *       gate① M5 裁决：P2 的 ProtocolState 身份快照校验需要 owner 键门的同屏 taskId 实参，
 *       双值下发免去宿主反查台账的竞态窗口）。
 *   偏7 Task::worktree 用单态 QString（空串 ≡ python None，判真语义 !isEmpty() 与 lcc
 *       `if task.worktree` 一致）；lcc 为 str|None 两态。worktree_validator/cwd_resolver 两个
 *       lcc 回调在 lite 收敛为单一 cwdResolver（校验折进解析：解析不动即报错 fail-closed）。
 */
class TaskStore
{
public:
    // s13 租约台账条目（lcc assignments[owner] = {"task_id", "cwd"}；键 "agent" = Lead 预留）
    struct Lease
    {
        QString taskId;
        QString cwd;
    };

    // s13 跨模块任务视图（gate① M3）：P2/P3 的程序化消费一律取本结构（经
    // listTaskSnapshots/scanUnclaimedTasks 导出），禁刮 runListTasks/runGetTask 的人类可读
    // 文本——那是模型面契约，形态可随文案迭代破坏。字段为 Task 的对外子集（worktree 存名字，
    // 见 setWorktree 注释）；blockedBy/timestamp 暂无跨模块消费需求，不加。
    // M3 扩形：Gate② FIND-H 裁决恢复 lcc 任务卡 description 通道
    //（lcc agent_teams_manager.py :830-831/:1019——原「description 不外泄是既定形状」
    // 的 P1/R2 判断被 lcc 真码推翻：两处任务卡拼装均消费 description）。
    struct TaskSnapshot
    {
        QString id;
        QString subject;
        QString description; // 任务正文描述（任务卡通道，lcc Task.description 透传）
        QString status;
        QString owner;    // 空串 ≡ 无主（lcc None——偏差⑦单态口径）
        QString worktree; // worktree 名字；空串 ≡ 未绑定（lcc None）
    };

    // sessionRootSink 惰性取宿主会话数据根（仿 CronSchedulerManager 等注入法，见类头注释）
    // workDirSink（s13 新增，默认空保兼容）：cwd 解析回落链的宿主工作目录段——
    // P3 装配根在 AgentLoop 构造点补传；本阶段缺省回落到 sessionRootSink（宿主语义近似）。
    explicit TaskStore(std::function<QString()> sessionRootSink,
                       std::function<QString()> workDirSink = nullptr);

    // 六个工具 handler（mainToolHandlers 表路由同步执行；权限规则不涵盖任务图 → 无权限卡；
    // 钩子文案零改动——toolUseInfo 不加任务图分支，对齐 lcc s10 hooks.py 字节不变）
    QString runCreateTask(const QJsonObject &args) const;
    QString runUpdateTask(const QJsonObject &args) const;
    QString runListTasks() const;
    QString runGetTask(const QJsonObject &args) const;
    QString runClaimTask(const QJsonObject &args) const;     // 遗留非租约路径（向后兼容铁律）
    QString runCompleteTask(const QJsonObject &args) const;  // 遗留非租约路径（向后兼容铁律）

    // ── s13 Lane A：worktree 绑定（P2 WorktreeManager 将调用；本类不触碰 git，仅存 worktree 名字）──
    // 绑定/解绑无状态门（lcc set_worktree 同款：名字合法性/在册校验属 P2 领地——C3 钉桩）；
    // clearWorktree 即置 null（空串 ≡ lcc None = 在主工作目录干活）。
    // gate① M1 修正：绑的是 worktree **名字**不是路径——lcc worktree_manager.py:304 存 name，
    // 台账/解析全按 name 键查（:169-172/:176-179）；name→路径的推导归 P2::worktreePath。
    bool setWorktree(const QString &taskId, const QString &worktreeName, QString *error) const;
    bool clearWorktree(const QString &taskId, QString *error) const;

    // ── s13 Lane A：租约内核（lcc claim_task :312-349 / complete_task :354-396）──
    // 状态机纪律与遗留版一致：业务性失败（门②-⑥不满足/协议否决）按 lcc 以文本经 result 返回
    // （return true）；读盘/校验/台账不可读类失败经 *error 返回（return false），run*Leased 折叠。
    // 六门按 lcc 顺序：①任务可读 ②pending 且无主 ③该 owner 无内存租约 ④磁盘无属于该 owner 的
    // in_progress 任务 ⑤blockedBy 全部 completed ⑥cwd 可解（经 cwdResolver / 回落链）。
    bool claimTask(const QString &taskId, const QString &owner, QString *result, QString *error);
    // 完成：planGateCheck 可否决（否决=业务文本）；租约缺失/指向不符时自愈重建（lcc :370-375）；
    // 完成后【故意不释放租约】——释放只发生在下面两个回合边界 API（lcc :354 铁律：同回合后续
    // 工具仍需租约 cwd 路由），也不递增版本号。
    bool completeTask(const QString &taskId, const QString &owner, QString *result, QString *error);

    // ── s13 Lane A：回合边界两个释放点（lcc release_completed_assignment :400 /
    //    release_teammate_assignment :419）──
    // 仅当租约指向的任务确已 completed 且 owner 相符时清租约并触发 onAssignmentReleased；
    // 其余情形（无租约/未完成/不可读）返回 false 并给出可判定 error 文案，租约保留（幂等安全，
    // 调用方按 false=无需处理即可，不区分原因）。偏差④：不递增版本、不触发 advanced（两释放点统一）。
    bool releaseCompletedAssignment(const QString &owner, QString *error);
    // 队友死亡/退出清理：磁盘遗留 in_progress 任务降级 pending、owner 清空；随后
    // 【无条件】清内存租约 + 触发 onAssignmentReleased（lcc try/finally 语义：内存清理必须
    // 完成，否则死 owner 永久占用名字）。磁盘清理失败时返回 false + error，但内存已净。
    // 无租约无遗留的正常空转返回 true（幂等）。
    bool releaseTeammateAssignment(const QString &owner, QString *error);

    // 裸查询（D8 的热路径缓存属 P2 WorktreeManager 职责，本类不做缓存）
    std::optional<Lease> leaseFor(const QString &owner) const;
    int assignmentVersion(const QString &owner) const; // 无台账记录 = 0

    // 全量快照导出（gate① M3）：复用私有 listTasks——内容损坏文件经 *error 上抛不跳过
    //（对齐 lcc task_manager.py list() :267-274 现行为：load 崩直接上抛；「文件名不合 ID
    // 正则的脏文件跳过」是 listTasks 既有登记的 lite 防御偏差，同口径沿用）。
    // error=nullptr 时错误仍靠 false 判定（nullptr 容错本仓 uniform）。
    bool listTaskSnapshots(QVector<TaskSnapshot> *snapshots, QString *error = nullptr) const;

    // 租约 cwd 指向某目录的 owner 反查（gate① M3；lcc worktree remove 门④ :348-349
    // `Path(a["cwd"]).resolve()==path.resolve()` 的 lite 词法形——QDir::cleanPath 归一后
    // 比较，canonical 复校（符号链接/junction 绕行）留 fix-3 消费点补做。本语义留在
    // TaskStore 单源的理由：remove 门④「拒绝销毁磁盘代码」的最后一道防线数据主人在此）。
    // 纯 const 查询；命中 owner 依字典序追加进 *owners（可为 nullptr=只判有无）。
    bool leasesPointingAt(const QString &dirPath, QStringList *owners) const;

    // 租约 cwd 热路径（gate① M2；lcc worktree_manager.py assignment_cwd :190-212，
    // 「内存租约当缓存、磁盘当真相」）三分支：
    //   ① 无租约且 owner == "agent"（Lead 保留键）→ 回落链 workDirSink → sessionRootSink（lcc :193-194）；
    //   ② 无租约且 owner 为队友 → false + "No active assignment for <owner>" fail-closed（:196）；
    //   ③ 有租约 → 现读盘校验：status ∈ {in_progress, completed} 且 task.owner == owner，否则
    //      false + "Assignment for <owner> is no longer active"（:199-200，completed 放行配合回合
    //      边界退租）；任务绑定了 worktree 时经 resolveTaskCwd 走 resolver，不可解报错
    //      （lcc "Worktree '<name>' binding is broken" 文本由 fix-3 的 resolver 置 *error 承接——
    //      偏差⑦ validator 折进解析）；未绑定走回落链。
    // 计算值 ≠ 台账 cwd 时自愈回写台账，且【不递增版本、不触发 advanced】（lcc :210-211 直接
    // 回写 assignments[owner] 而不调 advance_assignment_version——此边界语义归 fix-4：
    // 挂 plan gate 复位/work_version 推进的只有 claim 换工与释放回调，自愈回写两者皆无）。
    // 因分支③含自愈回写，本函数非 const。
    bool assignmentCwd(const QString &owner, QString *cwd, QString *error);

    // 队友拉活侦察（gate① §3-4；lcc scan_unclaimed_tasks :435-446）：纯侦察只读——pending
    // 且无主且依赖就绪且 cwd 可解（resolveTaskCwd 出错的「worktree 破损」任务不计入候选，
    // lcc `if not error` 同款），产出快照列表。不改任何状态；认领是下一步的事（走 claimTask 六门）。
    bool scanUnclaimedTasks(QVector<TaskSnapshot> *tasks, QString *error) const;

    // ── s13 Lane A：可注入回调（P3 宿主接线；默认空 = 透传放行 / cwd 回落）──
    // 完成前置否决门（lcc plan_gate_check）：返回 false = 否决，reason 原样作为业务文本回传
    // （可为空串=静默否决，lcc 空串拒口径同款）。偏差⑥（M5 改形）：按 (owner, taskId) 双值判定。
    void setPlanGateCheck(
        std::function<bool(const QString &owner, const QString &taskId, QString *reason)> gate);
    // claim 成功换工时通知（lcc :356-368 换工复位 plan gate 的挂点）。偏差⑤：带 taskId。
    void setOnAssignmentAdvanced(std::function<void(const QString &owner, const QString &taskId)> cb);
    // 两个释放点清租约后通知。
    void setOnAssignmentReleased(std::function<void(const QString &owner)> cb);
    // cwd 解析（P2 将接 WorktreeManager::taskWorktreeCwd——lcc worktree_validator +
    // worktree_cwd_resolver 收敛为单一回调，偏差⑦后半；lcc task_worktree_cwd :169-172 按
    // task.worktree 名字查注册表，故实参为任务快照）。约定：置 *error 非空 = 不可解
    // （fail-closed，claim 报 "Cannot claim <id>: <error>"、complete 报
    // "Task <id> cannot complete: <error>"）；返回空串 = 本回调不解析，TaskStore 走回落链
    // （workDirSink → sessionRootSink）。
    // gate① M4 改形：入参 taskId → const TaskSnapshot &（解析须看 worktree 绑定态）；
    // 且 lcc _task_cwd :147-156 的「未绑定任务不起解析、直接回落工作目录」早退语义实现在
    // 本类调用点（resolveTaskCwd 内）：worktree 为空的任务根本不会进 resolver——
    // 未绑定不起子进程（git rev-parse）是热路径性能语义。
    void setCwdResolver(std::function<QString(const TaskSnapshot &task, QString *error)> resolver);

    // ── s13 Lane A：折叠版工具 handler（P3 团队侧接线用；owner 由宿主传入，Lead 用 "agent"）──
    // 内核 false → "Error: " + error；true → result 逐字（claim 成功文本以 "Claimed " 开头，
    // 承重契约见类头偏差③）。
    QString runClaimTaskLeased(const QJsonObject &args, const QString &owner);
    QString runCompleteTaskLeased(const QJsonObject &args, const QString &owner);

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
        // s13 绑定 worktree（lcc s13 34775c8 task_manager.py :24-35 末字段 worktree: str|None=None）：
        // 空串 ≡ None（偏差⑦单态化），语义 = 在宿主主工作目录干活；非空 = 任务绑定的 worktree 路径。
        // 声明序在 blockedBy 之后 = lcc dataclass 声明序 = taskToJsonText 输出序（键名 worktree）。
        QString worktree;
    };

    // 内核方法（对应 lcc TaskManager 各方法；const 者仅读写磁盘，不改动 TaskStore 自身状态）
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
    // s13 改名登记：原 claimTask/completeTask → claimTaskUnleased/completeTaskUnleased
    //（遗留非租约路径，行为逐字不变——向后兼容铁律②）；claimTask/completeTask 之名让给
    // 上方租约版公共内核。
    bool claimTaskUnleased(const QString &taskId, const QString &owner, QString *result, QString *error) const;
    bool completeTaskUnleased(const QString &taskId, const QString &owner, QString *result, QString *error) const;
    // asdict + json.dumps(indent=2) 的等价：键序按 Task 声明序手工输出
    //（id/subject/description/status/owner/timestamp/blockedBy/worktree）
    QString taskToJsonText(const Task &task) const;

    // ── s13 Lane A 私有辅助 ──
    // 换工版本递增 + advanced 回调触发（lcc advance_assignment_version :136-142；偏差⑤），返回新版本
    int advanceAssignmentVersion(const QString &owner, const QString &taskId);
    // cwd 解析（lcc _task_cwd :147-156 的 lite 收敛形——偏差⑦后半）：**M4 早退**——
    // task.worktree 为空（未绑定）→ 不调 resolver，直接回落 workDirSink → sessionRootSink；
    // 已绑定 → resolver(快照)：置错→false（fail-closed）、返回非空→采纳、空串→走回落链。
    // 入参为已读盘 Task（调用方 claim/complete/scan/assignmentCwd 均先 load，免二次读盘）。
    bool resolveTaskCwd(const Task &task, QString *cwd, QString *error) const;
    // 磁盘上属于该 owner 的 in_progress 任务（lcc _owner_in_progress :159-161）。
    // 三态：true=找到（*task 置入）；false 且 error 空=未找到；false 且 error 非空=台账不可读
    //（调用方必须 fail-closed，不得当作“未找到”）。
    bool ownerInProgressTask(const QString &owner, Task *task, QString *error) const;
    // Task → TaskSnapshot 裁剪（gate① M3；owned=false 折叠为空串 owner，lcc None 同款单态形）
    static TaskSnapshot makeSnapshot(const Task &task);

    std::function<QString()> m_sessionRootSink; // 宿主会话数据根惰性获取（见类头注释）
    std::function<QString()> m_workDirSink;     // s13：cwd 回落链的工作目录段（P3 补传，默认可空）

    // s13 Lane A 台账（偏差 D7：纯内存，进程重启即作废；偏差 D9：无跨进程文件锁）
    QHash<QString, Lease> m_assignments;        // owner → 当前租约（键 "agent" = Lead 预留）
    QHash<QString, int> m_assignmentVersions;   // owner → 换工版本（陈旧审批防 TOCTOU 用，P2 消费）

    // s13 Lane A 回调（默认空，见公共 setter 注释）
    std::function<bool(const QString &owner, const QString &taskId, QString *reason)> m_planGateCheck;
    std::function<void(const QString &owner, const QString &taskId)> m_onAssignmentAdvanced;
    std::function<void(const QString &owner)> m_onAssignmentReleased;
    std::function<QString(const TaskSnapshot &task, QString *error)> m_cwdResolver;
};
