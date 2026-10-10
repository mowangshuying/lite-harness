// 权限门：bash 硬拒绝黑名单（lcc fddb23e G4 单源）、破坏性命令升级判定、ASK 询问规则、
// 用户裁决续跑。黑名单与 ASK 前缀经 AgentLoopInternal.h 供工具链/钩子链/子代理共用。

#include "AgentLoop.h"

#include "AgentLoopInternal.h"
#include "SubAgent.h"
#include "ToolNames.h"

#include <QJsonDocument>
#include <QRegularExpression>

// --- 跨编译单元共享的内部工具（声明见 AgentLoopInternal.h，定义归属见该头注释）---
namespace AgentLoopDetail
{
// bash 危险命令硬禁止列表（lcc fddb23e G4 单源化）：合并原执行层黑名单与权限门
// DENY_LIST 两张同源异化表为唯一事实源，条目与顺序
// 逐字 = lcc permission.py:8 DENY_LIST。命中即拒绝，不询问；供权限门 checkDenyList、
// 主循环 executeBashAsync 与子代理（经 AgentLoopDetail::bashDenyList）三方共用。
// 既有匹配语义偏差保留登记：主仓为大小写不敏感 contains，lcc 为区分大小写 in——仅数据单源
const QStringList &bashDenyList()
{
    static const QStringList list = {
        // --- lcc 原表：条目与顺序逐字保留，维持移植 parity（lcc permission.py:8）---
        QStringLiteral("rm -rf /"),
        QStringLiteral("sudo"),
        QStringLiteral("shutdown"),
        QStringLiteral("reboot"),
        QStringLiteral("mkfs"),
        QStringLiteral("dd if="),
        QStringLiteral("> /dev/"),
        // --- lite 专属追加（有意偏离 lcc，理由即本注释）---
        // 宿主是 powershell.exe -Command（见 BashRunner 启动段），上面七条全是 Unix 词，
        // 在 Windows 上近乎装饰：`> /dev/` 永不命中，`rm -rf  /`（双空格）与 `rm -rf ~`
        // 绕过精确子串，而真正致命的 Format-Volume/diskpart/-EncodedCommand 一条都没有。
        // 追加判据 = 「PowerShell 下真能造成不可逆破坏」且「大小写不敏感子串匹配误伤率极低」。
        // 删除类动词（rm/del/Remove-Item）刻意不进硬拒表——合法清理太常见，仍由 ASK 层的
        // containsDestructiveCommand 升级询问兜住。
        // 本表由 AgentLoopDetail::bashDenyList 单源，前台/后台/子代理/队友四处共用；
        // 后台分支虽跳过 executeBashAsync 内的同步短路，但 PreToolUse 钩子的 checkDenyList
        // 始终执行（见 AgentLoopHooks.cpp「拒绝先于用户意志」），故追加项对后台同样生效。
        QStringLiteral("rm -rf"),          // 覆盖 rm -rf / 的空白与目标变体（~、双空格、C:\ 路径）
        QStringLiteral("format-volume"),   // 格式化卷
        QStringLiteral("diskpart"),        // 分区操作
        QStringLiteral("vssadmin"),        // 删卷影副本（勒索软件标准步骤）
        QStringLiteral("cipher /w"),       // 空闲空间擦除
        QStringLiteral("reg delete"),      // 删注册表键
        QStringLiteral("-EncodedCommand"), // 混淆载荷入口（短写 -enc 易误伤 --encoding，不收）
        QStringLiteral("Invoke-Expression"),
    };
    return list;
}

// PreToolUse 钩子的“需询问”返回协议前缀（C++ 移植约定，有意偏差）：
// lcc 的 permission 钩子内部同步 input() 询问后直接返回拦截文本或 None；
// GUI 无阻塞 stdin，钩子改为携带 "ASK:<reason>" 返回，由 executeTool 识别后
// 发 permissionRequired 异步挂起（s03 机制，UI 契约零改动）。
// 不带该前缀的非空返回值一律视为硬拦截文本（回填为 tool_result）。
// 现有拦截文案（"Blocked: ..."）不以 "ASK:" 开头，两路径无冲突。
const QString &askPrefix()
{
    static const QString prefix = QStringLiteral("ASK:");
    return prefix;
}
} // namespace AgentLoopDetail

