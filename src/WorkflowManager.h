#pragma once

// 工作流纯引擎件（lcc s16 42a555d 移植；规格参照 3rdparty/lcc workflow_manager.py s16 时点 892 行）
//
// 定位：工作流注册表 + 身份/校验/台账/文件产物/进程内抢占等「无状态机械件」单源，与
// McpManager / AgentTeamsManager 同形：非 QObject、纯 QtCore-only、零 GUI、零线程，宿主以
// 值成员持有。执行环（asyncio 编排、agent 调用、journal 回放续跑）属 WorkflowRuntime（Lane B），
// 本类只提供它消费的判据与产物。
//
// 与 lcc 的偏差登记（逐编号，行锚 lcc s16 42a555d）：
//   1. 跨进程文件锁不采（lcc :706-738 两层锁：threading.Lock + fcntl/msvcrt）：lite 单 GUI 进程
//      无第二入口，进程内 active-run 集合（QSet）足够表达独占语义；:714/:725 两层的
//      "workflow run {runId} is already active" 文案属 Lane B 宿主层，本类只提供 bool 抢占。
//   2. handler 内同步阻塞编排（lcc call() 的 asyncio.run）不采：执行环属 Lane B 异步形，
//      本类全部函数无事件循环依赖。
//   3. Python 内置 hash → sha256 stable_hash 语义照采（lcc :68-71）；%10**10（:320）与
//      %4/%5（:164-166）的大数取模经摘要字节逐项折叠实现，与 Python 大整数模**逐位相等**
//      （比「取前 8 字节」近似更强的落点，登记于此）。跨语言 resume 本不互通，只要求进程内确定性。
//   4. env.workflowDirPath（lcc :619）→ 宿主以 setStoreDir 注入 sessionDataRoot()/.workflows
//      （目录名常量属 Lane C，本头不建常量）；storeDir 为空 = 功能未就绪，reserveRunId 回
//      lite 自有守卫串 "workflow store dir is not configured"（lcc 无对应——其 env 恒有值）。
//   5. print/log_info("wf", ...) 族 → LOG_INFO 经 LiteLog.h，前缀逐字 "[wf] "（lcc :358 有意差异同款）。
//   6. QJsonObject 键序为字典序（Qt 保证），Python dict 为插入序：schema properties 的枚举校验
//      顺序、fill_schema 无 required 时的键序、progress_event 日志 kv 顺序在多处违规/多键场景下
//      报告次序可能不同——判定语义不变，仅「先报哪个」可能异，登记。
//   7. enum 错误文案 f"expected one of {list}"（lcc :119）的 Python repr（单引号 ['a']）转译为
//      JSON 紧凑数组文本 expected one of ["a"]——鸭子文案交还模型，形态差异无行为影响。
//   8. json.dumps(schema, sort_keys=True)（lcc :319）默认 ensure_ascii=True 把非 ASCII 转 \uXXXX；
//      Qt toJson 直出 UTF-8——非 ASCII schema 的 journal key 与 Python 值不同（跨语言本不互通，
//      进程内确定性仍成立）。
//   9. json.loads 接受任意顶层值（lcc :228）：lite 以「[文本] 数组包裹解析再取首元素」等效达成
//      标量平齐，object/array/标量与 Python 同口径（绕开 QJsonDocument 顶层标量的版本差异）。
//  10. _writeJson（lcc :742-748）.tmp+os.replace → QSaveFile 同原子语义；indent=2 vs Qt Indented
//      宽度=观感差异。O_EXCL 0o600（:678）Qt NewOnly 无权限位形，lite 不设（登记）。
//  11. fill_schema 缺 properties 键会 KeyError（lcc :160 直接下标）；lite 防御为「空子 schema →
//      叶子取 seed 末段」，不崩、不死循环。array 缺 items（lcc :162 同样 KeyError）同理。
//  12. WfTask 的 usage dict（lcc :365）展平为 agents/tokens 两字段——serializeTask 仍按
//      {"agents":..,"tokens":..} 原形输出，JSON 契约不动。
//  13. Python None → C++：metaFor 查无返回空 WfMeta；systemPromptNote 空注册表返回空串
//      （lcc :641-642 None）；readLastRun 无值返回空串；journal MISS 哨兵（lcc :59/:323）以
//      cached() 的 bool 返回值转译——合法 null 值命中时 out 为 null 且返回 true，与 MISS 可区分。
//  14. create_run_id(meta)（lcc :74-75）→ createRunId(name)：C++ 无 dict，只取 meta['name'] 一项。
//  15. validate_run_id 的 isinstance(str) 前置（lcc :83）与 _checkPermission 的 denied 文案
//      （lcc :779）、call() 全流程（lcc :784+）均属宿主/Lane B 层，本类不建。
//  16. mockRun 返回类型契约修正：任务书 item 4 写 QJsonObject，但 schema 空分支返回字符串值
//      （"[mock] ..."）→定稿 QJsonValue。tokens 口径：Python len()=码点、json.dumps 走
//      ensure_ascii 转义；lite 用 UTF-16 单元/裸 UTF-8 紧凑文本——纯 ASCII 输入下完全一致，
//      且 mock 路仅离线验证用，不进模型对话。
//  17. parseRunnerJson 错误文案归位按任务末版 item 5：:239 的
//      "workflow agent returned invalid JSON" 以可选 error 出参承载（raise→错误串转译）；
//      Lane B 若自拼文案可忽略出参。
//  18. reserveRunId 双形并存：候选 in/out 形（任务书预授权契约修正）与 name 新生成形
//      （lcc :670-683 meta 形直译，任务末版 item 9）——后者即「首轮候选=createRunId(name)」
//      的前者包装，总尝试数同为 32，Lane B 取用自便；readSnapshot 定稿 (storeDir, runId, error)
//      （任务末版 item 9），path 函数内拼 {storeDir}/{runId}.json。
//  19. storeDir 注入取 setStoreDir 推形而非 Cron 拉形 sink（任务 item 12 授权自选）：workflow
//      台账与 McpManager 同为会话级值成员，构造/切换时推入即可，无跨会话共享诉求。
//  20. mockRun（lcc MockAgentRunner :177-205）：Python 以 schema=None 走 "[mock] " 文本分支，
//      C++ 无 None——约定「空对象 {} ⇔ 无 schema」，调用方（Lane B）传 QJsonObject() 即同口径。
//  21. response_text（lcc :208-214）对 content 块按 type=="text" 过滤 join：lite 的 AsyncRequest
//      已回纯文本、无块结构层，responseText 收敛为 trim 收口（保留尾 .strip() 口径）。
//  22. WORKFLOW_TOOL（lcc :595-612）为 Anthropic input_schema 形：lite 转译为 OpenAI function
//      信封（仿 McpManager 工具装配先例）；工具名 "workflow" 不入 ToolNames.h 名单（禁区），
//      请求池装配与分发接线属 Lane C。
//
// 禁翻区纪律：发往模型与进 journal/快照的英文逐字串一律 QStringLiteral，禁 tr()。
// 零线程原则同仓规：本类纯内存 + 显式文件 IO（QSaveFile/QFile），无事件循环、无定时器。

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

