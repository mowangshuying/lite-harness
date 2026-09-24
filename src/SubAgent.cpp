#include "SubAgent.h"

#include "QOpenAi.h"
#include "ToolNames.h" // 工具名集中常量（lcc a6d29b9 tool_names.py 移植）

#include <QDebug>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>

#include <memory>
#include <utility>

namespace {

// 子代理轮次预算（lcc s06 MAX_SUBAGENT_TURNS）：每次发起请求消耗一轮
// （lcc 9165f8f 后子代理不再触发 Stop，见最终回答分支）
constexpr int kMaxSubagentTurns = 50;

// 子代理工具白名单（lcc s06 subTools）：主循环 7 工具定义中的前 5 个，
// 定义逐字共享（todo_write/task 不进子表，模型幻觉调用也只落 Unknown 回填）。
// 注：全量定义由成员函数取得后传入（createToolsDefinition 为 AgentLoop 私有静态，
// friend 权限仅覆盖 SubAgent 成员，不覆盖本自由函数）
QJsonArray filterSubTools(const QJsonArray &all)
{
    static const QSet<QString> allowed = {
        ToolNames::BASH, ToolNames::READ_FILE, ToolNames::WRITE_FILE,
        ToolNames::EDIT_FILE, ToolNames::GLOB,
    };

    QJsonArray sub;
    for (const QJsonValue &value : all)
    {
        const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!allowed.contains(name))
            continue;

        if (name == ToolNames::BASH)
        {
            // 子代理 bash 专用（lcc s11 95242de sub_bash_info 等价）：从 schema  properties
            // 剔除 run_in_background——schema 层禁止后台（执行层双保险见 SubAgent::executeTool）。
            // QJsonObject 隐式共享，逐层拷贝修改即深拷贝语义（lcc deepcopy 的 lite 等价）
            QJsonObject tool = value.toObject();
            QJsonObject fn = tool.value(QStringLiteral("function")).toObject();
            QJsonObject params = fn.value(QStringLiteral("parameters")).toObject();
            QJsonObject props = params.value(QStringLiteral("properties")).toObject();
            props.remove(QStringLiteral("run_in_background"));
            params[QStringLiteral("properties")] = props;
            fn[QStringLiteral("parameters")] = params;
            tool[QStringLiteral("function")] = fn;
            sub.append(tool);
            continue;
        }

        sub.append(value);
    }
    return sub;
}

} // namespace

SubAgent::SubAgent(AgentLoop *host, QObject *parent, const QString &workDir,
                   const QString &model, const QString &prompt)
    : QObject(parent)
    , m_host(host)
    , m_workDir(workDir)
    , m_model(model)
    , m_prompt(prompt)
    , m_handlers(AgentLoop::baseFileToolHandlers(workDir))
{
}

QString SubAgent::subSystemPrompt(const QString &workDir)
{
    // lcc s06 原文为两段相邻字面量拼接（句间无空格），按规格裁决规整为正常空格——
    // 此处为与 lcc 文本的有意偏差之一（主循环 prompt 保留无空格quirk，见 AgentLoop.cpp）
    return QStringLiteral("You are a coding agent at %1. Complete the given task, then return a concise final answer.")
        .arg(workDir);
}

void SubAgent::start(CompleteHandler onComplete)
{
    m_onComplete = std::move(onComplete);

    // 独立上下文：仅 system（子代理 prompt）+ user（任务描述），不携带主循环任何历史
    //（lcc run_subagent 的 messages=[{role:user,...}] + system= 参数，OpenAI 协议下
    //  system 作为首条消息发送，与主循环同一形态）
    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = subSystemPrompt(m_workDir);
    m_messages.append(systemMessage);

    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    userMessage[QStringLiteral("content")] = m_prompt;
    m_messages.append(userMessage);

    startChatRequest();
}

