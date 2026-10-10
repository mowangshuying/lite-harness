// 回合请求链：压缩前导（五级管线异步挂接）→ 流式请求 → 工具批推进 → 终局与记忆沉淀链。
// 与 AgentLoop.cpp 的 run()/stop() 共同构成一次完整回合；工具执行本体见 AgentLoopTools.cpp。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "SubAgent.h"
#include "AgentConstants.h"
#include "QOpenAi.h"

#include <QTimer>
#include <QJsonArray>
#include <functional>

// 上下文超限错误判定单源（规格修3 §5.4）：小写包含式匹配主流 OpenAI 兼容端点的溢出
// 文案族（含 4xx 响应体透传后的 message 文本）；命中即触发反应式压缩（预算 1 次不变）。
// 归属本文件的原因：主循环反应式压缩是唯一消费方（s13 队友侧经 AgentLoopInternal.h
// 声明共享同一判定，预算在队友自身运行时独立持有）。
namespace AgentLoopDetail {

bool isContextOverflowError(const QString &msg)
{
    static const char *kPatterns[] = {
        "prompt_too_long", "too many tokens", "context_length_exceeded",
        "maximum context length", "context length", "input is too long",
        "range of input length",
    };
    const QString lowered = msg.toLower();
    for (const char *p : kPatterns) {
        if (lowered.contains(QLatin1String(p)))
            return true;
    }
    return false;
}

} // namespace AgentLoopDetail

void AgentLoop::startChatRequest(const QJsonArray &messages)
{
    // 发送前压缩挂接（lcc s08 prepare：位于 lcc 主循环 while 顶部，即每次发起请求之前）。
    // P3 异步化：前四段本地管线仍同步跑，仅触发全量压缩时挂侧链；续延以最终快照
    // 交付 doStartChatRequest（未改写则原样透传调用方快照）——本地早退路径下
    // next 在本函数栈内同步执行，与挂起路径统一为"回调续延"单一时序模型
    applyCompactPipelineAsync(messages,
                              [this](const QJsonArray &requestMessages) {
                                  doStartChatRequest(requestMessages);
                              });
}

