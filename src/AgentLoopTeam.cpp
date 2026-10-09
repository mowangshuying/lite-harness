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
        // 决策（最小 UI 面）：teamEvent/taskFinished 展示信号不接卡片——团队事件经
        // [Team events] 注入文本进消息流即可见，UI 接线留待后续阶段。
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
    // bash 适配器（偏差登记，P3b 已知代价）：引擎适配器契约为同步 QString 返回，
    // lcc 队友 bash=daemon 线程内 subprocess.run(timeout) 阻塞——lite 零线程约束
    // 把这段阻塞压缩到主线程（至多 kBashTimeoutMs+启动/回收界）。Lead 自身 bash
    // 仍异步不受影响；GUI 冻结风险已知，后续如需再开异步适配器通道须改引擎契约。
    // 输出收口逐点对齐前台异步分支（AgentLoopBash.cpp）：dangerWarning 前置、
    // finalizeOutput 截断/超时文案、非零退出码走 formatBashResult 前缀。
    m_teams.setToolAdapter(ToolNames::BASH,
                           [this](const QJsonObject &params, const QString &cwd) {
        const QString command = params.value(QStringLiteral("command")).toString();
        const QString danger =
            BashRunner::dangerWarning(command, AgentLoopDetail::bashDenyList());
        if (!danger.isEmpty())
            return danger;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setWorkingDirectory(cwd);
        process.start(QStringLiteral("powershell.exe"),
                      {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                       QStringLiteral("-Command"), command});
        if (!process.waitForStarted(AgentConst::kTeamBashStartWaitMs))
            return QStringLiteral("Error: bash 启动失败：powershell.exe 无法启动（%1）")
                .arg(process.errorString());
        bool timedOut = false;
        if (!process.waitForFinished(AgentConst::kBashTimeoutMs)) {
            timedOut = true;
            process.kill();
            process.waitForFinished(AgentConst::kTeamBashKillWaitMs);
        }
        const QString base = BashRunner::finalizeOutput(&process, timedOut);
        if (timedOut)
            return base; // 即 kBashTimeoutError 文案（不读缓冲，口径同异步版）
        return BackgroundTasksManager::formatBashResult(base, process.exitCode(), false);
    });
}

// ═══════════════════════════════════════════════════════════════
// Lead 工具面 cwd（挂载十三/十四的取源；lcc _run_base current_cwd 等价）
// ═══════════════════════════════════════════════════════════════

QString AgentLoop::leadToolCwd()
{
    // assignmentCwd ①号分支（Lead 无租约）经 workDirSink 直接给 m_workDir——
    // 与 s13 前行为逐字一致；有租约→租约任务现读盘校验（status/owner 失真即 false）
    // →worktree 绑定经 resolver 折入。false/空 → 回落 m_workDir（fail-open 偏差
    // 登记：lcc current_cwd 对失真租约 raise 炸回合，lite 工具面失败折叠纪律下
    // 保守回落主工作区，宁可让 Lead 在旧目录干活也不毁回合——Lead 是交互主体，
    // 失真多由用户外部动盘造成，回落后 list_tasks/get_task 可自纠）。
    QString cwd;
    QString error;
    if (m_taskStore.assignmentCwd(QString::fromLatin1(AgentTeamsManager::kLeadOwnerKey),
                                  &cwd, &error)
        && !cwd.isEmpty())
        return cwd;
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
    // m_teamWasActive 边沿无需在此维护：所开新回合的自然终局 leadTurnEndSettlement
    // 统一刷新（lcc loop.py:419 每轮圈尾核对的 lite 落点）。
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
}

void AgentLoop::leadTurnEndSettlement()
{
    // lcc loop.py :168-172 finally（退租）+ 每回合圈尾收割的三合一：
    // 退租 → 收割 lead 邮箱注入 → 下线边沿检测。注入的 user 消息随调用点
    // 下方的 persistHistory 一并落盘（挂载三置于 finalizeInFlightDelivery 前）。
    settleLeadLease();
    injectTeamEvents();
    // lcc check_team_offline_edge :160-164：active→全下线边沿记一次日志。
    //（Gate③ MAJOR-1 更新：lcc wake 分支 :401-408 的「空闲即开新回合」已另立
    // tick 路径 tryDeliverTeamEvents 落地——本函数三卫兵继续只负责回合内/回合尾
    // 边界收割；skip_approval 无 lite 对应物，Ask 卡对 Lead 保留=增强非缺陷。）
    const bool active = !m_teams.teammateNames().isEmpty();
    if (m_teamWasActive && !active)
        qDebug() << "所有队友已下线，如需继续协作可再次 spawn_teammate";
    m_teamWasActive = active;
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
        TeammateRuntime *runtime = m_teammateRuntimes.take(name);
        if (!runtime)
            continue;
        runtime->blockSignals(true);
        runtime->cancel();
        delete runtime;
    }
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
        tools.append(tool(QStringLiteral("read_file"), QStringLiteral("Read file contents."),
                          {qMakePair(QString(QStringLiteral("path")),
                                     QString(QStringLiteral("string"))),
                           qMakePair(QString(QStringLiteral("limit")),
                                     QString(QStringLiteral("integer")))},
                          {QStringLiteral("path")}));
        tools.append(tool(QStringLiteral("write_file"), QStringLiteral("Write content to a file."),
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