void SubAgent::startChatRequest()
{
    if (m_cancelled || m_settled)
        return;

    // 轮次预算：50 次请求内未产出最终答案则以停跑文案收尾（lcc 循环耗尽后的 return 文案）
    if (m_turns >= kMaxSubagentTurns)
    {
        finish(QStringLiteral("Subagent stopped after %1 turns without a final answer.")
                   .arg(kMaxSubagentTurns));
        return;
    }
    ++m_turns;

    QJsonObject request;
    request[QStringLiteral("model")] = m_model;
    QJsonArray messagesJson;
    for (const QJsonObject &msg : m_messages)
        messagesJson.append(msg);
    request[QStringLiteral("messages")] = messagesJson;
    // 成员函数内调用私有静态（friend 生效），过滤交给自由函数
    request[QStringLiteral("tools")] = filterSubTools(AgentLoop::createToolsDefinition());
    // lcc s06：显式输出上限（与主循环同为 8000）；子代理不开 enable_thinking（黑盒无思考展示）
    request[QStringLiteral("max_tokens")] = 8000;

    QOpenAi::ChatStream *s = QOpenAi::chat().createStream(request, this);
    m_currentStream = s;

    // 黑盒：有意不连接 thinkingDelta/textDelta——子代理过程不进入 UI

    connect(s, &QOpenAi::ChatStream::messageFinished, this, [this, s](const QJsonObject &fullMsg) {
        m_currentStream = nullptr;
        s->deleteLater();
        if (m_cancelled)
            return;

        const QJsonArray toolCalls = fullMsg.value(QStringLiteral("tool_calls")).toArray();
        if (toolCalls.isEmpty())
        {
            m_messages.append(fullMsg);

            // Stop 钩子不在子代理触发（lcc 9165f8f）：Stop 只在主循环最终回答处触发一次，
            // 子代理黑盒终点即汇总；轮次上限收尾保留（startChatRequest 顶部检查）

            // extract_text 等价：OpenAI 形态下 content 即纯文本；空内容按 lcc 兜底文案
            const QString text = fullMsg.value(QStringLiteral("content")).toString();
            finish(text.isEmpty() ? QStringLiteral("(no summary)") : text);
            return;
        }

        continueWithToolResults(fullMsg);
    });

    connect(s, &QOpenAi::ChatStream::error, this, [this, s](const QString &msg) {
        m_currentStream = nullptr;
        s->deleteLater();
        if (m_cancelled)
            return;
        // API 失败以字符串结果回给主模型（lcc except → return "Error: ..."），
        // 不触发主循环 error 信号、不中断主循环
        finish(QStringLiteral("Error: subagent API call failed: %1").arg(msg));
    });
}

void SubAgent::continueWithToolResults(const QJsonObject &assistantMessage)
{
    m_messages.append(assistantMessage);
    m_pendingToolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    m_toolResultsReady = QJsonArray();

    runNextTool();
}

void SubAgent::runNextTool()
{
    if (m_cancelled || m_settled)
        return;

    if (m_pendingToolCalls.isEmpty())
    {
        // 本批工具全部完成：回填结果并再次请求（无 todo 提醒逻辑——lcc 子代理循环没有）
        for (const QJsonValue &value : m_toolResultsReady)
            m_messages.append(value.toObject());
        m_toolResultsReady = QJsonArray();

        startChatRequest();
        return;
    }

    const QJsonObject toolCall = m_pendingToolCalls.takeAt(0).toObject();
    executeTool(toolCall, /*permissionGranted = */ false);
}

void SubAgent::executeTool(const QJsonObject &toolCall, bool permissionGranted)
{
    // 解析工具名与参数（arguments 为流式拼装出的 JSON 字符串）
    const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
    const QString toolName = function.value(QStringLiteral("name")).toString();
    const QJsonObject args =
        QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
    const QString summary = AgentLoop::toolSummaryOf(toolName, args);

    // PreToolUse 钩子链（共用宿主注册表，含 s03 权限门与日志钩子）。返回协议与主循环一致：
    // 1) "ASK:" 前缀 → 需询问：暂停子队列（宿主仍在等 task 回调，整条链冻结），
    //    经自身 permissionRequired → 宿主转发为 UI 既有 3 参信号
    // 2) 其余非空 → 硬拒绝：直接回填为 tool_result（不触发 PostToolUse，对齐 lcc）
    const QString gate = m_host->triggerPreToolUseHooks(toolCall, permissionGranted);
    if (!gate.isEmpty())
    {
        if (gate.startsWith(AgentLoop::askPrefixOf()))
        {
            m_awaitingPermission = true;
            m_pendingPermissionCall = toolCall;
            emit permissionRequired(toolName, summary,
                                    gate.mid(AgentLoop::askPrefixOf().size()));
            return; // 队列暂停：不回填、不请求，等宿主把裁决路由进 resolvePermission()
        }
        onToolFinished(toolCall, gate);
        return;
    }

    // bash 走子代理自身的异步进程链（收口点在进程 finished 回调，簿记独立于宿主）
    if (toolName == ToolNames::BASH)
    {
        // 子代理禁后台·执行层双保险（lcc s11 95242de allow_background=False 等价）：
        // schema 已删 run_in_background（见 filterSubTools），模型若仍幻觉带参，丢弃后
        // 永远前台执行；降级提示仅打控制台、不进模型可见输出（lcc log_warn 语义）
        QJsonObject bashArgs = args;
        if (bashArgs.take(QStringLiteral("run_in_background")).toBool())
                qWarning() << "[bg] not allowed in this context, running in foreground";
        executeBashAsync(toolCall, bashArgs);
        return;
    }

    // 其余工具经 handler 表同步路由；未知名称不中断循环，错误文本作为结果回填
    //（文案沿用主循环 "Unknown tool: %1"，lcc 原文 "Unknown:" 的有意偏差在 s02 已确立）
    auto it = m_handlers.constFind(toolName);
    const QString output = it == m_handlers.constEnd()
        ? QStringLiteral("Unknown tool: %1").arg(toolName)
        : it.value()(args);

    // PostToolUse 钩子（共用宿主注册表）：子代理内部工具同样进入日志钩子
    m_host->triggerPostToolUseHooks(toolCall, output);

    onToolFinished(toolCall, output);
}

