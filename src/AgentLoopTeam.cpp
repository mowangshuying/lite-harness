// s13 Agent Teams 宿主装配（P3b）单源 TU：AgentLoop 的 8 个团队成员函数实现
// （声明见 AgentLoop.h「---- s13 Agent Teams ----」区）+ 队友侧 10 工具 schema 工厂。
// 定位：引擎五注入 + launcher 契约 + Lead 租约感知 cwd + 团队事件收割/回合终局清算。
// lcc 对应：loop.py 的 daemon 线程 consume 循环在此转译为主线程事件驱动
// （turnRequested→ChatStream→deliverTurnResult），零嵌套事件循环、零线程（AGENTS 铁律）。

#include "AgentLoop.h"

#include "AgentLoopTeam.h"
#include "AgentLoopInternal.h"
#include "ToolNames.h"
#include "BashRunner.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

// ═══════════════════════════════════════════════════════════════
// 引擎装配（构造体末尾 initTeamEngine，声明/挂载点见 AgentLoop.h/.cpp）
// ═══════════════════════════════════════════════════════════════

void AgentLoop::initTeamEngine()
{
    // 注入①（挂载十四同源）：TaskStore 的 worktree 解析回调——任务快照带绑定名时
    // 经 WorktreeManager 注册表现算 cwd；破损绑定折叠为不可解（fail-closed 文案
    // 单源于 WorktreeManager::resolveWorktreeCwd）。未绑定任务根本不进本回调
    //（TaskStore resolveTaskCwd 早退语义，见 TaskStore.h:193-197 注释）。
    m_taskStore.setCwdResolver([this](const TaskStore::TaskSnapshot &task, QString *error) {
        return m_worktrees.resolveWorktreeCwd(task, error);
    });

    // 注入②（挂载十五，D8 偏差登记）：lcc 的 create_worktree 异步壳（重入 QSet 防抖）
    // → lite 直用 WorktreeManager 同步内核：git 子进程在本调用栈阻塞至多数秒，
    // 与主循环其它同步 handler（bash 危险检查等）同型，P2a 已裁决内核同步性。
    m_teams.setWorktreeCreator([this](const QString &name, const QString &taskId) {
        return m_worktrees.createWorktree(name, taskId);
    });

    // 注入③（launcher 契约钉死项，§4.2/FIND-E/FIND-L）：先建后启——
    // new（parent=nullptr：FIND-L 要求 runtime 先死于 manager/bus/store 三个按值成员，
    // 挂 this 父子树会反序违例）→ 登记宿主所有权表 → 接两条回合信号 →
    // QTimer::singleShot(0) 把 start() 推迟到本调用栈返回后（runSpawnTeammate
    // 先完成句柄登记，对齐 lcc :677 start 前注册线程的「秒死也能找到自己」语义）。
    // 恒返回有效句柄，绝不返 nullptr（FIND-E：nullptr=创建失败=引擎回滚退租销账）。
    m_teams.setTeammateLauncher([this](const QString &name, const QString &role,
                                       const QString &prompt, const QString &taskId,
                                       bool requirePlan) -> TeammateRuntime * {
        TeammateRuntime *runtime =
            new TeammateRuntime(name, role, prompt, taskId, requirePlan,
                                &m_teams, &m_bus, &m_taskStore, nullptr);
        m_teammateRuntimes.insert(name, runtime);
        connect(runtime, &TeammateRuntime::turnRequested, this,
                &AgentLoop::onTeammateTurnRequested);
        connect(runtime, &TeammateRuntime::finished, this,
                &AgentLoop::onTeammateFinished);
        // s13 观测面 a 接线（兑现 P3「UI 接线留待后续阶段」决策）：回合心跳与事件转
        // 中继信号给页面。turnRequested 双槽并行（引擎回合驱动 + UI「回合推进」活动行，
        // Qt 多连接语义）；teamEvent 原样转发 type/content（数据域 token 单源在引擎，
        // 禁在此加工）；taskFinished 只中继终局语义「completed」——它非生命周期终点
        //（队友转 Idle 继续领活），页面收此终局冻卡，后续活动另起新卡。
        connect(runtime, &TeammateRuntime::turnRequested, this,
                [this](const QString &mateName) {
                    emit teammateProgress(mateName, QStringLiteral("turn"), QString());
                });
        connect(runtime, &TeammateRuntime::teamEvent, this,
                [this](const QString &type, const QString &from, const QString &content,
                       const QString & /*requestId*/) {
                    emit teammateProgress(from, type, content);
                });
        connect(runtime, &TeammateRuntime::taskFinished, this,
                [this](const QString &mateName, const QString & /*summary*/) {
                    emit teammateSettled(mateName, QStringLiteral("completed"));
                });
        // P8 需求2：逐工具活动行中继——摘要在宿主渲染（toolSummary 与 Lead 工具卡
        // 同源；AgentLoopInternal.h 已 include），引擎 ok 标志刻意丢弃（子代理
        // 口径：进度行不着色）。异步 bash 行由引擎在真实收口点发，本链零时序加工。
        connect(runtime, &TeammateRuntime::teammateToolActivity, this,
                [this](const QString &mateName, int turnNo, const QString &toolName,
                       const QJsonObject &args, bool /*ok*/) {
                    emit teammateToolProgress(mateName, turnNo, toolName,
                                              AgentLoopDetail::toolSummary(toolName, args));
                });
        QTimer::singleShot(0, runtime, [runtime] { runtime->start(); });
        return runtime;
    });

    // 注入④（D6 钉死：队友 ASK 硬拒，不弹审批卡——lcc prompt_user=False 同型）。
    // 围栏根取 m_workDir：worktree 落会话根 .worktrees（workDir 子树）内，
    // 「逃逸出 Lead 工作区」是「逃逸出队友自身 worktree」的保守超集——偏差登记。
    // bash 走硬禁止表（文案逐字同源 checkDenyList）；规则命中折成
    // "Permission required: <reason>" 判定文本回还队友模型。
    m_teams.setPermissionCheck([this](const QString &toolName, const QJsonObject &params) -> QString {
        if (toolName == ToolNames::BASH) {
            const QString danger =
                checkDenyList(params.value(QStringLiteral("command")).toString());
            if (!danger.isEmpty())
                return danger;
        }
        const QString reason = checkPermissionRules(m_workDir, toolName, params);
        if (!reason.isEmpty())
            return QStringLiteral("Permission required: ") + reason;
        return QString();
    });

    // 注入⑤（偏C' 口径）：钩子链复用主循环四事件内核的 Pre/PostToolUse 两段。
    // toolCall 按 OpenAI 形态组装（callToolName/callToolArgs 读 function.name/
    // function.arguments，arguments 为紧凑 JSON 串——与主循环流式拼装产物同构）。
    // PreToolUse 的 ASK: 前缀在本出口转成硬拒文本：队友无交互批准通道，
    // 挂起语义（m_awaitingPermission）绝不可被队友触发（注入④同源裁决）。
    m_teams.setHooksTrigger([this](const QString &eventName, const QString &toolName,
                                   const QJsonObject &params, const QString &output) -> QString {
        QJsonObject function;
        function[QStringLiteral("name")] = toolName;
        function[QStringLiteral("arguments")] =
            QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
        QJsonObject toolCall;
        toolCall[QStringLiteral("function")] = function;
        if (eventName == QLatin1String("PostToolUse"))
            return triggerPostToolUseHooks(toolCall, output);
        if (eventName != QLatin1String("PreToolUse"))
            return QString();
        QString gate = triggerPreToolUseHooks(toolCall, false);
        const QString ask = AgentLoopDetail::askPrefix();
        if (gate.startsWith(ask))
            gate = QStringLiteral("Permission required: ") + gate.mid(ask.size());
        return gate;
    });

    // 注入⑥（挂载十八）：基五件适配器。文件四件直通 run*In 静态内核——cwd 由
    // 引擎按 assignmentCwd 现读传入（fix-4 钉死第 3 条：禁读租约缓存），与 Lead
    // 主循环同一套实现（含 LineEnding 三道防线/截断/限界，零复制）。
    m_teams.setToolAdapter(ToolNames::READ_FILE,
                           [this](const QJsonObject &params, const QString &cwd) {
        return runReadFileIn(cwd, params);
    });
    m_teams.setToolAdapter(ToolNames::WRITE_FILE,
                           [this](const QJsonObject &params, const QString &cwd) {
        return runWriteFileIn(cwd, params);
    });
    m_teams.setToolAdapter(ToolNames::EDIT_FILE,
                           [this](const QJsonObject &params, const QString &cwd) {
        return runEditFileIn(cwd, params);
    });
    m_teams.setToolAdapter(ToolNames::GLOB,
                           [this](const QJsonObject &params, const QString &cwd) {
        return runGlobIn(cwd, params);
    });
    // bash 适配器（Gate③ MINOR-4 异步化真实落地）：改走异步桥——原同步形态把
    // lcc daemon 线程里的 subprocess.run(timeout) 压缩成主线程有界等待
    // （waitForStarted/waitForFinished ≤kBashTimeoutMs），GUI 冻结整段时长；现换
    // 信号驱动 BashRunner::start（与主循环 run_in_background 同机制，零线程零嵌套
    // 事件循环），原 P3b 同步偏差登记就此撤销。
    // danger 前置判定保持同步（主循环前台分支同口径：命中名单不发起进程）——
    // done 同步早归经 manager 的 earlyResult 暂存，setPendingResume 挂位即同栈续跑。
    // 收口三分支皆有界：FailedToStart（errorOccurred）/ 超时（BashRunner 内建
    // singleShot kBashTimeoutMs kill，finished 收口成 kBashTimeoutError）/ 正常
    // finished（finalizeOutput 截断 + formatBashResult 非零退出码前缀）。
    // 进程 parent=宿主：runtime 在挂起窗口被清算时 manager 已销账（结果丢弃，
    // 裁决口径），进程随宿主析构出清，晚归信号不再触发。
    m_teams.setToolAsyncAdapter(ToolNames::BASH,
                                [this](const QJsonObject &params, const QString &cwd,
                                       std::function<void(const QString &result)> done) {
        const QString command = params.value(QStringLiteral("command")).toString();
        const QString danger =
            BashRunner::dangerWarning(command, AgentLoopDetail::bashDenyList());
        if (!danger.isEmpty()) {
            done(danger);
            return;
        }
        const auto timedOut = std::make_shared<bool>(false);
        const auto settled = std::make_shared<bool>(false);
        BashRunner::start(
            command, cwd, this, nullptr, timedOut,
            [timedOut, settled, done](QProcess *process) {
                QObject::connect(process, &QProcess::errorOccurred, process,
                                 [process, settled, done](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart || *settled) {
                        return;
                    }
                    *settled = true;
                    done(QStringLiteral("Error: bash 启动失败：powershell.exe 无法启动（%1）")
                             .arg(process->errorString()));
                    process->deleteLater();
                });
                QObject::connect(process, &QProcess::finished, process,
                                 [process, timedOut, settled, done](int, QProcess::ExitStatus) {
                    if (*settled) {
                        return; // FailedToStart 时 errorOccurred/finished 连发，先到者收口
                    }
                    *settled = true;
                    const QString base = BashRunner::finalizeOutput(process, *timedOut);
                    if (*timedOut) {
                        done(base); // 即 kBashTimeoutError 文案（不读缓冲，口径同前台异步版）
                    } else {
                        done(BackgroundTasksManager::formatBashResult(
                            base, process->exitCode(), false));
                    }
                    process->deleteLater();
                });
            });
    });
}

