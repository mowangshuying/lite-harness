#pragma once

#include "TaskStore.h" // TaskStore::TaskSnapshot 为嵌套类型，限定名查找需完整类定义，故直接 include

#include <QObject>

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

/**
 * WorktreeManager —— git worktree 隔离内核（lcc s13 34775c8 worktree_manager.py 移植，Lane B）
 *
 * 每个任务可绑定一个独立 checkout（worktree），让并行队友在互不踩踏的目录里干活。
 * 同步内核口径（与 MessageBus/TaskStore 同族：零线程、无 GUI 依赖、QtCore-only——
 * QProcess 属 QtCore；同步 git 子进程用 waitForFinished，禁嵌事件循环）。
 * Gate③ MINOR-4（P4b Lane B）追加异步壳 startCreateWorktreeAsync：仅 `git worktree
 * add` 一步改信号驱动 QProcess（十连门/终局分类仍在当前栈同步跑，只读查询的有界
 * waitForFinished 按 D8 登记保留——waitForFinished 不泵应用事件循环，无重入窗口）。
 * 本类为此改挂 QObject 基（**不加 Q_OBJECT**——无自有信号/槽，仅作子进程 parent
 * 与连接 context；无 moc 负担，同步内核语义寸步未变）。
 *
 * 落点（D2 收敛偏差，登记：lcc 原落 workDir 直下 env worktreesDirPath=`.lcc/worktrees`）：
 * `<会话根>/.worktrees/<name>`，叶子段单源于 AgentConst::kWorktreesDirName；
 * git 命令 cwd = workDirSink()（仓库本体）。隔离根本身（.worktrees 目录）永远不得当
 * 任务 cwd 用——lcc :58-65「path == root 也拒」易漏关，本 lane 在 worktreePath 三关复刻。
 *
 * 安全铁律（与 repo-wipe 事故同族，lane 前言级）：**git 失败绝不自动清理**。
 * create 的 worktree add 失败后只做「重查现场 + 分类残留 + 交人工」，绝不反向 rm/删分支；
 * remove 成功后分支也永不删除（lcc :385 文案锚）。任何回滚冲动 = 二次销毁风险。
 *
 * 名字正则与 AgentPathGuard::isValidAgentName **刻意不同源**（gate① M7）：worktree 名
 * 允许 '.'（版本号式命名）但禁内嵌 ".."，字符集/语义与邮箱名不同，两正则永不共享 helper。
 * 路径包含三关复用 AgentPathGuard::isWithinPath；比较口径 **case-insensitive**（Windows
 * 文件系统语义，FIND-C/Gate②：eqCi/withinCi 单源，键原文存储不折叠，CI 只活在比较点；
 * 豁免族保持 CS：任务 id/owner 令牌 + branch/ref 名，git ref 语义，lcc :157 parity）。
 * m1（Gate① minor/Gate② 前置③）已落地：目标**已存在**处经 isWithinPathCanonical 复校
 * 防 junction/符号链接把 .worktrees 及其子项重定向出会话根；**未创建**时保持词法
 * （canonicalFilePath 对不存在路径无解可析，同 send 首帧目录理据）——lcc :59/:62 的
 * .resolve() 对已存在目标恢复同构，对不存在目标登记偏差。
 *
 * 失败折叠纪律（对齐 MessageBus）：createWorktree 直接把 lcc 的折叠串当返回值
 * （'Error: …' / 'Partial operation: …' / 'Partial success: …' / 成功文案），调用方即
 * 工具输出；removeWorktree 走 bool + *error 出参（内核 API，非工具）。
 */
class WorktreeManager : public QObject
{
public:
    // 注册表单条（git worktree list --porcelain 的解析产物）
    struct Entry
    {
        QString worktreePath; // porcelain 'worktree ' 前缀后的原文路径（可能含空格，partition 首空格）
        QString branch;       // 'refs/heads/…' 全名，可能为空（裸 HEAD/detached）
        QString head;         // 'HEAD ' 行的哈希，可能为空
    };

    // store：任务账本（读写 worktree 绑定字段）；两 sink 惰性取根（仿 MessageBus/TaskStore
    // 注入法，每次调用现取不缓存——setWorkDir 切根自然生效）。workDirSink 可为空函数，
    // 届时 git 子进程走继承 cwd（lcc 同构），但路径门①②与 toplevel 比对会 fail-closed 拒。
    // QObject 基（MINOR-4 异步壳）：parent 传空，子进程/定时器挂本对象出清；禁拷贝。
    explicit WorktreeManager(TaskStore *store,
                             std::function<QString()> sessionRootSink,
                             std::function<QString()> workDirSink);