// 运行时护栏（lcc s16 42a555d :57-58/:670-683；对照 Lane B 计数门/预留重试环使用）
inline constexpr int kWorkflowAgentCap = 1000;        // AGENT_CAP：单次运行 agent() 硬上限（含嵌套）
inline constexpr int kWorkflowConcurrency = 8;        // CONCURRENCY：agent() 并发上限（信号量属 Lane B）
inline constexpr int kWorkflowRunIdReserveAttempts = 32; // reserve_run_id 的 range(32) 总尝试数

// ---- runner 注入缝（Lane B/C 消费；lcc :242-283 AnthropicAgentRunner 同步 run() 的 lite 异步转译）----
// done 恰一次回调；error 非空=失败；schemaJson 空=无 schema（lcc schema=None 对应）。
// 执行环调度属 WorkflowRuntime（Lane B），本头只定契约类型。
using WfAgentDone = std::function<void(const QJsonValue &value, qint64 tokens, const QString &error)>;
using WfAgentRunner = std::function<void(const QString &prompt, const QString &schemaJson,
                                         const QString &label, WfAgentDone done)>;

// 工作流元数据（lcc :88-106 validate_meta 的受检对象；C++ 强类型下 description 必为 str、
// meta 必为对象——对应两条错误串不可达，见 validateMeta 注释）
struct WfMeta
{
    QString name;
    QString description;
    QStringList phases;
};