// ═══════════════════════════════════════════════════════════════
// Lead 工具面 cwd（挂载十三/十四的取源；lcc _run_base current_cwd 等价）
// ═══════════════════════════════════════════════════════════════

QString AgentLoop::leadToolCwd()
{
    // assignmentCwd ①号分支（Lead 无租约）经 workDirSink 直接给 m_workDir——
    // 与 s13 前行为逐字一致（真回落，非失真，不打警告）；有租约→租约任务现读盘
    // 校验（status/owner 失真即 false）→worktree 绑定经 resolver 折入。
    // Gate③ MINOR-2 注释纠偏（原盘码注释失实，现以 lcc 盘码为准）：lcc
    // tools_manager.py:559-563 _agent_cwd **不 raise**——catch (FileNotFoundError,
    // ValueError) 后把 "Error: Invalid task assignment: {exc}" 作为工具输出串折叠
    // （run_agent_* 五个包装 :565-583 `return error or self.run_x(..., cwd=cwd)`），
    // 即 lcc 失真租约下 Lead 工具**失败**而非回落。lite 采 fail-open 有意偏离：
    // 本函数返回值被消费方一律当路径用（run*In 围栏根 / BashRunner cwd），签名
    // 与消费点均不在本轮写域，无法透传错误串；失真多由用户外部动盘造成，保守
    // 回落主工作区并 qWarning 留痕（禁静默），Lead 可经 list_tasks/get_task 自纠。
    QString cwd;
    QString error;
    const bool leased = m_taskStore.assignmentCwd(
        QString::fromLatin1(AgentTeamsManager::kLeadOwnerKey), &cwd, &error);
    if (leased && !cwd.isEmpty())
        return cwd;
    if (!leased)
    {
        // 租约失真分支（assignmentCwd=false 仅出现在有租约但盘校失败；Lead 无租约
        // 走①号 true 回落）：与「无租约回落」区分留痕，日志各走各路
        qWarning().noquote()
            << QStringLiteral("[team] lead lease invalid, falling back to workDir: %1").arg(error);
    }
    return m_workDir;
}