    Q_DISABLE_COPY(WorktreeManager)

    // ---- 工具面（Lead create_worktree 现走本同步内核；异步接线方案见
    // startCreateWorktreeAsync 与交付报告「Lead handler 未接异步」登记）----
    // 十连门（lcc create_worktree :234-312 顺序逐字）：名字/路径三关 → 任务存在 →
    // pending 且未认领 → 任务未绑 worktree → 名字未被别的任务占用 → 目标路径不存在 →
    // workDir 是仓库 toplevel → 分支名合法（check-ref-format）→ 分支不存在（show-ref）→
    // 注册表可读（不可读 fail-closed 拒，绝不按「无 worktree」放行）→ 路径未注册。
    // 全部门禁跑在同步快照上（校验期零 git 写操作，仅只读查询）。
    // 成功链：mkpath 父目录 → `git worktree add -b wt/<name> <path> HEAD` →
    // store->setWorktree(taskId, name)（存**名字**非路径——gate① M1）。
    // git 失败绝不自动清理：重查注册表/分支/目录，按残留分类回 'Partial operation: …'
    // （含人工处置指引），零残留才回 'Git error: …'；add 成功但写回绑定失败回
    // 'Partial success: …Git data was retained…'。成功文案锚：
    // Created worktree '<name>' at <path> for task <task_id>
    // in-flight 名字表为同步/异步**共享**承重门（Gate③ MINOR-4 转承重）：同步路径
    // RAII 出清、异步路径发起 insert + 收口 latch 恰一次 remove；同名并发第二发起
    // 直接折叠 'Error: Worktree '<name>' creation is already in progress' 拒止。
    QString createWorktree(const QString &name, const QString &taskId);

    // ---- 异步壳（Gate③ MINOR-4）----
    // 十连门与 mkpath 在当前栈同步跑（只读查询走 runGit 有界等待，D8 登记）；仅
    // `git worktree add` 改信号驱动 QProcess（parent=this，零线程零嵌套事件循环）。
    // 终局分类复用与同步路径同一 helper——finishCreateFailure / finishCreateSuccess，
    // Partial operation / Git error / Partial success / Created worktree 逐字同产。
    // in-flight QSet：发起前 insert、收口 latch remove；同名第二发起折叠拒止
    // （MINOR-4 转承重——异步在途可达）。
    // done 恰一次契约：门拒/装配缺位/in-flight 拒止在**当前栈同步回调**；异步收口
    // 在信号槽回调。连接 context = this —— WorktreeManager 先亡则 done 不再触发
    // （宿主关停清算语义：Lead 的 executeTool 挂起位由既有 stop/析构链收口，
    // 见报告「Lead handler」登记）。
    void startCreateWorktreeAsync(const QString &name, const QString &taskId,
                                  std::function<void(const QString &result)> done);

    // ---- 内核 API（**刻意不上工具面**：lcc 未暴露 remove_worktree，防误删；
    // 调用方只有宿主关停/人工流程。不进 ToolNames.h——gate① fix-3 专属④）----
    // 五连门（lcc remove_worktree :323-385 顺序逐字）：注册表条目合法（名字/路径/
    // 分支==refs/heads/wt/<name>/目录存在）→ 有任务绑定 → 绑定任务全部 completed →
    // 无活跃租约指向该路径（owners 非空即拒：still in use by …; wait for the turn to end）
    // → status --porcelain --ignored 干净（命令失败按脏处理）。
    // discardChanges=true **只豁免第五门**并追加 --force，其余四门寸步不让。
    // 成功：clearWorktree 解绑（存名口径的反向操作）+ 分支永不删除，文案锚：
    // Worktree '<name>' removed; branch 'wt/<name>' retained
    bool removeWorktree(const QString &name, bool discardChanges, QString *error);

    // ---- P3 接线查询面 ----
    // 解析 `git worktree list --porcelain`（空行哨兵 flush + 逐行 partition 首空格，
    // 路径含空格安全），按 lcc registered_worktrees :118-141 三过滤（根自身/越界、
    // 分支不符、目录缺失/**非目录**）后以**名字**为键返回。根过滤为 CI 口径
    // （eqCi/withinCi，FIND-C：git porcelain 盘符大小写与 sink 拼写的拼写差是典型
    // 假剔除源）。注册表不可读 → *error 置折叠串且返回
    // 空表（fail-closed，绝不返回空表冒充「没有 worktree」——lcc :101-116，fix-3 专属①）。
    QMap<QString, Entry> registeredWorktrees(QString *error) const;