// 真实发起段：原 startChatRequest 主体逐字平移（组请求体、createStream、三信号接线）。
// 顶部 !m_running 为防御卫兵：异步链的正常路径已由续延回调首行卫兵把关
// （原"嵌套事件循环豁免（裁决 f）——阻塞摘要返回后用户已停止则放弃发送"的
// 检查点随压缩挂起而失去语义，卫兵保留仅作纵深防御）
void AgentLoop::doStartChatRequest(const QJsonArray &requestMessages)
{
    if (!m_running)
        return;

    QJsonObject request;
    request[QStringLiteral("model")] = m_model;
    // 修1（D1）：记忆注入块作为独立 user 消息追加在 payload 尾部——不落 m_messages/
    // history.json（历史与 UI 零污染、会话恢复无残留），跨回合的分歧点恒在序列末端，
    // messages[0..n-1] 前缀字节稳定 → OpenAI 兼容端点的前缀缓存全保
    QJsonArray payload = requestMessages;
    if (!m_contextInjection.isEmpty()) {
        QJsonObject injection;
        injection[QStringLiteral("role")] = QStringLiteral("user");
        injection[QStringLiteral("content")] = m_contextInjection;
        payload.append(injection);
    }
    request[QStringLiteral("messages")] = payload;
    request[QStringLiteral("tools")] = createToolsDefinition();
    // 默认开启思考
    request[QStringLiteral("enable_thinking")] = true;
    // 推理强度档位：对齐 opencode xhigh 观感（长思考、少工具轮）；端点不认则回退服务端默认
    request[QStringLiteral("reasoning_effort")] = AgentConst::kReasoningEffort;
    // 采样参数：对齐 opencode（低温决策更稳）；端点不认则回退服务端默认
    request[QStringLiteral("temperature")] = AgentConst::kTemperature;
    request[QStringLiteral("top_p")] = AgentConst::kTopP;
    // 输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致，取自单源常量）
    request[QStringLiteral("max_tokens")] = AgentConst::kMaxTokens;
    // 修3：流式回读 usage（OpenAI 兼容扩展，末帧携带 prompt_tokens 等）——仅主循环注入；
    // 侧链压缩与 SubAgent 请求不带此键，token 锚只认真值来源（端点不认则静默忽略，走本地兜底）
    request[QStringLiteral("stream_options")] =
        QJsonObject{{QStringLiteral("include_usage"), true}};
    // stream 由 QOpenAi 内部按流式发送，无需在此显式指定

    // 修4 发送点快照：usage 回读到达时以「当时发出的历史条数 + 注入块估算」落锚，
    // 与 prompt_tokens 口径严格对齐（requestMessages 即由 m_messages 快照而来，同口径）
    m_lastSendHistoryCount = m_messages.size();
    m_lastSendInjectionTokens = AgentConst::estimateTokens(m_contextInjection);

    QOpenAi::ChatStream *s = QOpenAi::chat().createStream(request, this);
    m_currentStream = s;

    // 增量转发（this 上下文：AgentLoop 销毁自动断连，s 为 this 子对象自动释放）
    connect(s, &QOpenAi::ChatStream::thinkingDelta, this, &AgentLoop::thinkingDelta);
    connect(s, &QOpenAi::ChatStream::textDelta, this, &AgentLoop::textDelta);
    // 修3/修4：usage 末帧回读 → token 锚（契约 C-2：成功路径、messageFinished 之前发）
    connect(s, &QOpenAi::ChatStream::usageReceived, this,
            [this](const QJsonObject &usage) { adoptUsageAnchor(usage); });

    connect(s, &QOpenAi::ChatStream::messageFinished, this, [this, s](const QJsonObject &fullMsg) {
        // 卫兵（第六轮审计 C1 纵深防御）：坏 JSON 帧 error 后流已成终局，但 error 回调
        // 链可能已在栈内完成回合终局（setRunning(false)），此刻残留帧再触发本信号则
        // 必须弃收——否则向已终局的历史追加孤儿 tool_calls 消息，下一回合上游 400。
        // s 的清理由 error 回调/stop() 负责，本分支不重复处置
        if (!m_running)
            return;
        m_currentStream = nullptr;
        s->deleteLater();
        // lcc s08：成功收到响应即视为上下文已可容纳，反应式重试预算复位（loop.py create 后 :50）
        m_reactiveRetries = 0;

        // 无工具调用 -> 最终回复，循环结束
        const QJsonArray toolCalls = fullMsg.value(QStringLiteral("tool_calls")).toArray();
        if (toolCalls.isEmpty())
        {
            m_messages.append(fullMsg);

            // Stop 钩子（lcc s04 引入，s06 起为"续跑"语义）：返回非空则作为一条 user
            // 消息注入历史并发起新一轮请求（消耗 m_toolIterations，m_maxToolIterations
            // 回合快照兜底，不加额外计数上限）。内置 summary 钩子恒返回空串，故默认行为与 s04 一致
            // 直接收尾。lcc 原文误拼 "conent"，此处按正确键名 "content" 写入
            const QString force = triggerStopHooks();
            if (!force.isEmpty())
            {
                QJsonObject injected;
                injected[QStringLiteral("role")] = QStringLiteral("user");
                injected[QStringLiteral("content")] = force;
                m_messages.append(injected);

                if (++m_toolIterations > m_maxToolIterations)
                {
                    // s13：lcc finally 覆盖异常分支——撞上限终局同样退租（只释放，
                    // 不注入事件：注入窗口已过，欠账留待下一自然回合收割）
                    settleLeadLease();
                    // lcc 31a99d1：轮次上限失败终局——在途 cron 批回队
                    m_cron.finalizeInFlightDelivery(false);
                    setRunning(false);
                    persistHistory(); // 轮次上限失败终局也落盘（已累积历史不丢）
                    emit error(tr("工具调用轮次超过上限（%1 轮），终止循环。").arg(m_maxToolIterations));
                    return;
                }
                const QJsonArray messagesJson = AgentLoopDetail::snapshotMessages(m_messages);
                startChatRequest(messagesJson);
                return;
            }

            // 回合成功终局（异步化 P2 重排，设计文档 §2.2/§6-10）：旧序为
            // memoryPhaseStarted → [extract 阻塞≤120s → consolidate 阻塞≤120s] →（stop
            // 竞态则 return 吞掉 finished）→ finalize → m_running=false → persistHistory →
            // finished；新序把记忆沉淀从终局关键路径摘除——finalize → setRunning(false) →
            // persistHistory → finished → memoryPhaseStarted → 异步链尾巴。
            // persistHistory 先于记忆链是 §6-10 有意决策：history.json 与 .memory/ 为独立
            // 文件域，中途崩溃最坏丢本轮记忆沉淀、不丢历史（旧序同样存在该窗口的更差形态：
            // 阻塞期间崩溃则历史与 finished 一起丢）。finished 不再被 2×120s 阻塞窗口延迟，
            // 旧代码"阻塞后复验 m_running 不通过则 return"的吞 finished 竞态随之消亡。
            // Stop 钩子在上方 force 分支触发并已 return，位置与语义与旧版逐字不变。
            // s13 Agent Teams（P3 挂载三）：lcc agent_loop finally 的
            // release_completed_assignment("agent") 与 inject_team_events、
            // check_team_offline_edge 三合一，置于终局序列最前——注入的 [Team events]
            // user 消息随下方 persistHistory 一并落盘（lcc loop.py :168-172 finally +
            // :419 每轮圈尾核对）
            leadTurnEndSettlement();
            // lcc 31a99d1：回合成功终局——确认在途 cron 批（at-least-once 收口）
            m_cron.finalizeInFlightDelivery(true);
            setRunning(false);
            persistHistory(); // 回合终局落盘（emit 前，确保 UI 侧后续动作可见）
            emit finished(fullMsg.value(QStringLiteral("content")).toString());

            // 记忆沉淀（lcc s09 loop.py :113-117：仅自然结束分支触发——force 续跑分支与撞
            // m_maxToolIterations 上限分支均不提取，lcc 语义不修正；轮次上限/流错误/stop
            // 三类终局同样不触发，与旧版一致）。mid(1) 排除 system 与 lcc 会话主体对齐。
            // memoryPhaseStarted 与 finished 同栈紧随：UI 据 §3.5a 保留气泡占位、把正文
            // 就地定稿并挂记忆进度 live 卡；随后异步链启动（fire-and-forget，结果卡经
            // 既有 toolOutputReady("memory") 通道落位，链尾不阻塞本回调返回）
            emit memoryPhaseStarted();
            startMemoryChain();
            return;
        }

        // 有工具调用 -> 防死循环计数（上限取回合快照，见 run() 入口注释）
        if (++m_toolIterations > m_maxToolIterations)
        {
            // s13：lcc finally 覆盖异常分支——撞上限终局同样退租
            settleLeadLease();
            // lcc 31a99d1：轮次上限失败终局——在途 cron 批回队
            m_cron.finalizeInFlightDelivery(false);
            setRunning(false);
            persistHistory(); // 轮次上限失败终局也落盘
            emit error(tr("工具调用轮次超过上限（%1 轮），终止循环。").arg(m_maxToolIterations));
            return;
        }

        continueWithToolResults(fullMsg);
    });

    connect(s, &QOpenAi::ChatStream::error, this, [this, s](const QString &msg) {
        m_currentStream = nullptr;
        s->deleteLater();
        // lcc s06 R1 收口三路之一：错误链同样级联取消子代理并合成 "(cancelled)" 回填
        // （串行队列下宿主流错误与子代理运行实际互斥，此处为防御性接线）
        cancelSubAgent();
        // 反应式压缩（lcc s08）：上下文超限且重试预算未用尽 → 压缩历史后重发请求，
        // 否则落入原错误路径终止。修3 后 4xx 响应体已并入 error 文本，关键词表扩至
        // isContextOverflowError 单源；MAX_REACTIVE_RETRIES=1 为每轮用户提问的局部预算（run 归零）
        if (AgentLoopDetail::isContextOverflowError(msg) && m_reactiveRetries < 1)
        {
            // 重试预算在发起前消费（原同步段 ++ 位置不动，防重试风暴）；空对话短路时
            // 预算同样被消费——与原同步链行为一致（reactiveCompact 空对话原样返回后重发）
            ++m_reactiveRetries;
            // P3 异步化：摘要侧链挂起，本函数立即返回（cancelSubAgent 保持在发起压缩
            // 之前——原相对顺序不动）；续延交付后回写并重发。挂起窗口 m_running 恒 true
            // （cron 拒投门槛持续成立，与召回在途契约一致，§6-9）；此间 stop → done
            // 被 m_sideRequest cancel 永久静默 → 不重发、历史不被压缩替换（收尾由
            // stop 自身完成），回调首行卫兵仅作防御复验
            auto *req = m_compact.reactiveCompactAsync(
                m_messages.mid(1), m_activeRequest, tr("反应式压缩（上下文超限）"), this,
                [this](const QVector<QJsonObject> &replaced) {
                    if (!m_running)
                        return;
                    applyCompressedConversation(replaced);
                    const QJsonArray retryMessages = AgentLoopDetail::snapshotMessages(m_messages);
                    startChatRequest(retryMessages);
                });
            if (req)
                m_sideRequest = req;
            return;
        }
        // s13：流错误失败终局同样退租（反应式压缩重发分支非终局，上方已 return 不处理）
        settleLeadLease();
        // lcc 31a99d1：流错误失败终局——在途 cron 批回队（反应式压缩重发分支非终局，不处理）
        m_cron.finalizeInFlightDelivery(false);
        setRunning(false);
        persistHistory(); // 流错误终局落盘
        emit error(msg);
    });
}