// 任务台账（lcc LocalWorkflowTask :356-375）：一次运行的状态、用量与进度事件。
// 宿主持有实例并在执行环各节点调用 event/progressEvent；agents/tokens 由宿主累加（lcc 同形）。
struct WfTask
{
    QString taskId;
    QString runId;
    QString workflowName;
    QString status = QStringLiteral("running"); // running | completed | failed（lcc :364 起点）
    int agents = 0;                              // usage["agents"]（lcc :365，展平登记偏差⑿）
    qint64 tokens = 0;                           // usage["tokens"]
    QJsonArray progress;                         // progress_event 累积（lcc :366）

    // 事件日志（lcc :368-370）：LOG_INFO "[wf] {name} {k=v 空格连}"，尾部空白 rstrip 逐字。
    // kvLines 为已格式化 "k=v" 串列表——格式化经 jsonValueLogText 口径由调用方完成（日志形同 lcc f-string）。
    void event(const QString &name, const QStringList &kvLines);

    // 进度事件（lcc :372-375）：append {"type": ptype, ...data}，随后同 event 打日志
    // （前缀 "progress {ptype} ..."）。data 键序按偏差⑥；type 键若被 data 覆盖与 lcc {"type":..., **data} 同形。
    void progressEvent(const QString &ptype, const QJsonObject &data);
};

// token 预算（lcc Budget :334-353）：total=-1 表示不限（Python None 转译，偏差⑬）。
// 花超即拒并回错误串（绝不静默超支，且超支那次不计数——与 lcc raise-before-+= 同形）。
class WfBudget
{
public:
    explicit WfBudget(qint64 total = -1);
    bool add(qint64 n, QString *error); // false + *error="token budget exceeded ({spent+n} > {total})"（逐字 :345）
    qint64 remaining() const;           // 不限时返回 int64 最大值（float('inf') 转译；Lane B 判预算请用 total()<0，勿比较 remaining 上限）
    qint64 spent() const;
    qint64 total() const;               // -1 = 不限

private:
    qint64 m_total;
    qint64 m_spent = 0;
};

// agent 计数上限（lcc ExecutionLimits :378-388 的 cap 部分；semaphore 属 Lane B 执行环）。
// claim 语义逐字 lcc：先自增再判限——超限失败后 agentsUsed() 停在 cap+1（:386-387 先 +=1 后 raise）。
class WfExecutionLimits
{
public:
    bool claimAgent(QString *error); // 超限 false + "agent() cap reached (1000)"（数字经 kWorkflowAgentCap，:388）
    int agentsUsed() const;

private:
    int m_agents = 0;
};

// 追加式 <runId>.journal.jsonl（lcc WorkflowJournal :286-331）：resume 时语义 key 已存在的
// agent() 直接回放缓存，不再调用模型。坏行 fail-closed——绝不当作「没跑过」（:307-310 逐字）。
// 析构调 close()（RAII，对应 lcc :840-841 finally close，登记）。
class WfJournal
{
public:
    WfJournal();
    ~WfJournal();

    // resume=true：文件缺失→false+"resume journal not found for {runId}"（:296-297）；
    // 逐行必须为含 str "key" 且携 "value" 的对象，坏行→false+"invalid resume journal record at line {n}"
    // （1 起算，:298-310）。resume 以 Append 打开续写，新运行 Truncate（:312-314）。
    bool open(const QString &storeDir, const QString &runId, bool resume, QString *error);
    bool cached(const QString &key, QJsonValue *out) const; // MISS 哨兵转译 bool（偏差⑬）
    void record(const QString &key, const QJsonValue &value); // 一行 {"key":..,"value":..} Compact+flush+缓存更新（:326-328）
    void close();

private:
    QFile m_file;
    QHash<QString, QJsonValue> m_cache;
};

// 注册表 + 身份/校验/文件产物/进程内抢占（lcc WorkflowManager 的无执行环子集）
class WorkflowManager
{
public:
    // 内置注册表：仅 review-changes 一条（lcc :527-531/:622-624 逐字）。执行脚本属 Lane B，
    // 本构造只建 meta。
    WorkflowManager();

    // 存储根目录：宿主在 sessionDataRoot 变更时推入 <会话根>/.workflows（偏差④）；空=未就绪。
    void setStoreDir(const QString &dir);
    QString storeDir() const;

