#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QPair>
#include <QStringList>

#include <functional>

#include "AgentConstants.h" // m_maxToolIterations 成员初值取轮次上限默认常量（第十二轮）
#include "BackgroundTasksManager.h"
#include "CompactManager.h"
#include "CronSchedulerManager.h"
#include "MemoryManager.h"
#include "QOpenAi.h" // m_currentStream 类型化弱引用需 ChatStream 完整类型（审计 C8）
#include "TaskStore.h"
// s13 Agent Teams（P3 宿主装配）：引擎三件套按值成员需要完整类型
#include "MessageBus.h"
#include "WorktreeManager.h"
#include "AgentTeamsManager.h"
#include "TeammateRuntime.h" // m_teammateRuntimes 值类型 + 宿主回合驱动

class QProcess;
class SubAgent;
class QTimer;

class AgentLoop : public QObject
{
    Q_OBJECT
public:
    // sessionDataId：会话数据目录短 ID（可空）。非空时持久化数据（任务图/记忆/压缩转写/
    // 工具输出/定时台账/prompt 临时目录）隔离到 <workDir>/.lite-harness/sessions/<id>/，
    // 空则回退全局 <workDir>/.lite-harness/（保证未注入 ID 的独立构造路径行为不变）。
    // skills 始终共享 <workDir>/.lite-harness/skills，不受本 ID 影响。
    // 须在构造时注入：构造体内 m_cron.start() 会装载 durable 台账，先于任何落盘解析定值可免中途切根重复装载。
    // workDir：会话工作目录（空则回落 QDir::currentPath()）。须经构造注入，令 m_workDir 先于构造体内
    // m_cron.start()/scanSkills/初始 system prompt 定值，使各数据根与技能目录随所选目录解析，避免默认目录装载后再切根重载。
    explicit AgentLoop(const QString &sessionDataId = QString(), const QString &workDir = QString(),
                       QObject *parent = nullptr);
    ~AgentLoop() override;

    // 启动代理循环（异步，不阻塞 UI 线程）
    void run(const QString &userMessage);
    // 代理循环是否运行中（UI 预查：运行中勿动旧气泡现场，避免触发 run() 重入卫兵后
    // error 链收掉新气泡导致旧循环后续 delta 无处可落）
    bool isRunning() const { return m_running; }
    // 停止：取消当前流、kill 正在运行的 QProcess，并通知错误
    void stop();

    // 设置工作目录：作为 bash 执行的 cwd，并注入 system prompt（空串忽略，路径归一化为绝对路径）
    void setWorkDir(const QString &dir);
    QString workDir() const;

    // 会话数据目录短 ID（见构造函数注释）。setSessionDataId 仅供未走构造注入的扩展路径调用，
    // 须在首次落盘前设置；正常会话经 ChatSessionPage 构造注入。
    void setSessionDataId(const QString &id);
    // 会话数据目录短 ID（构造注入或 setSessionDataId 设置；空=未隔离）。供上层按 dataId
    // 定位/清理 index.json 条目与会话数据根。
    QString sessionDataId() const;
    // 会话数据根：有 ID → <m_workDir>/.lite-harness/sessions/<id>，无 ID → <m_workDir>/.lite-harness（回退）。
    // 供 CompactManager/MemoryManager/CronSchedulerManager/TaskStore 注入回调使用；
    // 三引擎内部各拼自己的叶子段（.memory/.transcripts/.../scheduled_tasks.json），故 .lite-harness 中间层统一在此拼。
    QString sessionDataRoot() const;

    // 只读会话消息（供 ChatSessionPage 在磁盘恢复后重放 UI；系统消息在下标 0，重放侧自行跳过）
    const QVector<QJsonObject> &messages() const { return m_messages; }
    // 从会话数据根/history.json 恢复历史：保留 [0] 系统消息、追加落盘消息、为缺失结果的 tool_call
    // 回填占位，恢复模型。无 ID / 文件不存在（全新会话）返回 false 且不置 error
    bool loadSavedHistory(QString *error = nullptr);

    // 切换模型：下一轮请求生效（主请求即时读 m_model；压缩/记忆回调为取值 lambda，同样即时读到新值）
    void setModel(const QString &model);
    QString model() const { return m_model; }

    // 上下文 token 口径单源（规格修4）：UI 进度条与压缩触发共用。usage.prompt_tokens 锚有效时
    // = 锚 + 其后新增消息逐条估算 + 注入块变化量；锚失效时回退本地全量估算（含 system[0]、
    // tools schema、注入块——修 B4 触发漏计）。估算单源 AgentConst::estimateTokens /
    // CompactManager::estimateTokens（字符→token 折算）。
    qsizetype estimatedContextTokens() const;

    // 权限门：收到 permissionRequired 后工具队列暂停，UI 取得用户裁决后调用本方法续跑
    // （allow=true 继续执行该工具调用；false 回填 "Permission denied"；无待决询问时忽略）
    void resolvePermission(bool allow);