// 五级压缩异步挂接点（P3，设计文档 §2.3；lcc s08 prepare：对不含 system 的会话主体做五级
// 压缩——lcc 的 system 随每次请求单独下发、不在 messages 估算窗口内，此处以 mid(1) 对齐）：
// 判定逻辑沿用迁移前同步实现——本地四段在
// CompactManager::prepareAsync 内同步跑，仅全量压缩（含摘要 LLM 调用）挂起。
// 压缩中 stop → stop 的 m_sideRequest cancel 使 done 永久静默 → next 不执行、
// 历史不被替换、不落盘（P3 验证点，对应原同步链的 stop 卫兵"阻塞摘要返回后不回了就不改写"）。
// 落槽纪律：条件写入 `if (req)`——本地早退路径 done 在 prepareAsync 返回前已同步交付、
// 回调链可能在栈内继续发起新侧链写槽，外层无条件清空会覆盖新句柄；挂起路径的 done
// 必在后续事件循环交付（AsyncRequest 永不回调同步触发），与栈尾落槽无竞态。
// §6-9 并发论证：本挂接点仅由 startChatRequest 驱动（单一前链），批尾与召回续延
// 各自串成一条链，同一时刻至多一条侧链在途——与召回共用 m_sideRequest 槽安全
void AgentLoop::applyCompactPipelineAsync(const QJsonArray &callerMessages,
                                          std::function<void(const QJsonArray &requestMessages)> next)
{
    if (m_messages.isEmpty())
    {
        if (next)
            next(callerMessages);
        return;
    }
    // 修4（契约 C-1/C-6）：token 口径拆分——conversationTokens 只含会话主体（mid(1)），
    // 判定总额 = 锚定/本地全量估算（estimatedContextTokens 含 system[0]+tools+注入，
    // 修 B4 漏计）；overhead = system+tools+注入，差值交 prepareAsync 与 token 预算比较
    const qsizetype overheadTokens = contextOverheadTokens();
    const qsizetype totalTokens = estimatedContextTokens();
    const qsizetype conversationTokens = totalTokens > overheadTokens ? totalTokens - overheadTokens : 0;
    auto *req = m_compact.prepareAsync(
        m_messages.mid(1), conversationTokens, overheadTokens, m_activeRequest,
        tr("自动压缩（上下文超限）"), this,
        [this, callerMessages, next](bool changed, const QVector<QJsonObject> &conversation) {
            // 续延卫兵（同 P1 召回范式）：stop 后不回写历史、不发请求
            if (!m_running)
                return;
            QJsonArray requestMessages = callerMessages;
            if (changed)
            {
                applyCompressedConversation(conversation);
                requestMessages = AgentLoopDetail::snapshotMessages(m_messages);
            }
            if (next)
                next(requestMessages);
        });
    if (req)
        m_sideRequest = req;
}