    // 名字是否为合法已注册 worktree（空串=未绑、合法——镜像 lcc is_valid_worktree :176-179）。
    // 供 P3/fix-4 接线 TaskStore::setTaskWorktree 校验钩子（偏差⑦折进 resolver 之外的独立判定）。
    // 注册表不可读一律 false（fail-closed）。名字键查找经 findCi（CI 口径——name=路径
    // 叶段，Windows CI 文件系统语义；键原文存储，登记偏差 lcc python `in dict` CS）。
    bool isWorktreeRegistered(const QString &name) const;

    // TaskStore::setCwdResolver 的目标形态（gate① M4 签名冻结：QString(const TaskSnapshot&,
    // QString*)，TaskStore.h :161-162 注释即约由本函数供「Worktree '<name>' binding is
    // broken for task <id>」错误原文）：
    //  · task.worktree 为空 → 返回空串且**零 git 进程**（M4 早退由 TaskStore 侧保证，
    //    本函数再兜一层：空绑定不查询——热路径每 claim 白跑 rev-parse 不可接受）；
    //  · 绑定可解析 → 返回 worktree 目录绝对串；
    //  · 绑定破损（未注册/目录缺失/分支不符/注册表不可读）→ *error 置折叠串
    //    `worktree '<name>' is not available for task <id>`（gate① M1 钉死文案），
    //    返回空串——TaskStore 侧 nonempty *error = fail-closed 上交调用方。
    // （签名即 TaskStore::setCwdResolver 的 std::function 目标：嵌套类型限定名 TaskStore::TaskSnapshot）
    QString resolveWorktreeCwd(const TaskStore::TaskSnapshot &task, QString *error) const;

    // git worktree 分支名单源（lcc _worktree_branch :68-69）：wt/<name>。
    // remove 成功后分支**永不删除**（历史与重试凭据——fix-3 专属②）。
    static QString branchForWorktree(const QString &name);

    // worktree 目录三关（lcc _worktree_path :47-65，fail-closed 宁拒不猜）：
    // ⓪ 名字 fullmatch `^(?!.*\.\.)[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`（独立单源，见下）；
    // ① 隔离根必须在工作目录内（lcc「Worktrees root escapes the working directory」）；
    // ② 目录路径必须仍在隔离根内（防前缀合法后缀越狱）；
    // ③ **path == root 也拒**（lcc :58-65 易漏关——隔离根本身永远不得当一个 worktree，
    //    否则 "任务 cwd 落在别的 worktree 里面" 的嵌套假象）。isWithinPath 等值返回
    //    true，故本关经 eqCi 单源另判等值（AgentPathGuard.h 头注释同款提醒）。
    // 三关比较一律 CI（eqCi/withinCi 单源，FIND-C——盘符大小写差不得假报越界）；
    // 目标已存在处经 isWithinPathCanonical 复校（m1 已落地，junction/符号链接逃逸
    // fail-closed 拒），未创建保持词法（lcc resolve(strict=False) 差异，偏差已登记）。
    bool worktreePath(const QString &name, QString *path, QString *error) const;

    // worktree 名校验（lcc validate_worktree_name :33-45 的 lite 合一形）。
    // **独立单源**（gate① M7/§3-5：字符集与邮箱名正则不同——此处允许 '.'、首字符必须
    // 字母数字、负向先行断言禁内嵌 ".."；lcc python fullmatch 天然整串锚定，等价物 =
    // hasMatch && capturedStart(0)==0 && capturedLength(0)==size 惯用法）。
    // 失败文案由调用方按 lcc :150-152 折叠（'worktree name must be 1-64 …'），本函数纯判定。
    static bool isValidWorktreeName(const QString &name);

private:
    // git 唯一出口（lcc _run_git :68-92 parity，fix-3 专属⑥）：QProcess 无 shell 列表参数、
    // MergedChannels（stdout+stderr 并流）、waitForFinished(30000)（D8 同款 30s 硬顶）、
    // 超时 kill 计失败、输出 strip；空输出折叠为哨兵 "(no output)"（**逐字**——remove
    // 第五门的「干净」判定依赖该哨兵）。返回 ok=（进程正常结束且 exitCode==0）。
    // 全量输出走本私有口；对上层文本中的 git 输出一律经 truncateForReport（5000 字符外截，
    // lcc run_git :91-92 语义）。
    bool runGit(const QStringList &args, QString *output, const QString &cwd = QString()) const;