    // s13 队友名册（侧栏观测面 b 的数据口）：ledger 纯读——teammateNames() 升序名单配
    // statusName() 数据域 token（working/waiting_approval/idle/stopping），缺账默认 Working
    // （与引擎 cpp 侧 value_or 先例同口径）。本地化归 UI 层（侧栏），本方法不产文案
    QList<QPair<QString, QString>> teammateRoster() const;

    // 工具输出成败判定单源（B1）：工具侧一切失败一律折叠为输出文本，文案族固定——
    // "Error:" 前缀（handler 校验/超时/危险拦截/TaskStore 补前缀族）、精确
    // "Permission denied"（权限门拒绝）、"Blocked:" 前缀（deny 列表硬拒）、
    // "[Background task start error]"（后台启动失败）、"Unknown tool:"（未注册名）。
    // 发射点据此算 ok 随 toolOutputReady 带出；历史回放侧以 tool 消息 content 同判据复用
    static bool isToolFailure(const QString &output);

signals:
    // 思考过程增量（forward 给 UI）
    void thinkingDelta(const QString &delta);
    // 回复文本增量（逐字/逐段）
    void textDelta(const QString &delta);
    // 工具执行开始（UI 用于事前 live 卡）：工具名 / 人类可读关键参数摘要（与
    // toolOutputReady 前两参同源生成）。经权限门放行、即将真正执行时发射；
    // compact（批尾特判）与参数 JSON 解析失败（直接终态）不发。权限询问续跑重发时
    // 同名信号可能到达两次，UI 端幂等去重；SubAgent 内部工具不经本信号（黑盒契约不变）
    void toolStarted(const QString &toolName, const QString &summary);
    // 工具执行结果（UI 展示）：工具名 / 人类可读关键参数摘要 / 完整输出 / 成败判定
    // 每次工具执行完成发射一次（含 Dangerous blocked / Unknown tool / 沙箱拒绝等错误结果）。
    // ok 由 isToolFailure(output) 取反单源判定（B1：失败折叠文本在 UI 显形，消费端不再嗅探）
    void toolOutputReady(const QString &toolName, const QString &summary, const QString &output,
                         bool ok);
    // task 子代理内部活动转发（SubAgent::progressEmitted 信号直连，见 startSubAgentTask）：
    // 子代理每完成一个内部工具调用发射一次，仅主循环运行且存在活动子代理时到达。
    // UI 用于 task 工具卡的 live 进度行（turnNo=子代理轮次自 1 起，toolName/summary=
    // 内部工具名与关键参数摘要）；task 终态仍由 toolOutputReady("task") 唯一收口
    void subagentProgress(int turnNo, const QString &toolName, const QString &summary);
    // s13 队友运行态中继（观测面 a 的引擎→页面链路；兑现 P3 装配时「UI 接线留待后续阶段」决策）：
    // teammateProgress = 活动行。type 为数据域 token（QStringLiteral，禁翻区）：turn=回合推进
    // （turnRequested 心跳源）/ result=交付成果 / error=出错 / idle_notification=空闲待命
    //（后三者经 teamEvent 转发，content 取事件原文，turn 时为空）。UI 组件按已知 token 译词条，
    // 未知 token 原样透传（宁可显协议名不编造误导词条）。
    void teammateProgress(const QString &teammateName, const QString &type, const QString &content);
    // teammateSettled = 队友卡终局。outcome 数据域 token：completed=自报完成（taskFinished，
    // 非生命周期终点——队友转 Idle 继续领活，后续活动另起新卡）/ exited=退出（finished）/
    // settled=清算收口（settleTeamOnExit 或页面 runningChanged(false) 兜底扫）。幂等：同名只终局一次
    void teammateSettled(const QString &teammateName, const QString &outcome);
    // teamRosterChanged = 队友名册变化边沿（随 cron tick 1s 节拍签名比对，变才发；零新 QTimer）。
    // 页面订阅后拉 teammateRoster() 推侧栏（setter-push 惯例，侧栏保持纯视图不读引擎）
    void teamRosterChanged();
    // 权限门：工具调用命中询问规则（不含硬拒绝列表项）时发射，队列暂停等待 resolvePermission() 裁决
    // toolName / summary 与 toolOutputReady 前两参同义；reason = 命中规则文案
    //（"Writing outside workspace" / "Potentially destructive command"）
    void permissionRequired(const QString &toolName, const QString &summary, const QString &reason);
    // 待办清单更新（lcc s05 引入，跨车道契约；s06 起为无状态语义）：todo_write 每次校验通过
    // （含清为空清单）后发射，携带本次输入清单的快照 [{content, status}, ...]（不再持久化，
    // 由模型按计划逐轮重发全量清单）；校验失败与权限询问中不发射
    void todoUpdated(const QJsonArray &todos);
    // 记忆沉淀阶段开始（仅自然结束分支，异步化 P2）：finished 同栈随后发射，提取/合并链
    // 已改为 QOpenAi::AsyncRequest 异步执行（startMemoryChain，不再阻塞事件循环），
    // UI 据此定稿 markdown 并在时间线挂记忆进度 live 卡、保留气泡引用供结果卡就地切换
    void memoryPhaseStarted();
    // 记忆沉淀链终态（异步化 P2，设计文档 §3.4）：提取→（stored>=1 时）合并整链结束，
    // 无论成败降级均以 done(0) 收口。UI 据此收尾 live 进度卡并释放保留的气泡引用
    void memoryChainFinished();
    // 定时任务送达（lcc s12）：cron tick 到点且处于空闲边界，交付两条文本——
    // displayText 带 "[Scheduled] " 前缀供 UI 展示，activeRequestText 为无原文前缀拼接的活跃请求
    //（lcc deliver 双路：history 存前缀版、_run_turn 用原文 join）
    void scheduledUserMessage(const QString &displayText, const QString &activeRequestText);
    // 循环结束，最终回复
    void finished(const QString &replyText);
    // 运行态变化（异步化 P2，设计文档 §3.4）：与 m_running 的每次实际翻转同点发射
    //（setRunning 单点收口，同值不发射）。true＝回合开始（含 P1 召回异步飞行期），
    // false＝四类终局（自然/轮次上限/流错误/stop）已收口。UI 订阅本信号驱动输入侧
    // 禁用，覆盖面大于 finished（记忆链尾巴期间仍为 false，输入不禁——链只写 .memory/）
    void runningChanged(bool running);
    // 错误
    void error(const QString &errorMessage);

private:
    // task 子代理需要复用本类的钩子注册表、权限门静态检查、工具定义与 handler 表构建器，
    // 以及 ToolHandler 嵌套类型（对外部接口零暴露，仅友元可见）
    friend class SubAgent;
    // ChatSessionPage 恢复历史时需按实时链路一致口径渲染工具折叠块，复用
    // AgentLoopInternal.h 的 AgentLoopDetail::toolSummary（不为此扩大公开 API 面）
    friend class ChatSessionPage;