// 压缩结果回写（裁决 g）：保留 m_messages[0] system，会话主体整体替换
// （lcc compact_history/reactive 的替换含 system —— lite 保留 system 前缀，因我们的
// system 始终驻留历史首位且随每次请求快照下发）
void AgentLoop::applyCompressedConversation(const QVector<QJsonObject> &conversation)
{
    if (m_messages.isEmpty())
        return;
    const QJsonObject systemMessage = m_messages.first();
    m_messages.clear();
    m_messages.append(systemMessage);
    m_messages.append(conversation);
    // 修4：压缩整体改写历史 → prompt_tokens 锚作废（回退本地全量估算，直至下次 usage 重锚）
    m_tokenAnchor = -1;
    m_historyRewrittenSinceAnchor = true;
}

void AgentLoop::setRunning(bool running)
{
    // m_running 单点收口（异步化 P2，设计文档 §3.4）：翻转与 runningChanged 发射同点，
    // 防六处终局漏发。同值不发射（run() 卫兵拒绝/重复 stop 等路径不打扰订阅者）。
    // 红线纪律在调用点：run() 的 setRunning(true) 必须同步先于一切异步发起
    //（tryDeliverCron 同栈回读 m_running 的 lcc s12 R3 契约）；信号经直连在调用栈内
    // 送达 UI，订阅者不得借 runningChanged(false) 栈内启动新回合（此刻 cron finalize/
    // persistHistory 可能尚未完成）。本函数不驱动 cron：tryDeliverCron 回读的是成员值，
    // 与信号发射时序无关
    if (m_running == running)
        return;
    m_running = running;
    emit runningChanged(running);
}

