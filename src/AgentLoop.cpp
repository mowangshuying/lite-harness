// AgentLoop 核心：生命周期与会话身份（构造/析构、工作目录与会话数据根、模型、回合入口与终局）。
// 本类的其余职责见同名前缀的兄弟文件（AgentLoopRequest / Tools / Bash / SubAgent / Permission /
// FileTools / Skills / Todo / Cron / Hooks / History / Prompt）。
// AgentLoop —— 本类按职责拆分为多个编译单元（同一个类，方法分布在不同 .cpp），分工如下：
//   AgentLoop.cpp            生命周期与会话身份：构造/析构、workDir / 会话数据根 / 模型、回合入口 run() 与终局 stop()
//   AgentLoopRequest.cpp     一次 LLM 请求回合：压缩前导 → 流式请求 → 工具批推进 → 记忆沉淀链
//   AgentLoopTools.cpp       工具分发：executeTool / handler 表 / 统一收口 / 供友元复用的静态转发
//   AgentLoopBash.cpp        bash 异步执行链与后台任务结果收割
//   AgentLoopSubAgent.cpp    task 子代理的启动与统一收口
//   AgentLoopPermission.cpp  权限门：硬拒绝黑名单、询问规则、用户裁决续跑
//   AgentLoopFileTools.cpp   沙箱文件工具：read / write / edit / glob 与路径逃逸判定
//   AgentLoopSkills.cpp      技能扫描与 load_skill
//   AgentLoopTodo.cpp        todo_write 校验与渲染
//   AgentLoopCron.cpp        定时任务 handler 与空闲交付
//   AgentLoopHooks.cpp       生命周期钩子注册表（UserPromptSubmit / PreToolUse / PostToolUse / Stop）
//   AgentLoopHistory.cpp     history.json 落盘与恢复
//   AgentLoopPrompt.cpp      system prompt 组装与 25 工具 schema
//   AgentLoopTeam.cpp        s13 Agent Teams 宿主装配：引擎六注入、Lead 租约感知 cwd、
//                            团队事件收割注入、队友回合驱动（turnRequested→deliverTurnResult）
// 跨单元共享的内部工具集中在 AgentLoopInternal.h（非公开 API）。

#include "AgentLoop.h"

#include "SubAgent.h"
#include "ToolNames.h"
#include "AgentConstants.h"
#include "QOpenAi.h"

#include <QProcess>
#include <QDir>
#include <QTimer>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

