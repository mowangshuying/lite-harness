#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QStringList>

#include <functional>

#include "BackgroundTasksManager.h"
#include "CompactManager.h"
#include "CronSchedulerManager.h"
#include "MemoryManager.h"
#include "TaskStore.h"

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

    // 权限门：收到 permissionRequired 后工具队列暂停，UI 取得用户裁决后调用本方法续跑
    // （allow=true 继续执行该工具调用；false 回填 "Permission denied"；无待决询问时忽略）
    void resolvePermission(bool allow);

signals:
    // 思考过程增量（forward 给 UI）
    void thinkingDelta(const QString &delta);
    // 回复文本增量（逐字/逐段）
    void textDelta(const QString &delta);
    // 工具执行结果（UI 展示）：工具名 / 人类可读关键参数摘要 / 完整输出
    // 每次工具执行完成发射一次（含 Dangerous blocked / Unknown tool / 沙箱拒绝等错误结果）
    void toolOutputReady(const QString &toolName, const QString &summary, const QString &output);
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
    // ChatSessionPage 恢复历史时需按实时链路一致口径渲染工具折叠块，复用私有静态
    // toolSummaryOf（与 SubAgent 同走友元通道，不为此扩大公开 API 面）
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
    QHash<QString, ToolHandler> mainToolHandlers();
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
    // —— 供 SubAgent（友元）复用同一份实现的静态转发（转调 .cpp 内部同名工具函数）——
    static const QString &askPrefixOf();
    static QString toolSummaryOf(const QString &toolName, const QJsonObject &args);
    // lcc fddb23e G4 单源：bash 危险黑名单与权限门 DENY_LIST 共用同一份列表
    static const QStringList &bashDenyList();
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
    // "(cancelled)" tool_result 直写历史 → 清父队列；stop()/错误链/析构三路复用
    void cancelSubAgent();
    // 压缩流水线挂接点（lcc s08）：发送请求前对会话（不含 system）跑 prepare() 五级压缩，
    // 有变化则回写 m_messages（裁决 g：保留 m_messages[0] system）；返回是否发生了改写。
    // 同步版仅作迁移期兼容面（P3 后 startChatRequest 已改走异步版，P4 删净）
    bool applyCompactPipeline();
    // 五级压缩异步挂接点（P3，设计文档 §2.3）：本地段同步跑，仅触发全量压缩时挂起；
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
    // / schedule_cron / list_crons / cancel_cron，共 18 个；
    // s10 六个任务图工具仅注册进主循环表，子代理白名单不含；lcc s12 cron 三件套同为仅主循环注册）
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
    // 以当前工作目录/技能目录/记忆索引/本轮召回记录重建 m_messages[0] 的 system prompt
    // （构造、setWorkDir 与每轮 run() 召回后调用；lcc build_system_prompt 六段结构（含 lcc 7e33a8e temp 段）的 lite 等价）
    void rebuildSystemPromptMessage();

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
    QPointer<QObject> m_currentStream = nullptr; // 当前 ChatStream（弱引用）
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
    QJsonArray m_pendingToolCalls;   // 待执行 tool 调用队列
    QJsonArray m_toolResultsReady;   // 已执行完的 tool 结果消息
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
    // 记忆系统（lcc s09）：引擎以回调取宿主 workDir/model，卡片复用三参 toolOutputReady（"memory"）
    MemoryManager m_memory;              // 记忆引擎（召回/提取/合并，阻塞式，构造时注入回调）
    QString m_relevantMemories;          // 本轮召回的记录文本（system prompt 尾段；run() 时刷新）
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