void AgentLoop::startMemoryChain()
{
    // 记忆沉淀链（异步化 P2，设计文档 §2.2）：extract →（stored>=1 时）consolidate →
    // finishMemoryChain，同会话严格串行——消除 extract 追加写与 consolidate 全库重写
    // 跨 LLM 等待的交错窗口（旧同步链天然串行，异步化后必须以链保序）；跨会话各有
    // 独立 AgentLoop 与 .memory/ 根，天然隔离。
    // 与新一轮召回（m_sideRequest）并发可接受（§2.2 末段论证）：召回对 .memory/ 只读，
    // extract 只追加新文件、consolidate 的重写段无 await 点，最坏窗口是召回读到整理前
    // 版本（陈旧容忍，无数据破坏），与旧阻塞时代"上一轮 consolidate 前开始的本轮召回"
    // 同面。
    if (m_memoryChainActive)
    {
        m_memoryChainPending = true; // 单槽补跑（丢弃/续跑条件见成员注释）
        return;
    }
    m_memoryChainActive = true;

    // ctx=this 生命周期锚（§6-3）：宿主析构则链随父子关系作废、done 永久静默，与
    // stop() 不 cancel 本链的 fire-and-forget 语义并存（§6-7，理由见 stop() 注释）。
    // m_memoryRequest 仅作句柄记账与析构期作废锚点
    m_memoryRequest = m_memory.extractMemoriesAsync(
        m_messages.mid(1), this, [this](int stored) {
            // 有新增才合并（lcc s09 语义；consolidate 自带阈值护栏，stored==0 时显式
            // 短路省一次全目录扫描）。复验纪律不适用本链：链属后台尾巴，跨新回合照常
            // 跑完——新回合的沉淀请求已按 active 折叠进 pending，串行保证不破
            if (stored >= 1)
            {
                m_memoryRequest = m_memory.consolidateMemoriesAsync(
                    this, [this](int /*consolidated*/) { finishMemoryChain(); });
                return;
            }
            finishMemoryChain();
        });
}

void AgentLoop::finishMemoryChain()
{
    m_memoryChainActive = false;
    m_memoryRequest = nullptr; // 在途对象由 AsyncRequest 终态自 deleteLater，此处仅收句柄
    emit memoryChainFinished();

    if (!m_memoryChainPending)
        return;
    m_memoryChainPending = false;
    if (m_running)
        return; // 新回合在飞：丢弃补跑（尽力而为）——该回合终局自然再启整链，对话主体
                // 已覆盖旧 pending 的沉淀来源；m_running 恒 false 的 stop()/错误终局
                // 不重启链，属 lcc"仅自然结束分支沉淀"语义的既定豁免
    // 排程续跑而非直调：防 memoryChainFinished 订阅者栈内重入，也防同步短路链
    //（done 同步交付）在本回调栈内递归
    QTimer::singleShot(0, this, [this]() { startMemoryChain(); });
}

void AgentLoop::continueWithToolResults(const QJsonObject &assistantMessage)
{
    // 追加带 tool_calls 的完整 assistant 消息
    m_messages.append(assistantMessage);
    m_pendingToolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    m_toolResultsReady = QJsonArray();
    // lcc s05：used_todo 为每批 tool_calls 的轮内标志，新批次开始归零
    m_usedTodoThisRound = false;

    runNextTool();
}

