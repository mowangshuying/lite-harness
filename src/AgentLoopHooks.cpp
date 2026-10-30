// 生命周期钩子（lcc s04 HOOKS 注册表的内聚移植）：注册顺序即执行顺序，四个 trigger 首个非空返回短路。
// 返回值约定：空串＝放行；PreToolUse 非空＝硬拦截文本，带 ASK: 前缀＝触发异步询问。

#include "AgentLoop.h"
#include "LiteLog.h"

#include "AgentConstants.h" // 审批开关 permissionApproval 单点取值（s17.2）
#include "AgentLoopInternal.h"
#include "ToolNames.h"
#include "AgentRecovery.h"

#include <QDebug>

namespace {
// log_before 钩子的参数预览（对应 lcc str(list(block.input.values())[:2])[:60]）：
// 取前两个参数值拼为 "[v1, v2]" 后截 60 字符。
// 偏差：QJsonObject 按键名字典序遍历（Python dict 为文档插入序）；非字符串值经 QVariant 转文本
QString argsPreview(const QJsonObject &args)
{
    QStringList head;
    for (auto it = args.constBegin(); it != args.constEnd() && head.size() < 2; ++it)
        head.append(it.value().toVariant().toString());
    return (QStringLiteral("[") + head.join(QStringLiteral(", ")) + QStringLiteral("]")).left(60);
}

// log_after 钩子的工具参数描述（逐字对应 lcc log_after_use_tool_hook 的 info 分支文案）。
// 偏差：lcc 用 block.input[key] 直接取值（缺 key 会 KeyError），此处 .toString() 缺省为空串
QString toolUseInfo(const QString &toolName, const QJsonObject &args)
{
    if (toolName == ToolNames::BASH)
        return QStringLiteral("command: ") + args.value(QStringLiteral("command")).toString();
    if (toolName == ToolNames::READ_FILE || toolName == ToolNames::WRITE_FILE
        || toolName == ToolNames::EDIT_FILE)
        return QStringLiteral("path: ") + args.value(QStringLiteral("path")).toString();
    if (toolName == ToolNames::GLOB)
        return QStringLiteral("pattern: ") + args.value(QStringLiteral("pattern")).toString();
    if (toolName == ToolNames::TODO_WRITE)
        return QStringLiteral("update task list"); // lcc s06 原文（hooks.py）已去掉末尾冒号
    if (toolName == ToolNames::TASK)
        // lcc 原文为 f"task: {block.input.get('prompt','')}" 不截断；task prompt 可能很长，
        // 为避免日志刷屏截 60 字符（裁决项，有意偏离 lcc）
        return QStringLiteral("task: ")
            + args.value(QStringLiteral("prompt")).toString().left(60);
    return QString();
}
} // namespace