    // ---- 注册表与策略（lcc :622-624/:586-591/:638-655）----

    QStringList workflowNames() const;                 // 单条目注册表；多条目时键序欠账见偏差⑥
    bool hasWorkflow(const QString &name) const;
    WfMeta metaFor(const QString &name) const;         // 查无=空 meta（None→空转译，偏差⑬）
    // 模型侧清单提示（lcc :638-646）：空注册表→空串；否则逐字
    // "Registered workflows (call the workflow tool with one of these names):\n- {name}: {description}"
    // （每行一条 \n 连、无尾 \n）
    QString systemPromptNote() const;
    // OpenAI function 信封（lcc WORKFLOW_TOOL :595-612 的 Anthropic 形转译为 OpenAI 信封，仿 McpManager 工具装配先例）：
    // {type:"function",function:{name:"workflow",description:<逐字>,parameters:{...}}}
    QJsonObject toolInfo() const;
    // 宿主授权（lcc :648-655）：deny 表命中→"deny"；策略表 {"review-changes":"allow"}；
    // 未登记/空名→"confirm"（fail-closed，同 s14 MCP 策略缺省）
    QString policyFor(const QString &name) const;

    // ---- 静态纯函数（公开供单测直接调用，仿 McpManager::normalizeName 纪律）----

    // sha256(text) 十六进制小写全串（lcc stable_hash :68-71 的字节序大整数在本仓以 hex 承载；
    // %N 取模走内部折叠助手与大整数模逐位相等，见偏差③）
    static QString stableHash(const QString &text);
    // "wf_{name}_{16 位小写 hex}"（lcc :74-75；随机源 QRandomGenerator::system，token_hex(8) 同款 8 字节）
    static QString createRunId(const QString &name);
    // "local_workflow_{runId}"（lcc :78-79）
    static QString createTaskId(const QString &runId);
    // ^wf_[A-Za-z0-9][A-Za-z0-9._-]{0,63}_[0-9a-f]{16}$（lcc :61/:82-85）；败→"invalid workflow runId"
    static bool validateRunId(const QString &runId, QString *error);
    // lcc :88-106 五条错误串中 "meta must be an object literal"（:92）与
    // "meta.description must be a string"（:100）在 C++ 强类型下不可达（结构体必为对象、
    // description 必为 QString）——刻意不建死码；phases 空列表视同 Python「键缺失」放行（:101），
    // 含空串条目→"meta.phases must be a list of non-empty strings"。
    static bool validateMeta(const WfMeta &meta, QString *error);
    // journal 语义 key（lcc :316-320）：basis="{kind}|{label}|{prompt}|{schemaCanonical}"，
    // 返回 "{kind}-{sha256(basis) % 10^10 补零 10 位}"。schemaCanonical：Null→"null"，
    // 其余 QJsonDocument(单元素数组).toJson(Compact) 剥外层括号——Qt 键字典序 = sort_keys=True（偏差⑥⑧）。
    // 与并发顺序无关：parallel/pipeline 调换调用顺序，同 (kind,label,prompt,schema) 仍是同一步（:317-318）。
    // label 缺省形（prompt 前 24 字符+略号）由 ExecutionState 侧算（lcc §10-7），本函数只吃显式 label。
    static QString agentKey(const QString &kind, const QString &label,
                            const QString &prompt, const QJsonValue &schema);
    // SimpleJsonSchema.validate（lcc :116-152）全量等价：enum 先于 type（:118-119）；
    // object/array/string/boolean/number(+integer 且拒 bool)/required/嵌套，逐字错误串族；
    // 未知 type 或无 type 放行（:152）；空 schema=无约束。败→false + *error=首个错误串。
    static bool validateJsonSchema(const QJsonValue &value, const QJsonObject &schema, QString *error);
    // Mock runner 确定性填充器（lcc fill_schema :155-167）：object 取键按 required（空则回落
    // properties）逐键递归（seed="{seed}/{k}"）；array 单项（seed 尾 "/0"）；boolean=hash%4!=0；number=hash%5；
    // 叶子=seed 末段。缺 properties 键/array 缺 items 的 KeyError 防御见偏差⑾。
    static QJsonValue fillSchema(const QJsonObject &schema, const QString &seed);
    // 确定性 Mock runner（lcc MockAgentRunner.run :177-205）：schema 空（{}≈Python None）→
    // value="[mock] "+(label 非空取 label 否则 prompt).left(60)；properties 含 "findings"→
    // 1+hash(prompt)%2 条 {title:"{label 或 audit} #{i+1}", severity:[high,medium,low][hash(prompt+i)%3]}
    // （:187-194）；含 "isReal"→{isReal:hash(prompt)%4!=0, reason:"reproduced"/"could not reproduce"}
    // （:195-198）；否则 fillSchema(schema,prompt)（:200）。tokensOut=prompt.length()/4+
    // 紧凑序列化文本长/4（_tokens :203-205）。返回定稿 QJsonValue（偏差⑯）。
    static QJsonValue mockRun(const QString &prompt, const QJsonObject &schema,
                              const QString &label, qint64 *tokensOut);
    // 模型回复抠 JSON（lcc parse_runner_json :217-239）：剥 ``` 围栏→整串 parse（任意顶层值，偏差⑨）
    // →失败则逐个 '{' 位置取配平候选解析、首个合法对象胜出（容忍前后噪声）；全败 ok=false。
    // error 可选出参承载 :239 文案（偏差⑰，任务末版归位），Lane B 可忽略自拼。
    static QJsonValue parseRunnerJson(const QString &text, bool *ok, QString *error = nullptr);
    // lcc response_text（:208-214）的 lite 转译：AsyncRequest 已回纯文本，无 text 块拼接层，
    // 只剩 .strip() 收口（偏差登记于头注释 5 段之外的补充：join 无对应）。
    static QString responseText(const QString &raw);