// ═══════════════════════════════════════════════════════════════
// 团队事件收割与回合终局清算（挂载十/十一；lcc inject_team_events/finally 转译）
// ═══════════════════════════════════════════════════════════════

void AgentLoop::injectTeamEvents()
{
    // 破坏性收割 lead 邮箱（consumeLeadInbox：drain+协议簿记，回执不吞）。
    // 空批不产生注入（偏G 同源：formatTeamEvents 空批返空串，此处提前返回，
    // 杜绝无信息量的 [Team events] 头污染会话/缓存）。
    const QVector<BusMessage> events = m_teams.consumeLeadInbox();
    if (events.isEmpty())
        return;
    const QString joined = AgentTeamsManager::formatTeamEvents(events);
    if (joined.isEmpty())
        return;

    // 注入形态逐字复刻 injectBackgroundResults（lcc inject_team_events :145-157
    // 的扁平字符串转译）：末条 user 合并进 content 尾部，否则新增一条 user 消息。
    if (!m_messages.isEmpty()
        && m_messages.back().value(QStringLiteral("role")).toString() == QStringLiteral("user"))
    {
        QJsonObject &tail = m_messages.back();
        tail[QStringLiteral("content")] =
            tail.value(QStringLiteral("content")).toString() + QStringLiteral("\n\n") + joined;
    }
    else
    {
        QJsonObject injected;
        injected[QStringLiteral("role")] = QStringLiteral("user");
        injected[QStringLiteral("content")] = joined;
        m_messages.append(injected);
    }

    qDebug().noquote() << QStringLiteral("[team] events\n%1").arg(joined);
}