void AgentLoop::registerBuiltinHooks()
{
    // UserPromptSubmit: context_inject —— 打印会话工作目录（lcc 用 Path.cwd()，此处对应 m_workDir）。
    // （lcc 终态已修正 "UserPromptSubmit" 拼写，同步跟进）
    m_userPromptSubmitHooks.append([this](const QString &) -> QString {
        LOG_DEBUG << QStringLiteral("[hook] UserPromptSubmit: working in %1").arg(m_workDir);
        return QString();
    });

    // PreToolUse #1: permission —— s03 的 checkDenyList / checkPermissionRules 检查逻辑原样移入
    // 钩子（文案逐字不变）。permissionGranted=true（批准后续跑）只跳过询问规则；
    // deny 列表检查始终执行——拒绝先于用户意志，且防御队列重放/迟到的续跑路径
    m_preToolUseHooks.append([this](const QJsonObject &toolCall, bool permissionGranted) -> QString {
        const QString toolName = AgentLoopDetail::callToolName(toolCall);
        const QJsonObject args = AgentLoopDetail::callToolArgs(toolCall);

        if (toolName == ToolNames::BASH)
        {
            const QString blocked = checkDenyList(args.value(QStringLiteral("command")).toString());
            if (!blocked.isEmpty())
                return blocked; // 硬拒绝：直接作为拦截文本回填
        }

        if (permissionGranted)
            return QString(); // 已批准：跳过询问，日志等其余钩子照常执行

        // s17.2 审批总开关（settings.ini 键 permissionApproval，单点取值
        // AgentConst::permissionApprovalEnabled()）：关闭时下面三道「询问类」门（ASK 规则 /
        // mcp__ confirm / workflow confirm）一律放行——不出卡、不挂队列，异步回合也不再
        // fail-closed 返拒串（压根不需要审批，就不存在「无人可问」的困境）。
        // 上方 bash 硬拒绝黑名单与 workflow 的 deny 态是定值硬拒、不是审批，不受本开关影响。
        // 每次工具调用现取（经 AppSettings::value() 读缓存），故设置页改完下一个工具调用即生效。
        const bool approvalNeeded = AgentConst::permissionApprovalEnabled();

        // 围栏根经 sandboxRoots(m_workDir) 现取，与主循环文件四件套同源（见 mainToolHandlers）：
        // 门与工具必须同口径，否则工具放行、门弹卡——存储根统一后会话 .temp 已不在 workDir 之下。
        const QString reason = approvalNeeded
            ? checkPermissionRules(sandboxRoots(m_workDir), toolName, args)
            : QString();
        if (!reason.isEmpty())
        {
            // lcc s15 af26862 异步回合 fail-closed：无人值守回合（wake/bg/cron）不得挂在永久审批卡上，
            // 返回定值长拒串（无 ASK: 前缀 → 走 executeTool 既有硬拒回填通道，不挂起队列、不出卡）。
            // 逐字串单源 RecoveryRules::asyncApprovalDenied()。deny 表硬拒顺序在最前不受影响。
            if (m_asyncTurn)
                return RecoveryRules::asyncApprovalDenied();
            return AgentLoopDetail::askPrefix() + reason; // 需询问：异步协议，由 executeTool 挂起队列
        }

        // s14: MCP 策略门（lcc s14 7b4ac1c permission.py——置于 deny 链与 check_rules 之后；
        // mcp__ 名不命中前两者天然互斥。fail-closed：宿主表未登记 allow 一律询问）。
        // permissionGranted 短路的续跑分支在上方已 return，批准重入时直接放行，不动此顺序。
        if (approvalNeeded && toolName.startsWith(ToolNames::MCP_PREFIX)
            && m_mcp.toolPolicy(toolName) != QLatin1String("allow"))
        {
            // lcc s15 af26862 异步回合 fail-closed：MCP confirm 门同样不得挂在审批卡上，
            // 与上方 ASK 门同口径返回定值长拒串（无 ASK: 前缀 → 既有硬拒回填通道）
            if (m_asyncTurn)
                return RecoveryRules::asyncApprovalDenied();
            return AgentLoopDetail::askPrefix() + QStringLiteral("MCP tool policy requires confirmation");
        }

        // s16: Workflow 策略门（lcc s16 42a555d permission.py s16 三态）：按 args.name 查
        // Lane A 策略表——deny 定值硬拒 / 非 allow 询问（fail-closed：未登记名与空名皆 confirm）/
        // allow 落末放行。异步回合与 ASK 门、MCP 门同口径改返定值长拒串；granted 短路续跑分支在
        // 上方已 return，批准重入时直接放行，不动此顺序。
        if (toolName == ToolNames::WORKFLOW)
        {
            const QString policy = m_workflow.policyFor(args.value(QStringLiteral("name")).toString());
            if (policy == QLatin1String("deny"))
                return QStringLiteral("Permission denied by host policy"); // 定值硬拒回填通道
            if (approvalNeeded && policy != QLatin1String("allow"))
            {
                if (m_asyncTurn)
                    return RecoveryRules::asyncApprovalDenied();
                return AgentLoopDetail::askPrefix() + QStringLiteral("Workflow policy requires confirmation");
            }
        }

        return QString();
    });

    // PreToolUse #2: log_before —— 打印工具名与参数预览（lcc: [hook] name(args_preview)）
    m_preToolUseHooks.append([](const QJsonObject &toolCall, bool) -> QString {
        LOG_DEBUG << QStringLiteral("[hook] %1(%2)")
                                    .arg(AgentLoopDetail::callToolName(toolCall), argsPreview(AgentLoopDetail::callToolArgs(toolCall)));
        return QString();
    });

    // PostToolUse: log_after —— 打印工具调用信息与输出（lcc 原文案；
    // lcc 在此再次打印 tool_use 行，与 log_before 有意重复，原样保留）
    m_postToolUseHooks.append([](const QJsonObject &toolCall, const QString &output) -> QString {
        const QString toolName = AgentLoopDetail::callToolName(toolCall);
        LOG_DEBUG << QStringLiteral("[hook] tool_use: %1 - %2")
                                    .arg(toolName, toolUseInfo(toolName, AgentLoopDetail::callToolArgs(toolCall)));
        // 完整工具输出可达 kOutputCharLimit（50000 字符），QString::arg(output) 的格式化与内存分配
        // 在 Release 下同样发生、且本钩子跑在主线程——每个工具调用无条件打印全文是热点开销。
        // 默认只打上面的 tool_use 简报行，仅显式设置 LITE_VERBOSE_TOOLS 环境变量时才输出全文（调试用）。
        static const bool verboseTools = qEnvironmentVariableIsSet("LITE_VERBOSE_TOOLS");
        if (verboseTools)
            LOG_DEBUG << QStringLiteral("[hook] tool_result:\n%1").arg(output);
        return QString();
    });

    // Stop: summary —— 统计整场会话的工具调用次数并打印。
    // lcc 扫描全量消息中的 tool_result 块；本实现为 OpenAI 格式，等价于统计 role=="tool"
    // 的历史消息条数（含被拒/被拦截的回填项，lcc 同样计入），故直接扫 m_messages 而非成员计数
    m_stopHooks.append([this]() -> QString {
        int toolCount = 0;
        for (const QJsonObject &msg : m_messages)
        {
            if (msg.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
                ++toolCount;
        }
        LOG_DEBUG << QStringLiteral("[hook] Stop: session used %1 tool calls").arg(toolCount);
        return QString();
    });
}

QString AgentLoop::triggerUserPromptSubmitHooks(const QString &prompt)
{
    for (const auto &hook : m_userPromptSubmitHooks)
    {
        const QString result = hook(prompt);
        if (!result.isEmpty())
            return result; // 首个非空即短路（lcc: if result is not None: return result）
    }
    return QString();
}

QString AgentLoop::triggerPreToolUseHooks(const QJsonObject &toolCall, bool permissionGranted)
{
    for (const auto &hook : m_preToolUseHooks)
    {
        const QString result = hook(toolCall, permissionGranted);
        if (!result.isEmpty())
            return result;
    }
    return QString();
}

QString AgentLoop::triggerPostToolUseHooks(const QJsonObject &toolCall, const QString &output)
{
    for (const auto &hook : m_postToolUseHooks)
    {
        const QString result = hook(toolCall, output);
        if (!result.isEmpty())
            return result;
    }
    return QString();
}

QString AgentLoop::triggerStopHooks()
{
    for (const auto &hook : m_stopHooks)
    {
        const QString result = hook();
        if (!result.isEmpty())
            return result;
    }
    return QString();
}
