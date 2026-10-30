#pragma once

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * AgentPathGuard —— 路径/名字守护纯头（header-only，仅 QtCore，inline 单源）。
 *
 * 收口理由（gate① M7）：lcc 把 is_valid_agent_name 设为模块级公共 API、RESERVED 常量注释
 * 「供 Lane B/C/D 复用」；lite 原实现藏在 MessageBus.cpp 匿名命名空间，WorktreeManager（fix-3）
 * 与 AgentTeamsManager（fix-4）无从复用——各自重抄正则必然漂移，产出「能 spawn 却收不到信」
 * 的哑队友。两个名字正则各用各的单源（邮箱名与本头；worktree 名 `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`
 * 允许 '.'，字符集不同，由 fix-3 另立单源，**不得共用本头 helper**）。
 */
namespace AgentPathGuard {

// lcc VALID_AGENT_NAME（message_bus.py:14）：fullmatch 防 "abc/../evil" 前缀合法后缀越狱。
// 字符集不含 '.' 与 '/'，故 '.'/'..'/'a/b' 一律在①关即拒。
// 供 MessageBus 三关①与 Lane B/C/D（spawn 名字校验、邮箱名判定）复用——spawn/send 同源，
// 防「可 spawn 但收不到信」。邮箱名正则大小写敏感且字符集白名单，故此处不做 casefold
// （casefold 语义只属保留名比较，见 AgentConst::isReservedTeammateName）。
inline bool isValidAgentName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    const QRegularExpressionMatch m = re.match(name);
    // python fullmatch 等价：锚定命中 + 捕获段恰覆盖全串（规避 PCRE $ 允许末尾换行的怪癖，
    // 与 isTaskIdFull 同款纪律）
    return m.hasMatch() && m.capturedStart(0) == 0 && m.capturedLength(0) == name.size();
}

// python Path.is_relative_to 的词法等价：入参均已 QDir::cleanPath 归一（正斜杠、无冗余段）。
// 用前缀比较而非 canonicalFilePath——后者要求文件存在且解析符号链接，而 send 时邮箱/目录尚未
// 落地。名字正则已禁路径分隔符，sessionRoot 为宿主可信值，词法包含校验足以拦越狱（登记偏差：
// lcc resolve() 另做符号链接归一，此处仅词法，纵深防御仍覆盖任务书三重校验）。
// 供 MessageBus 三关②③与 WorktreeManager 三关（lcc worktree_manager.py:58-65，含
// 「path==root 也拒」易漏关）共用；只做词法，符号链接/junction 复校见 isWithinPathCanonical。
inline bool isWithinPath(const QString &child, const QString &parent)
{
    if (child == parent)
        return true;
    if (parent.endsWith(QLatin1Char('/')))
        return child.startsWith(parent);
    return child.startsWith(parent + QLatin1Char('/'));
}

// m1/P3 预留件（safePathIn 围栏根随 cwd 议程，本轮不接入任何调用点）：parent 目录存在时用
// canonicalFilePath 复校——解析符号链接/junction，补词法口径的纵深缺口（模型可经 bash
// mklink /J 把词法包含绕开）；parent 不存在时回落词法 isWithinPath（与 lcc resolve() 不同，
// canonicalFilePath 对不存在路径返回空串，无从复校）。child 不存在不影响复校（解析的是 parent）。
// 约定：入参为 cleanPath 归一后的词法路径串；parent 存在但 canonical 解析失败（空串，如权限受限）
// 时按 fail-closed 拒绝。
inline bool isWithinPathCanonical(const QString &child, const QString &parent)
{
    const QFileInfo parentInfo(parent);
    if (!parentInfo.exists())
        return isWithinPath(child, parent); // 目录未落地：回落词法（send 首写时邮箱目录尚不存在）
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty())
        return false; // fail-closed：目录存在却解析不出规范路径，不放行
    return isWithinPath(QDir::cleanPath(child), canonicalParent);
}

// 沙箱围栏多根判定——`AgentLoop::safePathIn` 的判定内核，抽到本头是为了能进 ctest 直测
// （AgentLoop 带 Q_OBJECT，其 TU 依赖 moc + QOpenAi + FluentUI，测试目标只链 Qt6::Core，
// 原样留在类内就是 L4 零覆盖区；判定外提后类内只剩「取规范路径 + 转调本口」）。
//
// 为什么是**根集合**而不是单根：存储根统一到 exe 同级 config/ 后，会话数据根
// （含 system prompt 引导写入的 `.temp`）不再落在 workDir 之内，单根围栏会把模型自己的
// 临时目录判成越狱（每次写临时文件都弹审批卡、或直接 Error: Path escapes workspace）。
// 故围栏 = { workDir, 会话数据根 }。
//
// 口径与改造前的 safePathIn 逐字保持：入参必须是已归一的词法路径（cleanPath，或存在时的
// canonicalFilePath），本口不再自行归一（谁归一、归一到哪一层，是调用方的契约，两处各归
// 一次会得出不同形态）；前缀比较带目录分隔符边界（防 "D:/work" 误判 "D:/work-evil"）；
// Windows 文件系统大小写不敏感，故比较忽略大小写。
// fail-closed：roots 为空一律拒——「没配围栏」不等于「围栏全开」。空串根同样跳过，
// 否则空串会让 startsWith("") 恒真、把整条判定变成放行一切。
inline bool isWithinSandboxRoots(const QString &path, const QStringList &roots)
{
    for (const QString &rawRoot : roots)
    {
        if (rawRoot.isEmpty())
            continue;
        const QString root = QDir::cleanPath(rawRoot);
        if (root.isEmpty())
            continue;
        if (path.compare(root, Qt::CaseInsensitive) == 0
            || path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace AgentPathGuard