void AgentLoop::tryDeliverTeamEvents()
{
    // Gate③ MAJOR-1：lcc loop.py:340-343/:401-408 wake 分支转译——Lead 空闲时
    // 团队事件自动开新回合，兑现 spawn 成功文案「End this turn; the runtime will
    // deliver its events.」与 teams 纪律段「运行时会投递团队事件并唤醒你」；
    // m_running 卫兵=lcc 仅在 wait_for_cli_event 等待态消费（回合内三边界不受影响）。
    if (m_running)
        return;
    // 门铃（lcc :342 peek("lead") 非破坏偷看同型）：收件名以现盘对账——
    // AgentTeamsManager.cpp:23 kLeadName=QStringLiteral("lead")、consumeLeadInbox
    // 经 :238 m_bus->drain(kLeadName) 收「lead」；oracle 报告处写 hasPending("agent")
    // 系把 TaskStore owner 键（kLeadOwnerKey="agent"）误当邮箱名，采「lead」。
    // 引擎常量未导出而引擎只读→宿主此处字面量 duplication，两处口径以注释互钉。
    if (!m_bus.hasPending(QStringLiteral("lead")))
        return;
    // 破坏性收割（lcc :402 consume_lead_inbox）：M8 fail-closed（unlink 失败→空批
    // +lastError）与 peek→drain 竞态空窗（事件已被他路收走）均折为空批——本拍放弃
    // 不开空回合，下拍门铃再试（lcc :403-404 防御空窗 continue 同型）。
    const QVector<BusMessage> events = m_teams.consumeLeadInbox();
    if (events.isEmpty())
        return;
    const QString text = AgentTeamsManager::formatTeamEvents(events);
    if (text.isEmpty()) // 偏G 同源卫兵：空渲染不开回合
        return;
    // 同栈直连宿主=与 tryDeliverCron（AgentLoopCron.cpp:44-63）完全同款路径，
    // GUI 侧零改动：ChatSessionPage 既有槽（:408-414）addMessage(display)+
    // startAssistantStream(request)→run() 同步置位 m_running（本拍随后的
    // tryDeliverCron 因 priority inbox→cron 被卫兵跳过，见 AgentLoop.cpp tick 注）。
    // displayText=activeRequestText=渲染块文本——lcc inject_team_events(text)+
    // _run_turn(text) 双条 user 在 lite 并为一条（cron「N 条→1 条」先例同款偏差）；
    // 新回合 run() 挂载点一的 injectTeamEvents 此刻邮箱已空=天然 no-op 不重复消费。
    // 偏差登记：lcc skip_approval=True（:408）在 lite 无对应物——Lead 保留 ASK 卡
    //（Gate③ 裁决「lite 增强非缺陷」）；bus 无 cron 式两段台账，无宿主接线时本批
    // 丢失——与 lcc consume 后即弃的一次性语义同性质（GUI 恒接线，理论分支）。
    emit scheduledUserMessage(text, text);
    qInfo().noquote() << QStringLiteral("[team] wake: delivered %1 events").arg(events.size());
    // m_teamWasActive 边沿无需在此维护：所开新回合的任一终局均经 settleLeadLease
    // 统一刷新（Gate③ NIT-1 下沉；lcc loop.py:419 每轮圈尾核对的 lite 落点）。
}