AgentLoop::AgentLoop(const QString &sessionDataId, const QString &workDir, QObject *parent)
    : QObject(parent)
    // m_workDir 声明序先于 m_sessionDataId，故此处 init 也前置；务必先于下方 body 内
    // scanSkills()/初始 system prompt/m_cron.start()（装载 durable 台账）定值，令各数据根随所选目录解析。
    // 空则回落进程当前目录，语义与既有 setWorkDir 一致（归一化为绝对路径）。
    , m_workDir(workDir.isEmpty() ? QDir::currentPath() : QDir(workDir).absolutePath())
    , m_compact([this] { return sessionDataRoot(); }, [this] { return m_model; })
    , m_memory([this] { return sessionDataRoot(); }, [this] { return m_model; })
    , m_cron([this] { return sessionDataRoot(); })
    // s13 P3：补第二参 workDirSink——TaskStore::assignmentCwd ①号分支（Lead 无租约回落）
    // 取此值，令无租约时行为与 s13 前逐字一致（回落 m_workDir 而非会话根）
    , m_taskStore([this] { return sessionDataRoot(); }, [this] { return m_workDir; })
    , m_sessionDataId(sessionDataId)
    // s13 Agent Teams 引擎三件套（声明序 bus→worktrees→teams；构造仅需 &m_taskStore/&m_bus
    // 稳定地址，sink 均 [this] 惰性读取，见 AgentLoop.h 成员注释）
    , m_bus([this] { return sessionDataRoot(); })
    , m_worktrees(&m_taskStore, [this] { return sessionDataRoot(); }, [this] { return m_workDir; })
    , m_teams(&m_bus, &m_taskStore)
{
    // 模型 ID：优先环境变量 MODEL_ID，缺省回落 AgentConst::defaultModel()
    // （settings.ini 的 defaultModel 键，未配置则取生效清单首项；清单本身可由
    // modelOptions 键配置，单点取值见 AgentConstants.h）
    m_model = QString::fromUtf8(qgetenv("MODEL_ID"));
    if (m_model.isEmpty())
        m_model = AgentConst::defaultModel();

    // 内置生命周期钩子（对齐 lcc s04 模块尾部的 register_hook 清单）
    registerBuiltinHooks();

    // 压缩卡片出口（lcc s08 裁决 e：零新增公共信号）：复用四参 toolOutputReady，
    // toolName 固定 "compact"，summary 为档位描述，output 携带转写路径与前后估算
    m_compact.setCardSink([this](const QString &summary, const QString &output) {
        emit toolOutputReady(ToolNames::COMPACT, summary, output, !isToolFailure(output));
    });

    // 记忆卡片出口（lcc s09 裁决：复用四参 toolOutputReady，toolName 固定 "memory"，
    // 零新增公共信号；卡片文本 [memory] stored / [memory] consolidated 系列，lcc 终态 tag 化）
    m_memory.setCardSink([this](const QString &summary, const QString &output) {
        emit toolOutputReady(QStringLiteral("memory"), summary, output, !isToolFailure(output));
    });

    // 技能扫描（lcc s07）：构造时扫描一次 <m_workDir>/.lite-harness/skills/*/SKILL.md；
    // 修1 后目录数据不再进 system（见 makeContextInjection 注入块），仅供 load_skill 消费
    scanSkills();

    // 初始 system prompt（规格修1 静态化：仅工作目录 + 临时目录 + 编排规则 + 使用说明句/
    // 记忆声明静态段；记忆目录与召回改由 run() 续延构建的注入块随 payload 尾部下发）
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = QString();
    m_messages.append(systemMessage);
    rebuildSystemPromptMessage();

    // 定时任务运行时（lcc s12 start_runtime_threads 转译）：QTimer 1s 节拍替代 python daemon 线程
    //（登记偏差）；tick 恒跑、以 isRuntimeStarted 短路（setWorkDir 的 stop→start 期间不误轮询）。
    // 装载 durable 台账（此刻 workDir 已由构造入参定值，sessionDataRoot 随所选目录解析；
    // 不再依赖构造后 setWorkDir 的 stop→start 重载）
    m_cronTick = new QTimer(this);
    m_cronTick->setInterval(1000);
    connect(m_cronTick, &QTimer::timeout, this, [this] {
        if (!m_cron.isRuntimeStarted())
            return;
        m_cron.pollDueJobs(QDateTime::currentDateTime());
        // s13 wake 交付（Gate③ MAJOR-1，lcc loop.py:401-408 wake 分支转译）：置于
        // tryDeliverCron 之前对齐 lcc wait_for_cli_event 的 inbox→cron 优先级
        //（:340-351）——同栈直连若已开团队回合，m_running 置位令 tryDeliverCron
        // 卫兵自动跳过，到期任务待下个空闲 tick，与 lcc 单事件语义一致
        tryDeliverTeamEvents();
        tryDeliverCron();
    });
    m_cronTick->start();
    m_cron.start();

    // s13 Agent Teams 引擎装配（P3）：六注入一次性完成，先于任何回合/心跳/工具执行
    initTeamEngine();
}

void AgentLoop::setWorkDir(const QString &dir)
{
    // 空串忽略；归一化为绝对路径
    if (dir.isEmpty())
        return;
    m_workDir = QDir(dir).absolutePath();
    // 效率 P4：handler 表的文件四件套 lambda 按值固化了旧 workDir（见 mainToolHandlers
    // 头注释），换目录后必须清缓存，下一工具调用经 ensureToolHandlers() 以新目录重建
    m_toolHandlers.clear();
    // 修4：换工作目录 → system/会话根全变，token 锚自然失效（回退本地全量估算，
    // 直至下一次 usage 回读重锚）
    m_tokenAnchor = -1;

    // lcc s12：换工作目录重载 durable 台账（stop→start 复位启动标志后重读新目录的
    // scheduled_tasks.json；load 幂等去重——同 id 已登记则跳过，见 CronSchedulerManager 偏差注释）
    m_cron.stop();
    m_cron.start();

    // lcc s07 仅在启动时扫描一次；lite 有意超集：换工作目录时重扫技能并同步重建
    // system prompt（技能目录随工作目录走，避免陈旧清单误导模型）
    scanSkills();

    // 记忆目录（会话根/.memory/，见 sessionDataRoot）随注入的会话工作目录与 ID 解析，索引随新目录重读
    // （s07 重扫超集语义延伸至 s09）
    rebuildSystemPromptMessage();
}