void SubAgent::onToolFinished(const QJsonObject &toolCall, const QString &output)
{
    if (m_cancelled || m_settled)
        return;

    // 黑盒收口：只回填 tool 结果，不发 toolOutputReady——task 对外可见性由宿主收口一次
    QJsonObject toolResult;
    toolResult[QStringLiteral("role")] = QStringLiteral("tool");
    toolResult[QStringLiteral("tool_call_id")] = toolCall.value(QStringLiteral("id")).toString();
    toolResult[QStringLiteral("content")] = output;
    m_toolResultsReady.append(toolResult);

    runNextTool();
}

void SubAgent::resolvePermission(bool allow)
{
    // 宿主 resolvePermission 在无自身待决时路由至此；无待决时忽略
    if (!m_awaitingPermission || m_cancelled)
        return;

    const QJsonObject toolCall = m_pendingPermissionCall;
    m_pendingPermissionCall = QJsonObject();
    m_awaitingPermission = false;
    if (m_settled)
        return;

    if (!allow)
    {
        // 拒绝：回填 "Permission denied"（与主循环 s03 文案逐字一致）并续跑子队列；
        // 不触发 PostToolUse（handler 未运行，对齐 lcc 拒绝即 continue）
        onToolFinished(toolCall, QStringLiteral("Permission denied"));
        return;
    }

    // 允许：重走执行链（permissionGranted=true 使权限钩子仅跳过询问规则，
    // deny 列表与 bash 内部黑名单仍照常检查——MINOR-3 双层防御）
    executeTool(toolCall, /*permissionGranted = */ true);
}

void SubAgent::executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args)
{
    const QString command = args.value(QStringLiteral("command")).toString();

    // bash 内部危险黑名单（lcc fddb23e G4 单源：与权限门 DENY_LIST 共用宿主 bashDenyList；
    // 清单与文案与主循环逐字一致，经宿主静态口共用）
    for (const QString &danger : AgentLoop::bashDenyList()) // lcc fddb23e 单源列表（G4）
    {
        if (command.contains(danger, Qt::CaseInsensitive))
        {
            const QString output = QStringLiteral("Error: Dangerous command blocked: %1").arg(command);
            m_host->triggerPostToolUseHooks(toolCall, output);
            onToolFinished(toolCall, output);
            return;
        }
    }

    // 异步执行（QProcess 为 this 子对象，析构自动清理）
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(m_workDir);
    m_activeProcesses.append(process);

    // 120 秒超时（与主循环一致；shared_ptr 标志随两回调捕获，无裸 new/delete——MINOR-2）
    auto timedOut = std::make_shared<bool>(false);
    QTimer::singleShot(120000, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });

    connect(process, &QProcess::finished, this,
            [this, process, toolCall, timedOut](int, QProcess::ExitStatus) {
        m_activeProcesses.removeAll(process);
        if (m_cancelled)
        {
            process->deleteLater();
            return;
        }

        QString output;
        if (*timedOut)
        {
            output = QStringLiteral("Error: Timeout (120s)");
        }
        else
        {
            output = QString::fromLocal8Bit(process->readAllStandardOutput());
            if (output.length() > 50000)
                output = output.left(50000); // 截断（与主循环一致）
            if (output.isEmpty())
                output = QStringLiteral("(no output)");
        }
        process->deleteLater();

        m_host->triggerPostToolUseHooks(toolCall, output);
        onToolFinished(toolCall, output);
    });

    process->start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
}

void SubAgent::finish(const QString &result)
{
    // 完结仅一次；cancel() 之后不再触发（收口配对由宿主 cancelSubAgent() 负责）
    if (m_settled || m_cancelled)
        return;
    m_settled = true;

    const CompleteHandler handler = std::exchange(m_onComplete, nullptr);
    if (handler)
        handler(result);
}

void SubAgent::cancel()
{
    // 幂等：断流、kill 子进程、清队列、抑制回调（宿主随后为 task 合成 "(cancelled)"）
    if (m_cancelled)
        return;
    m_cancelled = true;
    m_settled = true;
    m_onComplete = nullptr;

    QSignalBlocker blocker(this); // 阻断尚未处理的转发链

    if (m_currentStream)
    {
        static_cast<QOpenAi::ChatStream *>(m_currentStream.data())->cancel();
        m_currentStream->disconnect();
        m_currentStream->deleteLater();
        m_currentStream = nullptr;
    }

    for (QProcess *p : m_activeProcesses)
    {
        if (p)
            p->kill(); // kill 触发的 finished 回调经 m_cancelled 早退
    }
    m_activeProcesses.clear();

    m_pendingToolCalls = QJsonArray();
    m_toolResultsReady = QJsonArray();
    m_pendingPermissionCall = QJsonObject();
    m_awaitingPermission = false;
}