void AgentLoop::settleLeadLease()
{
    // lcc agent_loop finally 的 release_completed_assignment("agent")：仅当租约指向
    // completed 任务才清账；false=无租约/未完成/不可读，幂等安全一律忽略
    //（TaskStore.h:134 契约）。折叠文案丢弃——非自然终局无收尾注入时机，
    // 台账欠账留待下一自然回合 settleLeadLease/收割。
    QString error;
    m_taskStore.releaseCompletedAssignment(
        QString::fromLatin1(AgentTeamsManager::kLeadOwnerKey), &error);

    // Gate③ NIT-1：lcc loop.py:419 check_team_offline_edge 挂在**外层 run 每轮圈尾**
    // （自然终局/撞上限/流错误全走），lite 原仅挂 leadTurnEndSettlement（自然终局）
    // ——现迁入本函数：四类终局（自然/双上限/流错误/stop）全部经过此处，边沿恰好
    // 每终局核对一次，文案与语义不变。
    const bool active = !m_teams.teammateNames().isEmpty();
    if (m_teamWasActive && !active)
        qDebug() << "所有队友已下线，如需继续协作可再次 spawn_teammate";
    m_teamWasActive = active;
}

void AgentLoop::leadTurnEndSettlement()
{
    // lcc loop.py :168-172 finally（退租）+ 每回合圈尾收割的三合一：
    // 退租 → 收割 lead 邮箱注入 → 下线边沿检测。注入的 user 消息随调用点
    // 下方的 persistHistory 一并落盘（挂载三置于 finalizeInFlightDelivery 前）。
    // Gate③ NIT-1：下线边沿检测已下沉 settleLeadLease（四类终局全覆盖，lcc
    // loop.py:419 每轮圈尾语义），本函数不再重复核对。
    settleLeadLease();
    injectTeamEvents();
    //（Gate③ MAJOR-1 更新：lcc wake 分支 :401-408 的「空闲即开新回合」已另立
    // tick 路径 tryDeliverTeamEvents 落地——本函数三卫兵继续只负责回合内/回合尾
    // 边界收割；skip_approval 无 lite 对应物，Ask 卡对 Lead 保留=增强非缺陷。）
}