QString AgentLoop::workDir() const
{
    return m_workDir;
}

void AgentLoop::setSessionDataId(const QString &id)
{
    // 仅供未走构造注入的扩展路径；须在首次落盘前调用（正常会话经 ChatSessionPage 构造注入）。
    // 不做 durable 重载：构造体内 m_cron.start() 已按当时会话根装载，中途改 ID 不重复装载（登记约束）。
    m_sessionDataId = id;
}

QString AgentLoop::sessionDataId() const
{
    return m_sessionDataId;
}

QString AgentLoop::sessionDataRoot() const
{
    // 回退分支（无 ID）保持改造前的全局行为：m_workDir/.lite-harness
    if (m_sessionDataId.isEmpty())
        return QDir(m_workDir).filePath(QStringLiteral(".lite-harness"));
    return QDir(m_workDir).filePath(QStringLiteral(".lite-harness/sessions/") + m_sessionDataId);
}

void AgentLoop::setModel(const QString &model)
{
    // 空串忽略；运行中改值不打断当前请求，下一轮请求自然生效
    if (model.isEmpty())
        return;
    m_model = model;
}

AgentLoop::~AgentLoop()
{
    // 与 stop() 相同但静默（不发信号）
    // lcc s06 R1 收口三路之一（析构）：先级联取消子代理——cancel 抑制其完成回调，
    // "(cancelled)" 回填进即将清空的历史属良性无害
    cancelSubAgent();

    // s13 P3 退出清算（lcc finally 的 lite 转译，登记偏差：不做 courteous shutdown 广播——
    // 析构后无事件循环可收应答；引擎 settleLedgers 保证账本收口）：必须在成员析构前于本
    // 体内完成——此刻 m_teams/m_bus/m_taskStore 仍存活，满足 TeammateRuntime FIND-L 契约
    settleTeamOnExit();

    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill();
    }
    m_activeProcesses.clear();

    // lcc s12：停表并复位运行时（QTimer 随本对象父子关系销毁，无需 delete）
    if (m_cronTick)
        m_cronTick->stop();
    m_cron.stop();

    if (m_currentStream)
    {
        m_currentStream->disconnect(this);
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }
    m_messages.clear();
}