    // 发起一次流式聊天请求（P3 起为「压缩前导 + 真实发起」两段式的入口：先跑
    // applyCompactPipelineAsync，压缩续延落地后再 doStartChatRequest）
    void startChatRequest(const QJsonArray &messages);
    // 真实发起段：组请求体、createStream、接三条信号（原 startChatRequest 主体逐字平移；
    // 顶部保留 !m_running 防御卫兵）
    void doStartChatRequest(const QJsonArray &requestMessages);
    // 有工具调用：追加带 tool_calls 的 assistant 消息并进入工具执行链
    void continueWithToolResults(const QJsonObject &assistantMessage);
    // 依次取出待执行工具，全部完成后回填结果并再次请求
    void runNextTool();

    // 工具同步 handler 签名（lcc s06 handlers 字典等价：名称 → args 进、结果文本出；
    // 一切失败都是字符串，无异常）。bash/task 为异步特判路径，不进表（见 executeTool）
    using ToolHandler = std::function<QString(const QJsonObject &args)>;

    // 统一工具执行入口（lcc s06 execute_tool，主循环专用）：先触发 PreToolUse 钩子链
    // （权限门为其中的内置钩子：硬拒绝直接回填 / "ASK:" 前缀暂停队列），然后
    // bash 走异步进程链、task 走子代理链（两个异步特判），其余经 handlers 表同步路由
    // （未注册名称回填 "Unknown tool: <name>"），最后 PostToolUse 钩子 + onToolFinished 统一收口
    // permissionGranted=true 为用户批准后的续跑路径，由 permission 钩子内部短路（仅 resolvePermission 内部使用）
    // compact 工具（lcc s08）在钩子链与表路由之前特判：仅置位 m_compactRequested 并手动推进队列，
    // 不回填结果、不发卡片、不触发 PostToolUse（批尾以 compact_history 整体替换历史）
    void executeTool(const QJsonObject &toolCall, const QHash<QString, ToolHandler> &handlers,
                     bool permissionGranted);
    // 主循环同步 handler 表：read/write/edit/glob（复用 baseFileToolHandlers）+ todo_write + load_skill；
    // bash/task 为异步特判，不在此表；compact 为 schema-only 特判（lcc s08），同样不入表
    // 效率 P4 起本函数仅作「构建一份新表」的实现，供 ensureToolHandlers() 缓存初始化调用；
    // 执行路径勿直接调本函数（旧每调用点整表重建已收敛为缓存复用）
    QHash<QString, ToolHandler> mainToolHandlers();
    // handler 表缓存取用入口（效率 P4）：首用或失效后构建 mainToolHandlers()，返回成员缓存的 const 引用。
    // 失效点仅 setWorkDir（文件四件套按值固化 workDir，见 m_toolHandlers 注释）；
    // runNextTool 与权限批准续跑两处共用本入口，替代原先每执行一个工具调用就整表重建
    const QHash<QString, ToolHandler> &ensureToolHandlers();
    // 基础文件工具 handler 表（lcc s06 主/子代理共享注册形态）：以传入 workDir 为沙箱根，
    // 宿主与子代理各自构建、互不串扰（子代理不经 todo_write/task，工具集为其白名单子集）
    static QHash<QString, ToolHandler> baseFileToolHandlers(const QString &workDir);