// ═══════════════════════════════════════════════════════════════
// 队友回合驱动（挂载八：lcc daemon 线程 consume 循环的事件驱动转译）
// ═══════════════════════════════════════════════════════════════

void AgentLoop::onTeammateTurnRequested(const QString &name)
{
    TeammateRuntime *runtime = m_teammateRuntimes.value(name);
    if (!runtime || runtime->isFinished())
        return;
    if (m_teammateStreams.value(name))
        return; // 单飞防御：上一回合流未收口（正常时序不可达：deliver 后才再 request）

    // 请求体（SubAgent::startChatRequest 同型模板）：system 恒为 runtime 的
    // lcc :811-821 逐字提示词，历史为 runtime 自管的 m_messages（无 system 副本）。
    // 偏差登记：不接 usage 锚（上下文计量只属 Lead 会话）、不开 enable_thinking
    //（黑盒同 SubAgent 口径）、不带 stream_options.include_usage。
    QJsonArray messages;
    QJsonObject systemMsg;
    systemMsg[QStringLiteral("role")] = QStringLiteral("system");
    systemMsg[QStringLiteral("content")] = runtime->systemPrompt();
    messages.append(systemMsg);
    const QVector<QJsonObject> &history = runtime->messages();
    for (const QJsonObject &msg : history)
        messages.append(msg);

    QJsonObject request;
    request[QStringLiteral("model")] = m_model;
    request[QStringLiteral("messages")] = messages;
    request[QStringLiteral("tools")] = AgentLoopTeam::teammateToolsDefinition();
    request[QStringLiteral("max_tokens")] = AgentConst::kMaxTokens;

    QOpenAi::ChatStream *stream = QOpenAi::chat().createStream(request, this);
    m_teammateStreams.insert(name, stream);
    // QPointer 守卫：流回调到达时 runtime 可能已被清算（cancel→deleteLater 时序）
    QPointer<TeammateRuntime> guard(runtime);

    connect(stream, &QOpenAi::ChatStream::messageFinished, this,
            [this, name, stream, guard](const QJsonObject &fullMsg) {
        if (m_teammateStreams.value(name) == stream)
            m_teammateStreams.remove(name);
        stream->deleteLater();
        if (!guard || guard->isFinished())
            return;
        // 回填回合产物：工具块执行/信箱中继/退租/待命全在引擎单点（runtime→manager）
        guard->deliverTurnResult(fullMsg.value(QStringLiteral("content")).toString(),
                                 fullMsg.value(QStringLiteral("tool_calls")).toArray());
    });
    connect(stream, &QOpenAi::ChatStream::error, this,
            [this, name, stream, guard](const QString &message) {
        if (m_teammateStreams.value(name) == stream)
            m_teammateStreams.remove(name);
        stream->deleteLater();
        if (!guard || guard->isFinished())
            return;
        // lcc work() except 形态 f"{type}: {exc}" → Qt 无异常，P2 钉死宿主侧
        // 组装 APIError 前缀；非空 errorMessage 令 runtime 走 error 信箱 + finish()
        guard->deliverTurnResult(QString(), QJsonArray(),
                                 QStringLiteral("APIError: %1").arg(message));
    });
}

void AgentLoop::onTeammateFinished(const QString &name)
{
    // 观测面 a：退出终局中继（置于 take 前——句柄表状态与 UI 终局无关，凡 finished
    // 到达必终局一次；页面 settleTeammateCard 幂等，与兜底扫不双计）
    emit teammateSettled(name, QStringLiteral("exited"));
    // finished 信号可能从 deliverTurnResult/finish() 调用栈中段发出——只许
    // deleteLater，禁即时 delete（FIND-L 契约）。
    QPointer<QOpenAi::ChatStream> stream = m_teammateStreams.take(name);
    if (stream)
        stream->cancel(); // 防御：理论上 finished 恒晚于流收口，此处兜底断流
    TeammateRuntime *runtime = m_teammateRuntimes.take(name);
    if (!runtime)
        return;
    runtime->deleteLater();
}