namespace {
// 破坏性命令词正则（lcc s03 DESTRUCTIVE_COMMAND_WORD，防绕过升级）：
// (?i) 忽略大小写；(?:^|[;&|()\n{}"'`]) 匹配串首或分隔符/花括号/引号（覆盖 powershell -Command "..." 与 & {...} 嵌套写法）；
// (?:rm|del|erase|ri|rmdir|rd|Remove-Item) 各类删除命令及别名；(?=\s|$|[;&|(){}]) 词尾断言防 "rms" 类前缀误伤。
// Qt6 PCRE 原生支持 (?i) 与 lookahead；反引号在原始字符串中无需转义
bool containsDestructiveCommand(const QString &command)
{
    static const QRegularExpression re(QStringLiteral(
        R"RE((?i)(?:^|[;&|()\n{}"'`])\s*(?:rm|del|erase|ri|rmdir|rd|Remove-Item)(?=\s|$|[;&|(){}]))RE"));
    return re.match(command).hasMatch();
}
} // namespace

QString AgentLoop::checkDenyList(const QString &command)
{
    // 子串匹配，按列表顺序取第一个命中项（对齐 lcc check_deny_list；
    // 大小写不敏感与仓库既有 bash 黑名单风格一致，较 lcc 的大小写敏感更严格——Windows 命令名本就不区分大小写）
    for (const QString &pattern : AgentLoopDetail::bashDenyList()) // lcc fddb23e 单源列表（G4）
    {
        if (command.contains(pattern, Qt::CaseInsensitive))
        {
            // lcc permission.py:42 log_error(reason) 转译：拒绝事件补 [permission] tag 控制台日志
            //（级别取 qWarning，lcc 走 stderr 通道——登记偏差）；返回值文案不动
            qWarning().noquote() << QStringLiteral("[permission] Permission denied by deny list");
            return QStringLiteral("Blocked: %1 is on the deny list").arg(pattern);
        }
    }
    return QString();
}

QString AgentLoop::checkPermissionRules(const QString &workDir, const QString &toolName,
                                        const QJsonObject &args)
{
    // 规则 1（lcc PERMISSION_RULES）：read/write/edit_file 的 path 逃逸工作区。
    // lcc 用未归一化的拼接判定，这里复用 safePathIn 的越界检测结果，语义一致
    // （s06 起以显式 workDir 参数为准：子代理共用同一逻辑、各查各的沙箱根）
    if (toolName == ToolNames::READ_FILE || toolName == ToolNames::WRITE_FILE
        || toolName == ToolNames::EDIT_FILE)
    {
        QString err;
        if (safePathIn(workDir, args.value(QStringLiteral("path")).toString(), &err).isEmpty())
            return QStringLiteral("Writing outside workspace"); // 新增此类 reason 须同步 PermissionCard::tr 映射表（F7）
        return QString();
    }

    // 规则 2：bash 命中破坏性命令词正则，或包含关键子串（子串部分区分大小写，逐字对齐 lcc）
    if (toolName == ToolNames::BASH)
    {
        const QString command = args.value(QStringLiteral("command")).toString();
        if (containsDestructiveCommand(command) || command.contains(QStringLiteral("rm "))
            || command.contains(QStringLiteral("> /etc/")) || command.contains(QStringLiteral("chmod 777")))
            return QStringLiteral("Potentially destructive command"); // 同上：须同步 PermissionCard reason 映射表（F7）
    }

    // 其余工具（glob 等）无询问规则
    return QString();
}

void AgentLoop::resolvePermission(bool allow)
{
    // UI 收到 permissionRequired 后回传裁决（宿主询问与子代理转发的询问共用本入口，
    // 串行队列保证同一时刻至多一方等待）。lcc s06：路由到当前挂起方；无待决询问时忽略
    if (m_awaitingPermission)
    {
        const QJsonObject toolCall = m_pendingPermissionCall;
        m_pendingPermissionCall = QJsonObject();
        m_awaitingPermission = false;

        // stop() 已清理状态的话此处兜底：不再续跑工具链
        if (!m_running)
            return;

        if (!allow)
        {
            // 拒绝：照常回填 "Permission denied" 并发 toolOutputReady（UI 可展示被拒），随后继续队列。
            // 不触发 PostToolUse——lcc 中用户拒绝即 continue，handler 未运行（s04 语义）
            const QJsonObject function = toolCall.value(QStringLiteral("function")).toObject();
            const QString toolName = function.value(QStringLiteral("name")).toString();
            const QJsonObject args = QJsonDocument::fromJson(
                function.value(QStringLiteral("arguments")).toString().toUtf8()).object();
            onToolFinished(toolCall, toolName, AgentLoopDetail::toolSummary(toolName, args), QStringLiteral("Permission denied"));
            return;
        }

        // 允许：该 toolCall 重新走完整执行链（permission 钩子在 permissionGranted=true 时内部短路，
        // 不再二次询问；日志等其他 PreToolUse 钩子照常执行，对齐 lcc 批准后继续走链的行为。
        // executeBashAsync 内置黑名单仍生效——双层防御）
        // 效率 P4：批准续跑同走缓存表（与 runNextTool 单源，见 ensureToolHandlers）
        executeTool(toolCall, ensureToolHandlers(), /*permissionGranted = */ true);
        return;
    }

    // 宿主未在询问：若子代理正在等待裁决，把决定转发给它
    if (m_activeSub && m_activeSub->isAwaitingPermission())
        m_activeSub->resolvePermission(allow);
}