void AgentLoop::runNextTool()
{
    if (!m_running)
        return;

    if (m_pendingToolCalls.isEmpty())
    {
        // 全部工具执行完成：回填结果到历史并再次请求
        for (const auto &value : m_toolResultsReady)
            m_messages.append(value.toObject());
        m_toolResultsReady = QJsonArray();

        // 后台任务收割注入（lcc s11 inject_background_results 挂载点二，对应 lcc while 顶部、
        // compact 之前）：此刻末条为 tool 角色 → 新增独立 user 消息携带通知；无通知不动作。
        // 置于 todo 提醒之前不影响其向后查找末条 tool 消息（新增 user 消息会被跳过）
        injectBackgroundResults();

        // s13 Agent Teams（P3 挂载点二）：批尾收割投递 lead 邮箱团队事件（lcc 圈首
        // inject_team_events 的 GUI 转译：批尾=重发请求前最后一刻，队友 result/停机
        // 应答在此对 Lead 显影）
        injectTeamEvents();

        // 待办提醒（lcc s05 引入，s06 改并入形态）：本批 tool_calls 未"执行"todo_write 则
        // 计数 +1，执行过则归零。与 lcc 一致：权限门拦截/用户拒绝的 todo_write 不算执行
        // （handler 未跑）；走到 handler 的即使返回校验错误也算执行过。
        // lcc s06 把提醒文本并入最后一条 tool_result 的 content 尾部（OpenAI 协议 tool 消息
        // content 为扁平字符串，直接字符串拼接；提醒内文逐字一致，不再发独立 user 消息）
        if (m_usedTodoThisRound)
            m_roundsSinceTodo = 0;
        else
            ++m_roundsSinceTodo;
        if (m_roundsSinceTodo >= 3)
        {
            const QString reminder =
                QStringLiteral("\n\n<reminder>Update your todos.</reminder>");
            for (int i = m_messages.size() - 1; i >= 0; --i)
            {
                if (m_messages.at(i).value(QStringLiteral("role")).toString()
                    == QStringLiteral("tool"))
                {
                    QJsonObject &tail = m_messages[i];
                    tail[QStringLiteral("content")] =
                        tail.value(QStringLiteral("content")).toString() + reminder;
                    break;
                }
            }
            m_roundsSinceTodo = 0;
        }

        // 批尾 compact 替换（lcc s08 loop.py：结果批 append 进历史后，若 compact_requested 则
        // compact_history 替换整个历史——顺序红线 reminder→results 追加→压缩替换，不可交换；
        // 被替换的历史不再含 compact 的 tool_calls，OpenAI 配对因此保持完整）。
        // P3 异步化：摘要段挂起为侧链，压缩之后的落盘检查点/快照/重发全部搬进续延，
        // 相对顺序逐字不变；results 回填与 reminder 均已在同步段完成（红线起点不动）。
        // 挂起窗口 m_running 恒 true：run() 重入被 :555 卫兵拒绝、工具链由 runNextTool
        // 单一驱动 → 同一时刻至多一条前链在途，与召回/反应式压缩共用 m_sideRequest 槽
        // 安全（§6-9 并发论证）。此间 stop → done 被 cancel 永久静默 → persistHistory
        // 不执行、历史不被替换，停在上一检查点（原同步链卫兵"阻塞摘要返回后不回了
        // 就不改写"同款语义），回调首行卫兵仅作防御复验
        if (m_compactRequested)
        {
            m_compactRequested = false;
            auto *req = m_compact.compactHistoryAsync(
                m_messages.mid(1), m_activeRequest, tr("主动压缩（compact 工具）"), this,
                [this](const QVector<QJsonObject> &replaced) {
                    if (!m_running)
                        return;
                    applyCompressedConversation(replaced);
                    // 批尾落盘检查点（随续延平移）：注入与压缩后的最终状态被捕获
                    persistHistory();
                    const QJsonArray messagesJson = AgentLoopDetail::snapshotMessages(m_messages);
                    startChatRequest(messagesJson);
                });
            if (req)
                m_sideRequest = req;
            return;
        }

        // 批尾落盘检查点：结果注入 / 后台收割等本批变更均已生效，此刻历史是一个
        // 完整合法的配对状态，长工具轮中途崩溃也能恢复到此处（放在 startChatRequest 前而非
        // m_toolResultsReady 清空处；触发压缩时本检查点随续延在压缩替换后执行，见上）
        persistHistory();

        const QJsonArray messagesJson = AgentLoopDetail::snapshotMessages(m_messages);
        startChatRequest(messagesJson);
        return;
    }

    const QJsonObject toolCall = m_pendingToolCalls.takeAt(0).toObject();
    // 效率 P4：传缓存表（ensureToolHandlers 首用构建/setWorkDir 失效），替代旧每工具调用整表重建
    executeTool(toolCall, ensureToolHandlers(), /*permissionGranted=*/false);
}