void AgentLoop::run(const QString &userMessage)
{
    // 已有一个循环周期在运行则拒绝新消息
    if (m_running)
    {
        emit error(tr("Agent 仍在运行中，请等待完成后再发送。"));
        return;
    }

    setRunning(true); // 【红线】同步置位，先于本函数一切异步发起（见下方 P1 契约注释）
    m_toolIterations = 0;
    // 轮次上限入口快照（第十二轮）：每回合读一次设置值，本回合内所有判定与报错文案
    // 统一用 m_maxToolIterations——与压缩上限 prepareAsync 入口单取同型纪律，
    // 防回合进行中设置页改值造成同一回合前半/后半用不同上限的撕裂
    m_maxToolIterations = AgentConst::maxToolIterationsValue();
    // lcc s05：rounds_since_todo 为 loop() 的局部变量——每轮用户提问（run）从零起步
    m_roundsSinceTodo = 0;
    // lcc s08：compact_requested / reactive_retries 同为 loop() 局部——每轮用户提问归零；
    // active_request 记录本轮请求原文，供摘要消息 "Current user request" 字段使用
    m_compactRequested = false;
    m_reactiveRetries = 0;
    m_activeRequest = userMessage;

    // UserPromptSubmit 钩子（lcc s04）：用户消息入历史前触发
    // （内置 context_inject 打印当前工作目录；返回值在 lcc 中亦被忽略）
    triggerUserPromptSubmitHooks(userMessage);

    // 追加用户消息到会话历史
    QJsonObject userMessageObj;
    userMessageObj[QStringLiteral("role")] = QStringLiteral("user");
    userMessageObj[QStringLiteral("content")] = userMessage;
    m_messages.append(userMessageObj);

    // 后台任务收割注入（lcc s11 inject_background_results 挂载点一）：此刻末条即刚追加的
    // user 消息 → 通知并入其 content 尾部（lcc 末条 user 合并语义）；无通知不动作
    injectBackgroundResults();

    // s13 P3 团队事件收割注入（挂载点一，lcc inject_team_events 同型）：Lead 邮箱在
    // GUI 模型下的自然唤醒点即用户开新回合——事件并入刚追加的 user 消息尾部或新增
    // user 消息；空批不动作（lcc wait_for_cli_event stdin 唤醒轮的偏差登记见 AgentLoopTeam.cpp 头注释）
    injectTeamEvents();

    // 记忆召回（lcc s09 loop.py :71-72：每轮提问在 while 前 load_memories；规格修1 有意
    // 偏离：召回结果不再重建 system——快照进注入块 m_contextInjection，由 doStartChatRequest
    // 追加 payload 尾部，messages[0] 全会话字节恒定保前缀缓存；空存储时选择段短路，
    // 零 LLM 调用）。mid(1) 排除 system，与 lcc 会话主体语义对齐。异步化 P1（设计文档 §2.1）：
    // 召回链改走 AsyncRequest，下方续延是唯一发起路径；断网/超时/配置缺失在 MemoryManager
    // 内部统一降级为关键词兜底或空注入（§3.2 契约：done 恒收到可用文本），照常开聊、不抛不卡。
    // 【红线契约 §6-1】m_running=true 必须保持同步置于本函数任何异步发起之前（位置见
    // 函数头部，不得移动或延迟）：tryDeliverCron 同栈直连 scheduledUserMessage 后回读
    // m_running 判定接管成败，依赖 run() 返回前标志已置位——召回飞行中不算回合结束。
    // 召回飞行中 m_running 恒为 true，cron 的 !m_running 卫兵此窗口拒发交付（到期批次
    // 留队待下个空闲 tick 重投，at-least-once 语义不变——此为期望行为）。
    // 落槽纪律（第十一轮 F2，与本文件压缩挂接点同款条件落槽范式）：条件写入 `if (req)`——
    // 记忆为空/无查询文本时 loadMemoriesAsync 栈内同步调 done 并返回 nullptr，
    // 续延链可能在栈内经 startChatRequest→压缩触发把在途句柄写入 m_sideRequest，
    // 外层无条件落槽会用 nullptr 覆盖它，令 stop() 失去 cancel 手柄（F2 缺陷链）。
    // 挂起路径的 done 必在后续事件循环交付（AsyncRequest 永不回调同步触发），无竞态
    auto *req = m_memory.loadMemoriesAsync(
        m_messages.mid(1), this, [this](const QString &recalled) {
            // 卫兵（平移自同步时代"召回返回后复验 m_running"模式，§6-7）：stop() 已
            // cancel 本请求、done 正常永不触发，此处为防御复验，勿当作主防线删除
            if (!m_running)
                return;
            // 修1：召回结果快照进注入块（回合内字节恒定），不再重写 system [0]；
            // 目录读盘即时取（MEMORY.md 沉淀会全量重写索引，注入块每回合自然取新值）
            m_contextInjection = buildContextInjection(m_memory.readMemoryIndex(), recalled);
            // 快照历史并发起流式请求（事件驱动，不创建工作线程）
            QJsonArray messagesJson;
            for (const auto &msg : m_messages)
                messagesJson.append(msg);
            startChatRequest(messagesJson);
        });
    if (req)
        m_sideRequest = req;
}

