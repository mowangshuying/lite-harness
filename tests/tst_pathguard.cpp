// tst_pathguard.cpp —— 沙箱围栏多根判定直测（src/AgentPathGuard.h::isWithinSandboxRoots）
// 被测：根集合包含性判定（分隔符边界 / 大小写口径 / 空根与空集合的 fail-closed）。
//
// 本套件为什么必须存在（不是凑覆盖率）：这条判定是**安全边界**，判错的两种后果都没有信号。
//   · 判松（该拒的放行）：模型给一个 workDir 之外的绝对路径就能读写用户机器上任意文件，
//     工具返回成功、审批卡不弹，事后无从察觉；
//   · 判紧（该放的拒掉）：存储根统一到 exe 同级 config/ 后，会话 `.temp` 不再是 workDir 子树，
//     单根口径会把模型自己的临时目录判成越狱——主循环每次写临时文件弹一张审批卡，
//     队友侧更糟：队友无交互批准通道，越界直接折成硬拒文本，表现为「队友不会写临时文件」。
// 两种都是「跑得通但结果不对」，冒烟运行看不全，只能靠断言钉死。
//
// 判定内核为什么在本头而不是 AgentLoop::safePathIn：AgentLoop 带 Q_OBJECT，其 TU 依赖
// moc + QOpenAi + FluentUI，测试目标只链 Qt6::Core，留在类内就是 L4 零覆盖区。
// 外提后本头可直测，类内只剩「取规范路径 + 转调本口」的薄壳。
//
// 覆盖边界（勿高估）：本套件测的是**词法包含判定**。它不证明 safePathIn 的
// canonicalFilePath 归一（符号链接/junction 消解）正确——那要求文件真实存在，属夹具域；
// 也不证明各调用点都传了完整根集合（接线，靠 grep 收口 + 冒烟）。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "AgentPathGuard.h"

#include <QString>
#include <QStringList>

namespace {

using TestHarness::check; // 本套件断言密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// 两个固定根：工作区（代码沙箱）与会话数据根（统一锚后落在 config/ 之下，不在 workDir 子树里）。
// 形态取自真实布局：workDir 是被打开的项目目录，会话根是 <exe>/../config/.lite-harness/sessions/<id>。
const char *kWork = "D:/proj/work";
const char *kSession = "D:/proj/approot/config/.lite-harness/sessions/ce0b7305";

} // namespace

int tst_pathguard()
{
    const QString work(QString::fromLatin1(kWork));
    const QString session(QString::fromLatin1(kSession));
    const QStringList both{work, session};

    // ---- ① 单根语义：改造前 safePathIn 的全部行为必须在只传一个根时逐字保持 ----
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/src/App.cpp"), both),
          "file under workspace root is allowed");
    check(AgentPathGuard::isWithinSandboxRoots(work, both),
          "the root itself is allowed (safePathIn 原语义：absPath == root 放行)");
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/other/x.txt"), both),
          "path outside every root is rejected");

    // ---- ② 分隔符边界：兄弟目录不得被当成子树 ----
    // 这是本判定最经典也最致命的错法：纯 startsWith("D:/proj/work") 会放行 "D:/proj/work-evil"，
    // 而 "work-evil" 是完全另一个目录。改造前就有这条护栏（注释点名），统一锚后不能丢。
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work-evil/x.txt"), both),
          "sibling dir sharing the root's name prefix is rejected");
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/workX"), both),
          "root-plus-one-char path is rejected");
    // 边界字符必须是分隔符本身，不是「任何字符」
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/"), both),
          "root with trailing separator is allowed (same directory)");

    // ---- ③ 第二个根：会话数据根（本次改动的全部理由）----
    // `.temp` 是 system prompt 明确引导模型写入的目录，统一锚后它落在 config/ 之下。
    check(AgentPathGuard::isWithinSandboxRoots(
              QStringLiteral("D:/proj/approot/config/.lite-harness/sessions/ce0b7305/.temp/a.txt"), both),
          "session .temp is allowed once the session root joins the fence");
    check(AgentPathGuard::isWithinSandboxRoots(session, both),
          "the session root itself is allowed");
    // 队友 worktree 同理：.worktrees 挂在会话根下，不再是 workDir 子树。
    check(AgentPathGuard::isWithinSandboxRoots(
              QStringLiteral("D:/proj/approot/config/.lite-harness/sessions/ce0b7305/.worktrees/demo-wt/x"),
              both),
          "teammate worktree under the session root is allowed");
    // 但**兄弟会话**不得互相抵达——这是「只加自己那一个会话根」而非「加整个 sessions 根」的理由。
    check(!AgentPathGuard::isWithinSandboxRoots(
              QStringLiteral("D:/proj/approot/config/.lite-harness/sessions/OTHERID/history.json"), both),
          "another session's directory is NOT reachable (per-session root, not the whole store)");
    // 单根口径下同一必须被拒——这条断言就是「为什么非改不可」的证据，不是对旧行为的兼容测试。
    const QStringList workOnly{work};
    check(!AgentPathGuard::isWithinSandboxRoots(
              QStringLiteral("D:/proj/approot/config/.lite-harness/sessions/ce0b7305/.temp/a.txt"), workOnly),
          "session .temp is rejected by the legacy single-root fence (the bug being fixed)");

    // ---- ④ 大小写口径：Windows 文件系统不敏感，判定必须同样不敏感 ----
    // 判反方向的代价不对称：大小写敏感会把 "D:/PROJ/WORK/a.cpp" 判成越狱（模型完全可能给出
    // 全大写盘符），而放宽只在本机文件系统语义内等价。
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/PROJ/WORK/src/App.cpp"), both),
          "uppercase path against lowercase root is allowed");
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("d:/proj/work/src/App.cpp"), both),
          "lowercase-drive path is allowed");
    check(AgentPathGuard::isWithinSandboxRoots(work.toUpper(), both),
          "root compared case-insensitively against itself");
    // 但边界字符不能因为大小写放宽而失守
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/PROJ/WORK-evil/x"), both),
          "case-insensitive放宽不越过分隔符边界");

    // ---- ⑤ fail-closed：空集合与空根 ----
    // 「没配围栏」绝不能滑成「围栏全开」。空根尤其阴险：startsWith("") 恒真，
    // 一个混进集合的空串会让整条判定放行一切路径。
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/a.cpp"), QStringList()),
          "empty root set rejects everything (fail-closed)");
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/elsewhere/a.cpp"),
                                                QStringList{QString()}),
          "a lone empty root rejects (empty root must not match everything)");
    const QStringList withEmpty{QString(), work};
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/a.cpp"), withEmpty),
          "empty root is skipped, the valid root still admits");
    check(!AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/elsewhere/a.cpp"), withEmpty),
          "empty root in the set does not admit outside paths");

    // ---- ⑥ 根入参归一：调用方给反斜杠/尾斜杠根，判定不得静默失配 ----
    // safePathIn 传进来的根形态不止一种来源（config.json 手改面、QDir::currentPath、
    // applicationDirPath 反推），本头自行 cleanPath 归一，否则「能读到的根」与「判得出的根」分叉。
    const QStringList backslashRoots{QStringLiteral("D:\\proj\\work"), session};
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/a.cpp"), backslashRoots),
          "backslash-form root still admits forward-slash paths");
    const QStringList trailingRoots{QStringLiteral("D:/proj/work/"), session};
    check(AgentPathGuard::isWithinSandboxRoots(QStringLiteral("D:/proj/work/sub/a.cpp"), trailingRoots),
          "root with trailing separator does not double the boundary");

    return TestHarness::failCount() == 0 ? 0 : 1;
}