    // 权限门·硬拒绝列表（仅 bash）：命中返回 "Blocked: {pattern} is on the deny list"，未命中返回空串
    // （s04 起由内置 permission 钩子调用，逻辑与文案未变；s06 提为静态供子代理共用）
    static QString checkDenyList(const QString &command);
    // 权限门·询问规则：命中返回规则文案（钩子链据此携带 ASK 前缀触发 permissionRequired 暂停），未命中返回空串
    // （s06 起显式传入 workDir：文件工具逃逸判定以该沙箱根为准，供子代理共用同一逻辑）
    static QString checkPermissionRules(const QString &workDir, const QString &toolName,
                                        const QJsonObject &args);
    // —— 内部工具（toolSummary / askPrefix / bashDenyList / parseToolCall）不再经此静态转发：
    //    SubAgent 与 ChatSessionPage 直接 #include "AgentLoopInternal.h" 取用（声明单源、零转发层）。
    // 单个工具执行完成的统一收口（安全/超时/未知/沙箱等快捷路径也走这里）
    void onToolFinished(const QJsonObject &toolCall, const QString &toolName,
                        const QString &summary, const QString &output);
    // 异步执行 bash 命令（带安全检查与超时，不阻塞 UI）。
    // background=true 为后台任务模式（lcc s11）：跳过危险黑名单短路（lcc 黑名单在前台
    // run_bash 内，后台分支绕过——配对已由占位闭合，不可再走 onToolFinished），结束时
    // 不调 onToolFinished/不触发钩子，仅 m_backgroundTasks.recordResult(taskId, ...) 落账
    void executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args,
                          bool background = false, const QString &taskId = QString());
    // 收割后台任务通知并注入会话（lcc s11 loop.py inject_background_results）：
    // 无通知直接返回（一次性消费）；末条为 user 角色则并入其 content 尾部（lcc 末条 user
    // 合并语义），否则新增一条 user 消息。两个挂载点：run() 追加用户消息后、
    // runNextTool() 批尾 flush 之后（compact 之前）
    void injectBackgroundResults();
    // ---- s13 Agent Teams（P3 宿主挂载，实现单源 AgentLoopTeam.cpp）----
    // 引擎装配（构造体末尾调用一次）：setCwdResolver/setWorktreeCreator/
    // setTeammateLauncher/setPermissionCheck/setHooksTrigger/setToolAdapter×5 六注入
    void initTeamEngine();
    // Lead 工具面 cwd（lcc _run_base current_cwd 等价）：有租约→assignmentCwd 现读
    // （worktree 绑定经 resolver 折入）；无租约→m_workDir（assignmentCwd ① 号分支，
    // 与 s13 前行为逐字一致）
    QString leadToolCwd();
    // 收割 Lead 邮箱并注入会话（lcc inject_team_events 形态，复刻
    // injectBackgroundResults 的合并/追加两分支）；空批不产生消息（偏G：空串直接返回）
    void injectTeamEvents();
    // 回合终局清算（lcc agent_loop finally + check_team_offline_edge）：释放 Lead 已完成
    // 租约（幂等）→ 收割注入 → 队友全部下线沿记一次日志；挂 messageFinished 自然收尾前
    void leadTurnEndSettlement();
    // 非自然终局（轮次上限/流错误）只退租不收割（lcc finally 覆盖异常分支的 lite 转译）
    void settleLeadLease();
    // 队友回合驱动：turnRequested→建流（systemPrompt+messages+队友工具面）→
    // messageFinished/error→deliverTurnResult（lcc daemon 线程 consume 循环的事件驱动转译）
    void onTeammateTurnRequested(const QString &name);
    void onTeammateFinished(const QString &name);
    // 退出清算（析构体调用）：对在簿队友逐个 blockSignals→cancel→delete（引擎
    // settleLedgers 保证账本收口；manager/bus/store 此刻仍存活，FIND-L 契约成立）
    void settleTeamOnExit();
    // 空闲态团队事件唤醒交付（Gate③ MAJOR-1：lcc loop.py:340-343 peek("lead") 优先级
    // 首位 + :401-408 wake 分支转译）：cron tick 同拍调用，仅 !m_running 且 Lead 邮箱
    // 有件时收割并经 scheduledUserMessage 同栈直连开新回合（tryDeliverCron 同款先例）
    void tryDeliverTeamEvents();
    // 队友名册变更检测（观测面 b）：随 cron tick 1s 节拍调用（AgentLoop.cpp 挂点），拼
    // name|status 签名与上次比对、仅不同时 emit teamRosterChanged——零新 QTimer，
    // 空名单只在曾有名册时广播一次（初值空串与空名册同签不刷屏）
    void updateTeamRosterBroadcast();
    // 定时任务空闲交付（lcc s12/31a99d1 run_delivery 转译）：仅 m_running=false 时经
    // m_cron.runDelivery 收割——回调内 emit scheduledUserMessage 同栈直连（宿主同步 run()
    // 置位）后回读 m_running 作为接管结果：未接管（无 UI 接线/防御拒绝）→ runDelivery
    // 内部 restoreCronJobs 待下个 tick 重试；已接管 → 本批转入在途，ack 推迟至回合终局
    // （正常收尾/停止/流错误/轮次上限）finalizeInFlightDelivery 收口（lcc 双形态文本见信号注释）
    void tryDeliverCron();
    // task 工具（lcc s06）：启动 SubAgent 异步链（独立上下文黑盒；权限询问经本类
    // permissionRequired 转发；完成回调以汇总文本走 onToolFinished 收口）
    void startSubAgentTask(const QJsonObject &toolCall, const QJsonObject &args);
    // 子代理统一收口（lcc s06 R1）：级联 cancel → kill 流与进程 → 为 task 合成
    // "(cancelled)" tool_result 直写历史 → 半途批一并收口（已完成结果 flush +
    // 剩余调用合成 "(cancelled)"）后清父队列，绝不裸清空致配对断裂；
    // stop()/错误链/析构三路复用
    void cancelSubAgent();
    // 五级压缩异步挂接点（P3，设计文档 §2.3；lcc s08 prepare：发送请求前对会话——不含
    // system——跑五级压缩，有变化则回写 m_messages，裁决 g 保留 m_messages[0] system）：
    // 本地段同步跑，仅触发全量压缩时挂起；
    // 续延在回写压缩结果后以最终消息快照交付 next（未改写则原样透传 callerMessages）。
    // 在途句柄挂 m_sideRequest（与召回共用槽）：压缩中 stop → done 永久静默 →
    // next 不执行、历史不被替换（P3 验证点）
    void applyCompactPipelineAsync(const QJsonArray &callerMessages,
                                   std::function<void(const QJsonArray &requestMessages)> next);
    // 用压缩后的会话（不含 system）替换 m_messages：[system] + conversation 重新拼接
    void applyCompressedConversation(const QVector<QJsonObject> &conversation);
    // 文件类工具（本地 IO，同步执行；经 handler 表路由，参数取自解析后的 arguments JSON）。
    // s06 起为静态并以 workDir 为沙箱根参数：宿主与子代理共用同一实现、各传各的目录
    static QString runReadFileIn(const QString &workDir, const QJsonObject &args);
    static QString runWriteFileIn(const QString &workDir, const QJsonObject &args);
    static QString runEditFileIn(const QString &workDir, const QJsonObject &args);
    static QString runGlobIn(const QString &workDir, const QJsonObject &args);
    // 待办工具（lcc s06 update_todos 等价：无状态）：校验入参并返回本次输入的渲染文本
    // （不落盘不持久，成功时以输入快照发射 todoUpdated）；任一校验失败返回 "Error:..."
    QString runTodoWrite(const QJsonObject &args);
    // 沙箱路径解析（静态化供上述工具共用）：相对路径按 workDir 解析；逃逸时返回空串并置 *error
    static QString safePathIn(const QString &workDir, const QString &p, QString *error);
    // 工具定义（bash / read_file / write_file / edit_file / glob / todo_write / task / load_skill / compact
    // / create_task / update_task / list_tasks / get_task / claim_task / complete_task
    // / schedule_cron / list_crons / cancel_cron
    // / spawn_teammate / list_teammates / send_message / request_shutdown / request_plan
    // / review_plan / create_worktree，共 25 个；
    // s10 六个任务图工具仅注册进主循环表，子代理白名单不含；lcc s12 cron 三件套同为仅主循环注册；
    // s13 团队七件套同为仅主循环注册（lcc TEAM_TOOLS，AGENT_LOOP tools 面）；
    // 队友侧 10 工具 schema 另立于 AgentLoopTeam.cpp（D5：不动本缓存）
    static QJsonArray createToolsDefinition();

    // ---- 技能（lcc s07 SkillManager 内联移植：不建独立类，数据结构与方法置于本类私有段）----
    // 一条技能记录：name/description 取自 SKILL.md 极简 frontmatter（缺省时回落目录名/正文首行），
    // content 保存整份文件的原始文本（含 frontmatter，load_skill 原样返回）
    struct Skill
    {
        QString name;
        QString description;
        QString content;
    };
    // 扫描 <m_workDir>/.lite-harness/skills/*/SKILL.md 重建 m_skills（lcc scan_skills 等价；
    // lite 有意偏差：技能目录收进 .lite-harness 中间目录）：目录缺失静默为空；
    // 目录名升序遍历；同名技能后扫到的覆盖先扫到的（保留先插入位置，对齐 python dict 语义）
    void scanSkills();
    // 技能目录文本（lcc catalog 等价）：空 → "(no skills found)"；否则逐行 "- {name}: {description}" 以 \n 连接
    QString skillsCatalog() const;
    // load_skill 工具 handler（同步表路由，不经权限规则）：命中返回全文；未命中返回
    // "Error: Unknown skill '{name}'"（lcc load 等价）
    QString runLoadSkill(const QJsonObject &args) const;

    // ---- 记忆（lcc s09 MemoryManager：独立类承接存储与三条 LLM 链，本类持有实例）----
    // 以当前工作目录/会话根/技能目录使用说明句重建 m_messages[0] 的 system prompt
    //（规格修1 静态化：调用点=构造/setWorkDir/loadSavedHistory，run() 召回不再逐轮重写 [0]，
    // 技能目录/记忆目录/召回改走注入块（D2 回填技能目录）；lcc build_system_prompt 六段结构的 lite 有意偏离）
    void rebuildSystemPromptMessage();
    // 请求尾部注入块构建的成员包装（模板单源在 AgentLoopPrompt.cpp 匿名 ns；run() 召回续延调用；
    // 经包装透传 skillsCatalog()，签名不含技能参数）
    QString buildContextInjection(const QString &memoryIndex, const QString &memoryText) const;

    // ---- 上下文 token 计量（规格修4：usage 锚定 + 增量估算）----
    // 采纳 ChatStream::usageReceived 回传的 usage.prompt_tokens 为锚（<=0 忽略；同时以发送点
    // 快照 m_lastSendHistoryCount/m_lastSendInjectionTokens 落锚、清历史改写标记）
    void adoptUsageAnchor(const QJsonObject &usage);
    // 非会话口径的固定开销（system[0] + tools schema + 注入块），交 prepareAsync 拆分
    // conversationTokens = estimated − overhead（token 计量升级后压缩触发不再漏计 B4）
    qsizetype contextOverheadTokens() const;

    // 记忆沉淀链启动（异步化 P2，设计文档 §2.2/§3.4）：单槽队列——链空闲则立即发起
    // extractMemoriesAsync，在途则置 m_memoryChainPending（至多补一次，尽力而为语义）；
    // 链 = extract →（stored>=1 时）consolidate → finishMemoryChain，同会话严格串行
    void startMemoryChain();
    // 记忆沉淀链收口（P2）：清 active/句柄 → emit memoryChainFinished → 消费 pending
    //（!m_running 时 singleShot 续跑；m_running 置位则丢弃，新回合终局自然再启）
    void finishMemoryChain();

    // m_running 唯一写入口（异步化 P2，设计文档 §3.4）：状态机收口 + 同点 emit
    // runningChanged，防漏发。红线：run() 内的 setRunning(true) 必须保持同步置位、
    // 先于一切异步发起（tryDeliverCron 回读契约，lcc s12 R3），任何 await 点不得插在本
    // 函数与调用点之间；信号消费方（UI）不得在 runningChanged(false) 栈内启动新回合
    //（此刻 cron finalize/落盘尚未完成，见终局重排注释）
    void setRunning(bool running);

    // 将 m_messages（除 [0] 系统消息）落盘到会话数据根/history.json；无 ID 或历史为空则 no-op。
    // 顺带在索引已登记该会话时刷新 lastActiveMs（不新建条目，登记由 LiteHarness 负责）
    void persistHistory();

    // ---- 定时任务（lcc s12 cron 三件套 handler，mainToolHandlers 表路由；改台账+落盘故非 const；
    // recurring/durable 缺省 true 对齐 lcc run_schedule_cron 默认参数；子代理白名单不含）----
    QString runScheduleCron(const QJsonObject &args);
    QString runCancelCron(const QJsonObject &args);
    QString runListCrons();

    // ---- 生命周期钩子（lcc s04 HOOKS 注册表的内聚移植，事件名 → 有序 handler 列表）----
    // 各事件回调签名；返回空串表示放行/无副作用，非空含义按事件约定：
    // - PreToolUse：硬拦截文本（回填为 tool_result）或 "ASK:<reason>" 前缀（触发异步询问）
    // - Stop：强制继续文本（仅注入历史，不续跑——对齐 lcc 语义，详见 .cpp 注释）
    // - UserPromptSubmit / PostToolUse：约定恒为空（纯日志副作用）
    using UserPromptSubmitHook = std::function<QString(const QString &prompt)>;
    using PreToolUseHook = std::function<QString(const QJsonObject &toolCall, bool permissionGranted)>;
    using PostToolUseHook = std::function<QString(const QJsonObject &toolCall, const QString &output)>;
    using StopHook = std::function<QString()>;

    // 注册内置钩子（构造函数调用；对应 lcc s04 模块尾部的 6 个 register_hook）
    void registerBuiltinHooks();
    // 触发对应事件钩子链：按注册顺序执行，第一个非空返回值短路返回（逐字对齐 lcc trigger_hooks），
    // 全空则返回空串
    QString triggerUserPromptSubmitHooks(const QString &prompt);
    QString triggerPreToolUseHooks(const QJsonObject &toolCall, bool permissionGranted);
    QString triggerPostToolUseHooks(const QJsonObject &toolCall, const QString &output);
    QString triggerStopHooks();