void AgentLoop::stop()
{
    if (!m_running)
        return;

    // lcc s12：停止按钮不杀 cron 运行时——调度器存续仅令交付暂停于 m_running 卫兵，
    // 空闲后队列自动续投（若在此停掉调度，stop 一次即永久停摆，durable 任务跨会话失效）

    // lcc s06 R1：task 子代理在跑则先级联取消并合成 "(cancelled)" 配对回填。
    // 串行队列下"宿主待裁决"与"子代理运行中"互斥，随后的待决权限分支自然空转
    cancelSubAgent();

    // 待决权限询问：视为 deny，直接回填历史保持 tool_use/tool_result 配对完整
    //（OpenAI 协议要求每个 tool_call 必有对应 tool 消息；不经 onToolFinished 以免续跑队列或发展示信号）
    if (m_awaitingPermission)
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            m_pendingPermissionCall.value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("Permission denied");
        m_messages.append(toolResult);
        m_pendingPermissionCall = QJsonObject();
        m_awaitingPermission = false;
    }

    // 半途工具批收口（第六轮审计 C2）：批执行中停止时，已完成的工具结果还压在
    // m_toolResultsReady、未执行的调用还在 m_pendingToolCalls，而带 tool_calls 的
    // assistant 消息已入历史——不补齐则 tool 配对断裂，closeEvent 后的 persistHistory
    // 会把坏历史落盘、下一回合收到上游 400。三步：flush 已完成结果 → 为剩余每条调用
    // 合成 "(stopped)" 结果（与 cancelSubAgent 的 "(cancelled)" 同纪律，直写历史、不经
    // onToolFinished——不发展示信号不续跑队列）→ 清空两队列。
    // 置于上两步之后：子代理/待决权限各自负责在途调用的收口，本段只兜"已完成未回填 +
    // 未开始"两类队列态；若 cancelSubAgent 已清空队列，本段自然空转
    for (const auto &value : m_toolResultsReady)
        m_messages.append(value.toObject());
    m_toolResultsReady = QJsonArray();
    for (const auto &value : m_pendingToolCalls)
    {
        QJsonObject toolResult;
        toolResult[QStringLiteral("role")] = QStringLiteral("tool");
        toolResult[QStringLiteral("tool_call_id")] =
            value.toObject().value(QStringLiteral("id")).toString();
        toolResult[QStringLiteral("content")] = QStringLiteral("(stopped)");
        m_messages.append(toolResult);
    }
    m_pendingToolCalls = QJsonArray();

    // 中断正在执行的 QProcess（其 finished 后 onToolFinished 因 m_running=false 不再继续）
    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill();
    }
    m_activeProcesses.clear();

    // 主动取消当前流（cancel 内部设 done=true，后续信号不再处理）
    if (m_currentStream)
    {
        m_currentStream->cancel();
        m_currentStream->disconnect(this);
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }

    // 异步化 P1/P3（设计文档 §3.4/§6-7）：取消在途侧链请求（召回（P1）与三条压缩链——
    // prepare 全量段/批尾主动压缩/反应式压缩（P3）共用本槽，同槽异构故用 qobject_cast 而非
    // static_cast，转型空即无请求在途跳过）——cancel 后 done 永久静默：召回续延不再执行、
    // 压缩续延不回写历史/不落盘/不重发（P3 验证点：压缩中停止 → 历史不被替换）；
    // 请求已终态（回调已交付、deleteLater 未及处理）时 cancel 经 m_done 门闩天然无操作
    if (auto *side = qobject_cast<QOpenAi::AsyncRequest*>(m_sideRequest.data()))
        side->cancel();
    m_sideRequest = nullptr;

    // 记忆沉淀链不随 stop 取消（异步化 P2，设计文档 §6-7 语义分裂决策）：前链（召回）
    // 是"本回合还没开始"的门槛，停则回合作废；记忆链是 finished 之后的 fire-and-forget
    // 尾巴，与用户停止意图无关，且 consolidate 的快照-删-写段跨 LLM 等待之后仍需完整
    // 执行——cancel 落在写段之前只会白丢沉淀成果（尽力而为语义下无补偿路径）。故本函数
    // 对 m_memoryRequest 既不 cancel 也不清空（链在途则由 done/析构自行收口）

    // lcc 31a99d1：用户停止 = 回合失败终局——在途 cron 批回队待下个空闲 tick 重投
    m_cron.finalizeInFlightDelivery(false);

    setRunning(false);
    persistHistory(); // 用户停止终局落盘（已累积历史不丢）
    emit error(tr("已停止。"));
}