    // lcc run_git :91-92 的对外截断形：超 5000 字符裁剪（python 静默切片；lite 有意偏离
    // 追加 "...[truncated]" 标记——账目里被裁掉的输出若不留痕，模型无从知道信息缺失）。
    static QString truncateForReport(const QString &output);

    // 原始注册表解析（lcc _parse_registry :95-116）：键 = cleanPath 后的 worktree 路径
    // 原文（**大小写保留、不折叠**——QMap 键比较本征 CS，Windows CI 语义活在消费点
    // findCi/eqCi/withinCi，FIND-C/Gate② 修正旧失实注释；lcc 键为 Path.resolve()
    // 规范化形，lite 词法键 + CI 查找为登记偏差），未做任何合法性过滤。失败（git 非零/
    // 无法启动/超时）→ *registryError 置
    // 'cannot read Git worktree registry: <输出>' 且返回空表。
    QMap<QString, Entry> parseRegistry(QString *registryError) const;

    // 单条注册项的合法性核验（lcc _registered_entry :143-166 逐字文案）。
    // entry 为**必携**出参：null = 内部缺陷，FIND-A 显式防御直接拒绝——旧 `entry &&`
    // 短路形会在 null 时静默旁路 branch 校验（lcc :157 是无条件检查）；公开 API 不可达，
    // 不设专测。核验链：路径三关错 → 原样上抛；注册表不可读 → 上抛其错；路径不在
    // 注册表（findCi CI 查找，FIND-C：git 键与本地拼法的盘符大小写差不得假报未注册）→
    // "worktree '<name>' is not registered with Git"；路径缺失**或非目录**（FIND-B：
    // lcc :154 is_dir()，普通文件伪装按 missing，判词沿用原文案）→
    // "worktree '<name>' is missing at <path>"；目录在但 isWithinPathCanonical 复校
    // 失败（junction/符号链接重定向出根，m1）→ "Worktree path escapes directory: …"
    // （lcc 此处无复校、其键本为 resolve() 形——lite 更严，偏差登记）；
    // 分支 != refs/heads/wt/<name>（**保持 CS**，git ref 名语义，lcc parity）→
    // "worktree '<name>' is not registered on expected branch 'wt/<name>'"。
    bool registeredEntry(const QString &name, Entry *entry, QString *error) const;

    // python str(exc) 折叠串 → 'Error: ' 前缀形（lcc create/remove 首门同款包装）。
    static QString foldError(const QString &message);

    // python repr() 的轻形近拟（'name'，含单引号时转 "name"）——lcc :65 {name!r} 文案锚用。
    static QString pyRepr(const QString &text);

    // ---- create 共享核（Gate③ MINOR-4：同步/异步两径同源，文案逐字共保）----
    // 门①..⑩（lcc create_worktree :241-298 顺序逐字；真写仅 mkpath 一步）。
    // 调用前置：m_store 已核、path 已由 worktreePath 出、in-flight 已由调用方登记。
    bool createGatesAfterPath(const QString &name, const QString &taskId,
                              const QString &path, const QString &branch,
                              QString *error);

    // add 失败重查分类（lcc :300-310 逐字）：注册表/目录/分支现场 → Partial
    // operation（有残留）/ Git error（零残留）。**绝不自动清理**。
    QString finishCreateFailure(const QString &path, const QString &branch,
                                const QString &taskId, const QString &addOut);

    // add 成功后绑定收口：setWorktree 失败 → Partial success 文案；成功 → Created
    // worktree 锚文案。
    QString finishCreateSuccess(const QString &name, const QString &path,
                                const QString &branch, const QString &taskId);

    // 隔离根 = sessionRoot/.worktrees（叶子段单源，lcc env.py worktreesDirPath 的 lite 收敛）
    QString worktreesRootDir() const;

    TaskStore *m_store;                        // 任务账本（不拥有，宿主注入；lifetime 由宿主保证）
    std::function<QString()> m_sessionRootSink; // 会话数据根惰性获取
    std::function<QString()> m_workDirSink;     // 宿主工作目录（git cwd）惰性获取，可为空函数

    // Gate③ 第7条（P4 Lane A）：createWorktree 在途名字表。同步内核下入口必为空
    // （waitForFinished 不泵事件循环、无重入窗口）——本表为防御性 + Lane B 异步壳
    // 预埋：并发同名 create 由入口拒止（'Error: Worktree '<name>' creation is
    // already in progress'），RAII 出口清表。键大小写敏感（lcc 原语义）；若 Lane B
    // 异步壳启用，需连带 gate④ 的 eqCi 口径评估（Windows 目录 CI，登记于 cpp 注）。
    QSet<QString> m_createInFlight;
};