private:
    QString m_model;                 // 模型 ID，从环境变量 MODEL_ID 读取
    QString m_workDir;               // 工作目录，默认 QDir::currentPath()（构造时初始化）
    QString m_sessionDataId;         // 会话数据目录短 ID（构造注入）；空=回退全局 .lite-harness。置于 init-list 末位：子对象声明序无关（其注入 lambda 均为 [this] 惰性读取 sessionDataRoot），真正不变量=成员 init 早于构造体内 m_cron.start() 的 durable 装载
    QVector<Skill> m_skills;         // 技能表（lcc s07）：构造与 setWorkDir 时扫描重建，仅主线程访问
    QVector<QJsonObject> m_messages; // 对话历史（仅主线程访问，无需 mutex）
    bool m_running = false;          // 防并发（尽量只主线程）；P2 起写入一律经 setRunning（runningChanged 同点发射）
    QPointer<QOpenAi::ChatStream> m_currentStream = nullptr; // 当前 ChatStream（弱引用，类型化后取消处免 static_cast 向下转型）
    // 侧链在途请求句柄（异步化 P1，设计文档 §3.4）：m_running 为 true 期间至多一条前链
    // （当前为记忆召回，P3 起压缩侧链共用本槽）。对象归属纪律同 m_currentStream：
    // parent 到 this 随析构自动作废（回调丢弃、在途 reply abort），QPointer 防终态后悬挂；
    // stop() 负责 cancel（AsyncRequest m_done 门闩保证终态后 cancel 为无操作）
    QPointer<QObject> m_sideRequest;
    // 记忆沉淀链在途请求句柄（异步化 P2，设计文档 §3.4）：与 m_sideRequest 分槽——召回属
    // 前链（m_running 为 true，stop() cancel），记忆链属后台尾巴（m_running 已 false，
    // stop() 不 cancel：fire-and-forget，理由见 stop() 内语义分裂注释）。对象归属纪律
    // 同 m_sideRequest：parent 到 this 随析构自动作废，QPointer 防链终态后悬挂
    QPointer<QObject> m_memoryRequest;
    // 记忆链单槽队列（P2，§3.4）：extract→(stored>=1?consolidate:结束) 同会话严格串行，
    // 消除 extract 追加写与 consolidate 重写跨 LLM 等待的交错窗口（跨会话各有独立
    // AgentLoop/存储根，天然隔离）。链在途时新一轮再请求沉淀只留一格 pending：链收口后
    // !m_running 立即续跑（补沉淀本轮增量）、m_running 则丢弃（尽力而为——新回合终局自然
    // 再启整链，旧 pending 数据已含于新回合对话）
    bool m_memoryChainActive = false;
    bool m_memoryChainPending = false;
    int m_toolIterations = 0;        // 工具调用轮次计数
    // 回合入口快照的轮次上限（第十二轮可设置项）：run() 每次读取一次，回合内判定与
    // 报错文案统一用本值——与压缩上限入口单取同型纪律，防回合进行中设置页改值导致
    // 前后判定分叉
    int m_maxToolIterations = AgentConst::kMaxToolIterationsDefault;
    QJsonArray m_pendingToolCalls;   // 待执行 tool 调用队列
    QJsonArray m_toolResultsReady;   // 已执行完的 tool 结果消息
    // 主循环 handler 表缓存（效率 P4：每工具调用整表重建 → 首用构建、setWorkDir 失效）：
    // s13 P3 起 read/write/edit/glob 四件套改由 mainToolHandlers 以 [this] 重建覆盖
    // baseFileToolHandlers 的按值固化版——执行时才现读 leadToolCwd()（Lead 租约感知，
    // 无租约回落 m_workDir），切目录与租约迁移都不再依赖表重建；其余（todo_write/
    // load_skill/任务图六件套/cron 三件套/团队七件套）本就 [this] 惰性读取。
    // setWorkDir 的 clear 保留：baseFileToolHandlers(m_workDir) 仍在表构建时参与拼表，
    // 且失效重建是无害防御（与逐次重建严格等价）；
    // 表在服务端路由期间不被任何路径改写（工具 handler 无一调用 setWorkDir，主线程无嵌套事件循环重入）
    QHash<QString, ToolHandler> m_toolHandlers;
    QList<QProcess *> m_activeProcesses; // 正在运行的 QProcess，stop()/析构时 kill
    QJsonObject m_pendingPermissionCall; // 等待权限裁决的工具调用（队列暂停上下文）
    bool m_awaitingPermission = false;   // 权限询问中（permissionRequired 已发、resolvePermission 未到）
    // task 子代理（lcc s06）：串行队列保证同一时刻至多一个；随本对象父子销毁，QPointer 防回调竞态
    QPointer<SubAgent> m_activeSub;      // 正在运行的子代理（无则为 null）
    QJsonObject m_pendingTaskCall;       // 其对应的 tool call（cancelSubAgent 收口 "(cancelled)" 用）
    // 上下文压缩（lcc s08）：引擎以回调取宿主 workDir/model/卡片出口，本对象构造时注入
    CompactManager m_compact;            // 压缩引擎（转写与落盘目录随 workDir 动态解析）
    bool m_compactRequested = false;     // compact 工具被调用（批尾以 compact_history 替换历史后消费）
    int m_reactiveRetries = 0;           // 反应式压缩重试计数（lcc MAX_REACTIVE_RETRIES=1，每次 run 归零）
    QString m_activeRequest;             // 本轮用户请求原文（摘要消息 "Current user request" 字段）
    // 记忆系统（lcc s09）：引擎以回调取宿主 workDir/model，卡片复用四参 toolOutputReady（"memory"）
    MemoryManager m_memory;              // 记忆引擎（召回/提取/合并，异步回调式，构造时注入回调）
    // 本回合注入块快照（规格修1/D1）：run() 召回续延构建一次、回合内字节恒定；
    // doStartChatRequest 以独立 user 消息追加 payload 尾部，不落 m_messages/history.json
    QString m_contextInjection;
    // ---- 上下文 token 计量锚（规格修4）：prompt_tokens 真值锚 + 发送点快照增量外推 ----
    qint64 m_tokenAnchor = -1;           // 最近一次有效 usage.prompt_tokens；-1=无锚（本地全量兜底）
    qsizetype m_tokenAnchorCount = 0;    // 锚对应的历史条数（发送点 m_messages.size()）
    qsizetype m_anchorInjectionTokens = 0; // 锚对应发送点的注入块估算（当前值-锚值=注入变化量）
    qsizetype m_lastSendHistoryCount = 0;  // 发送点快照：请求发出时 m_messages.size()
    qsizetype m_lastSendInjectionTokens = 0; // 发送点快照：请求发出时注入块估算
    bool m_historyRewrittenSinceAnchor = false; // 锚后历史被压缩/恢复改写 → 锚作废
    mutable qsizetype m_toolsSchemaTokens = 0;  // tools schema 估算惰性缓存（构造后字节恒定）
    // 后台任务（lcc s11 BackgroundTasksManager 纯数据移植）：AgentLoop 每会话一个，即天然
    // 唯一实例——启动与注入共用本成员（lcc 踩过双实例静默丢结果的坑）；进程由
    // executeBashAsync 后台模式异步驱动，仅主线程访问，无锁
    BackgroundTasksManager m_backgroundTasks;
    // 定时任务（lcc s12 CronSchedulerManager 纯数据移植）：同 m_backgroundTasks 单实例纪律——
    // tick 轮询/交付/三 handler 共用本成员；1s QTimer 替代 lcc daemon 线程（登记偏差）
    CronSchedulerManager m_cron;
    QTimer *m_cronTick = nullptr; // 秒级节拍：pollDueJobs + tryDeliverCron（仅主线程）
    // 任务图存储（lcc s10 TaskManager 移植；重构第三轮自本类拆出为独立类文件）：
    // 六个 run_* 经 mainToolHandlers 表直通本成员；sessionRootSink 惰性取会话数据根，
    // setWorkDir 切根后自然生效（与拆分前每操作现取 sessionDataRoot() 逐点等价）
    TaskStore m_taskStore;
    // s13 Agent Teams 引擎三件套（P3 宿主装配，声明序=初始化序：bus 先于 worktrees/teams，
    // 后者构造需 &m_taskStore/&m_bus 地址——成员地址天然稳定）。析构反序（teams→bus→
    // worktrees→store）：TeammateRuntime 不受此保护（其 FIND-L 契约要求 manager/bus/store
    // 后死于 runtime），故队友在 ~AgentLoop 体内的 settleTeamOnExit() 先行显式清算，
    // 运行时从不挂 this 的 QObject 父子树（launcher 内 parent=nullptr）
    MessageBus m_bus;                          // 团队邮箱总线（会话根 .mailboxes，P1）
    WorktreeManager m_worktrees;               // git worktree 隔离（s13 P2a，需 &m_taskStore）
    AgentTeamsManager m_teams;                 // 团队编排引擎（s13 P2，需 &m_bus &m_taskStore）
    // 队友运行时宿主所有权表（引擎 handles 为不拥有引用，偏A）：launcher 建、
    // onTeammateFinished/settleTeamOnExit 销
    QHash<QString, TeammateRuntime *> m_teammateRuntimes;
    // 队友在途回合流（每队友至多一条，单飞防御；parent 到 this 随析构作废）
    QHash<QString, QPointer<QOpenAi::ChatStream>> m_teammateStreams;
    // 上次广播的队友名册签名（name|status 以 ';' 连接）：updateTeamRosterBroadcast 边沿检测单源
    QString m_teamRosterSignature;
    // lcc _team_was_active：队友全部下线的边沿检测（每回合终局比对一次）
    bool m_teamWasActive = false;
    // 四事件钩子链（仅主线程访问；注册顺序即执行顺序，见 registerBuiltinHooks）
    QVector<UserPromptSubmitHook> m_userPromptSubmitHooks;
    QVector<PreToolUseHook> m_preToolUseHooks;
    QVector<PostToolUseHook> m_postToolUseHooks;
    QVector<StopHook> m_stopHooks;

    // 待办清单（lcc s06 起无状态：不再持有持久清单成员，todo_write 每次只校验+渲染入参；
    // TodoItem/renderTodos 保留为纯数据结构与纯函数）
    struct TodoItem
    {
        QString content;
        QString status; // pending | in_progress | completed（校验归一后的值）
    };
    // 渲染面板文本（lcc render 等价的纯函数）：空清单返回 "No todos"
    static QString renderTodos(const QVector<TodoItem> &items);
    int m_roundsSinceTodo = 0;          // lcc rounds_since_todo：每轮用户提问（run）归零起步
    bool m_usedTodoThisRound = false;   // lcc used_todo：每批 tool_calls 开始时归零
};