// ---- 上下文 token 计量（规格修4：usage 锚定 + 增量估算，UI 与压缩触发共用单源）----

// 采纳一次 usage.prompt_tokens 为锚（契约 C-2：ChatStream 成功路径、messageFinished 之前发）。
// prompt_tokens 缺失/非正（部分兼容端点不回 usage）一律忽略，保持旧锚或本地兜底路径。
// 锚口径 = 发送点 payload 全量（system + 历史 + tools schema + 注入块）的服务端真值；
// 故 anchorCount/anchorInjection 均取发送点快照，与当前 m_messages/m_contextInjection 对齐。
void AgentLoop::adoptUsageAnchor(const QJsonObject &usage)
{
    const qint64 promptTokens = qint64(usage.value(QStringLiteral("prompt_tokens")).toDouble());
    if (promptTokens <= 0)
        return;
    m_tokenAnchor = promptTokens;
    m_tokenAnchorCount = m_lastSendHistoryCount;
    m_anchorInjectionTokens = m_lastSendInjectionTokens;
    m_historyRewrittenSinceAnchor = false;
}

// 当前上下文 token 估算（锚定 + 增量外推）：
// - 锚有效（已采纳 usage、锚后未压缩/恢复改写历史、锚计数不超过当前条数）：
//   锚真值 + 其后新增消息逐条估算 + 注入块变化量（当前注入 - 锚时注入，契约 C-6）。
//   注意锚路径不再计 overhead——prompt_tokens 已含 system/tools/注入的发送点真值；
//   增量与锚值同为 token 口径（AgentConst::estimateTokens），混合口径误差有界，
//   下一次 usage 回读自然自校正（§9.7 已知项：todo reminder/后台注入尾拼接不计入，轻微低估有界）。
// - 锚失效（首回合未回读/压缩/恢复/setWorkDir/换目录）：本地全量估算 =
//   CompactManager::estimateTokens(m_messages)（含 system[0]）+ tools schema + 注入块。
//   此为修 B4 的关键：旧字符口径触发漏计 system 与 18 工具 schema，本兜底一并计入。
qsizetype AgentLoop::estimatedContextTokens() const
{
    const qsizetype injectionTokens = AgentConst::estimateTokens(m_contextInjection);
    if (m_tokenAnchor > 0 && !m_historyRewrittenSinceAnchor
        && m_tokenAnchorCount <= m_messages.size()) {
        qsizetype delta = 0;
        for (qsizetype i = m_tokenAnchorCount; i < m_messages.size(); ++i) {
            delta += AgentConst::estimateTokens(QString::fromUtf8(
                QJsonDocument(m_messages.at(i)).toJson(QJsonDocument::Compact)));
        }
        return qMax<qsizetype>(0, static_cast<qsizetype>(m_tokenAnchor) + delta
                                    + injectionTokens - m_anchorInjectionTokens);
    }
    if (m_toolsSchemaTokens == 0) {
        m_toolsSchemaTokens = AgentConst::estimateTokens(QString::fromUtf8(
            QJsonDocument(createToolsDefinition()).toJson(QJsonDocument::Compact)));
    }
    return CompactManager::estimateTokens(m_messages) + m_toolsSchemaTokens + injectionTokens;
}

// 非会话开销（prepareAsync 拆分 conversationTokens 用，契约 C-1/C-6）：system[0] + tools
// schema + 注入块。注意与 estimatedContextTokens 的锚路径混用时仅用于减法拆口径，
// 误差同上（有界、下次 usage 自校正）。
qsizetype AgentLoop::contextOverheadTokens() const
{
    qsizetype tokens = 0;
    if (!m_messages.isEmpty()) {
        tokens += AgentConst::estimateTokens(QString::fromUtf8(
            QJsonDocument(m_messages.at(0)).toJson(QJsonDocument::Compact)));
    }
    if (m_toolsSchemaTokens == 0) {
        m_toolsSchemaTokens = AgentConst::estimateTokens(QString::fromUtf8(
            QJsonDocument(createToolsDefinition()).toJson(QJsonDocument::Compact)));
    }
    tokens += m_toolsSchemaTokens;
    tokens += AgentConst::estimateTokens(m_contextInjection);
    return tokens;
}