// ═══════════════════════════════════════════════════════════════
// 退出清算（挂载十二：~AgentLoop 体内调用，先于引擎按值成员析构）
// ═══════════════════════════════════════════════════════════════

void AgentLoop::settleTeamOnExit()
{
    // lcc loop.py :422-430 finally 的 lite 转译：先快照在册名再逐个清算
    //（lcc「先取 activeTeammates 再发信」同型）。偏差登记：不发礼貌 shutdown_request
    // 广播——析构后无事件循环消费应答与收尾回合，账目收口才是硬需求：
    // cancel()→finish()→settleLedgers（退租 releaseTeammateAssignment +
    // finalizeTeammate 弹四本账）。blockSignals 断 finished→onTeammateFinished
    // 自销路径的重入，随后即时 delete 安全（信号已屏蔽、singleShot(0) 的 start
    // 以 runtime 为 context 随其销毁自动失效；未 start 的出生即退队友由 ~TeammateRuntime
    // 的 FIND-L 账目核兜底——此处显式 cancel 已先行覆盖正道）。
    for (auto it = m_teammateStreams.begin(); it != m_teammateStreams.end(); ++it) {
        if (QOpenAi::ChatStream *stream = it.value())
            stream->cancel();
    }
    m_teammateStreams.clear();

    const QStringList names = m_teammateRuntimes.keys();
    for (const QString &name : names) {
        // 观测面 a 终局兜底：blockSignals 会吞掉 runtime 的 finished——若不在此先发
        // 「settled」中继，聊天流里的队友卡将永转「执行中」。页面幂等收口（QPointer）
        TeammateRuntime *runtime = m_teammateRuntimes.take(name);
        if (!runtime)
            continue;
        emit teammateSettled(name, QStringLiteral("settled"));
        runtime->blockSignals(true);
        runtime->cancel();
        delete runtime;
    }
}

// ═══════════════════════════════════════════════════════════════
// 观测面 b：队友名册只读口 + 1s 节拍变更广播（见 AgentLoop.cpp tick 挂点）
// ═══════════════════════════════════════════════════════════════

QList<QPair<QString, QString>> AgentLoop::teammateRoster() const
{
    // ledger 纯读（statusName 为数据域 token，本地化归侧栏）；缺账兜底 Working
    // 与引擎 value_or 先例同口径（正常不可达：names 出自同一本账）
    QList<QPair<QString, QString>> roster;
    const QStringList names = m_teams.teammateNames();
    roster.reserve(names.size());
    for (const QString &name : names) {
        const std::optional<AgentTeamsManager::TeammateStatus> status =
            m_teams.teammateStatus(name);
        roster.append(qMakePair(
            name, AgentTeamsManager::statusName(
                      status.value_or(AgentTeamsManager::TeammateStatus::Working))));
    }
    return roster;
}

void AgentLoop::updateTeamRosterBroadcast()
{
    const QList<QPair<QString, QString>> roster = teammateRoster();
    QStringList sig;
    sig.reserve(roster.size());
    for (const auto &entry : roster)
        sig << entry.first + QLatin1Char('|') + entry.second;
    const QString signature = sig.join(QLatin1Char(';'));
    if (signature == m_teamRosterSignature)
        return; // 边沿检测：无变化不广播（初值空串=空名册，首 tick 不刷屏）
    m_teamRosterSignature = signature;
    emit teamRosterChanged();
}

// ═══════════════════════════════════════════════════════════════
// 队友侧 10 工具 schema（lcc TEAMMATE_TOOLS :100-212 逐字；决策 D5 与主表缓存分立）
// ═══════════════════════════════════════════════════════════════