    // ---- 文件产物与预留（lcc :670-683/:742-771）----

    // 在任何 journal 被截断之前预留运行身份：storeDir/<runId>.json 以 NewOnly 独占创建；
    // 撞名换全新随机 id 重试（含候选共 kWorkflowRunIdReserveAttempts 次，lcc range(32) :674）。
    // 契约修正（任务书预授权）：入参候选、出参实际预留名——*runIdInOut 成功后写回真实 id
    // （lcc 的 O_EXCL 循环语义在 C++ 无异常形下的等价表达）。候选先过 validateRunId；
    // storeDir 空→lite 守卫串（偏差④）。耗尽→"could not allocate a unique workflow runId"（:683）。
    bool reserveRunId(QString *runIdInOut, QString *error);
    // lcc reserve_run_id(meta) :670-683 同形便捷重载：先 createRunId(name) 再入上形循环
    // （总尝试数仍 32，偏差⑱）。成功后 *runIdOut=实际预留名。
    bool reserveRunId(const QString &name, QString *runIdOut, QString *error);
    // 原子写：QSaveFile Indented（lcc .tmp+os.replace indent=2 :742-748，缩进宽度见偏差⑽）
    static bool writeJsonAtomic(const QString &path, const QJsonValue &value);
    // 读快照（lcc _readSnapshot :750-760）：path={storeDir}/{runId}.json 函数内拼（与 lcc 同形）；
    // 缺→"resume snapshot not found for {runId}"；解析失败/非对象→"invalid resume snapshot for {runId}"。
    // 签名定稿 (storeDir, runId, error)（偏差⑱）。
    static QJsonObject readSnapshot(const QString &storeDir, const QString &runId, QString *error);
    // 写 storeDir/last_run.txt 裸 runId + 内存优先（lcc :762-771）；storeDir 空=仅内存（登记）。
    void saveLastRun(const QString &runId);
    QString readLastRun() const;

    // ---- 进程内抢占（替代 lcc 两层锁，偏差①；lcc :706-738）----

    bool tryAcquireRunActive(const QString &runId); // 已 active=false；上层文案属 Lane B（偏差①）
    void releaseRunActive(const QString &runId);

    // 任务态 JSON 安全视图（lcc serialize_task :497-507）：
    // {taskId,taskType:"local_workflow",runId,workflowName,status,usage:{agents,tokens},progress}
    QJsonObject serializeTask(const WfTask &task) const;

private:
    QHash<QString, WfMeta> m_registry;
    QString m_storeDir;
    QString m_lastRunId;
    QSet<QString> m_activeRuns;
};
