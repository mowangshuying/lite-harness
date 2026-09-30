// 生命周期钩子（lcc s04 HOOKS 注册表的内聚移植）：注册顺序即执行顺序，四个 trigger 首个非空返回短路。
// 返回值约定：空串＝放行；PreToolUse 非空＝硬拦截文本，带 ASK: 前缀＝触发异步询问。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "ToolNames.h"
#include "AgentConstants.h"

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
        qDebug().noquote() << QStringLiteral("[hook] UserPromptSubmit: working in %1").arg(m_workDir);
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

        const QString reason = checkPermissionRules(m_workDir, toolName, args);
        if (!reason.isEmpty())
            return AgentLoopDetail::askPrefix() + reason; // 需询问：异步协议，由 executeTool 挂起队列

        return QString();
    });

    // PreToolUse #2: log_before —— 打印工具名与参数预览（lcc: [hook] name(args_preview)）
    m_preToolUseHooks.append([](const QJsonObject &toolCall, bool) -> QString {
        qDebug().noquote() << QStringLiteral("[hook] %1(%2)")
                                  .arg(AgentLoopDetail::callToolName(toolCall), argsPreview(AgentLoopDetail::callToolArgs(toolCall)));
        return QString();
    });

    // PostToolUse #1: log_after —— 打印工具调用信息与输出（lcc 原文案；
    // lcc 在此再次打印 tool_use 行，与 log_before 有意重复，原样保留）
    m_postToolUseHooks.append([](const QJsonObject &toolCall, const QString &output) -> QString {
        const QString toolName = AgentLoopDetail::callToolName(toolCall);
        qDebug().noquote() << QStringLiteral("[hook] tool_use: %1 - %2")
                                  .arg(toolName, toolUseInfo(toolName, AgentLoopDetail::callToolArgs(toolCall)));
        qDebug().noquote() << QStringLiteral("[hook] tool_result:\n%1").arg(output);
        return QString();
    });

    // PostToolUse #2: large_output —— 超长输出提醒（lcc 阈值 100000 字符）。
    // 注：本实现中 bash/read_file 输出在 handler 内已先行截断到 AgentConst::kOutputCharLimit，
    // 钩子实际难以触发，与 lcc 现状一致（lcc 的 run_bash 同样先行截断），保留以对齐结构
    m_postToolUseHooks.append([](const QJsonObject &toolCall, const QString &output) -> QString {
        if (output.size() > AgentConst::kLargeOutputThreshold)
            qDebug().noquote() << QStringLiteral("[hook] Large output from %1: %2 chars")
                                      .arg(AgentLoopDetail::callToolName(toolCall)).arg(output.size());
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
        qDebug().noquote() << QStringLiteral("[hook] Stop: session used %1 tool calls").arg(toolCount);
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