QJsonArray AgentLoopTeam::teammateToolsDefinition()
{
    // 禁翻区：全部 QStringLiteral、永不 tr()（发往 LLM，纪律同主表 createToolsDefinition）。
    static const QJsonArray cached = [] {
        auto prop = [](const QString &type) {
            QJsonObject o;
            o[QStringLiteral("type")] = type;
            return o;
        };
        auto tool = [&prop](const QString &name, const QString &description,
                            const QList<QPair<QString, QString>> &props,
                            const QStringList &required) {
            QJsonObject properties;
            for (const auto &p : props)
                properties.insert(p.first, prop(p.second));
            QJsonObject parameters;
            parameters[QStringLiteral("type")] = QStringLiteral("object");
            parameters[QStringLiteral("properties")] = properties;
            if (!required.isEmpty()) {
                QJsonArray req;
                for (const QString &r : required)
                    req.append(r);
                parameters[QStringLiteral("required")] = req;
            }
            QJsonObject function;
            function[QStringLiteral("name")] = name;
            function[QStringLiteral("description")] = description;
            function[QStringLiteral("parameters")] = parameters;
            QJsonObject item;
            item[QStringLiteral("type")] = QStringLiteral("function");
            item[QStringLiteral("function")] = function;
            return item;
        };

        QJsonArray tools;
        // —— 基五件（lcc _TEAMMATE_BASE_TOOLS；bash 无 run_in_background——队友不建后台台账）
        tools.append(tool(QStringLiteral("bash"), QStringLiteral("Run a shell command."),
                          {qMakePair(QString(QStringLiteral("command")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("command")}));
        // Gate③ NIT-2：desc 逐字对齐 lcc :113/:122（无句号；bash "Run a shell
        // command." lcc :103 原文带句号，lite 保持带句号=正确 parity）
        tools.append(tool(QStringLiteral("read_file"), QStringLiteral("Read file contents"),
                          {qMakePair(QString(QStringLiteral("path")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("limit")),
                                     QString(QStringLiteral("integer")))},
                          {QStringLiteral("path")}));
        tools.append(tool(QStringLiteral("write_file"), QStringLiteral("Write content to a file"),
                          {qMakePair(QString(QStringLiteral("path")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("content")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("path"), QStringLiteral("content")}));
        tools.append(tool(QStringLiteral("edit_file"),
                          QStringLiteral("Replace exact text in a file once."),
                          {qMakePair(QString(QStringLiteral("path")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("old_string")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("new_string")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("path"), QStringLiteral("old_string"),
                           QStringLiteral("new_string")}));
        tools.append(tool(QStringLiteral("glob"),
                          QStringLiteral("Find files matching a glob pattern; ** matches recursively."),
                          {qMakePair(QString(QStringLiteral("pattern")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("pattern")}));
        // —— 团队协作五件（submit_plan 不在 ToolNames：引擎匿名 ns 单源，此处 schema 字面量）
        tools.append(tool(QStringLiteral("send_message"),
                          QStringLiteral("Send an intermediate message to 'lead' or an active teammate."),
                          {qMakePair(QString(QStringLiteral("to")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("content")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("to"), QStringLiteral("content")}));
        tools.append(tool(QStringLiteral("submit_plan"),
                          QStringLiteral("Submit a work plan for Lead approval."),
                          {qMakePair(QString(QStringLiteral("plan")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("plan")}));
        // list_tasks：空 properties 且不写 required 键（lcc 同型；tool 工厂对空
        // required 列表即省略该键，口径同主表 list_tasks 手搓件）
        tools.append(tool(QStringLiteral("list_tasks"),
                          QStringLiteral("List tasks with status, owner, and dependencies."),
                          {}, {}));
        tools.append(tool(QStringLiteral("claim_task"),
                          QStringLiteral("Claim a pending task whose dependencies are complete."),
                          {qMakePair(QString(QStringLiteral("task_id")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("task_id")}));
        tools.append(tool(QStringLiteral("complete_task"),
                          QStringLiteral("Complete the task claimed by this agent."),
                          {qMakePair(QString(QStringLiteral("task_id")),
                                     QString(QStringLiteral("string")))},
                          {QStringLiteral("task_id")}));
        return tools;
    }();
    return cached;
}
