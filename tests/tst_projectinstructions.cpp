// tst_projectinstructions.cpp —— 项目指令文件（AGENTS.md）判据纯头直测（src/ProjectInstructions.h）
// 被测：两级候选顺序 / 命中即止择一 / 缓存签名 / 注入段拼装与截断。
//
// 本套件为什么必须存在（不是凑覆盖率）：这四条判据全部「跑得出但喂错料」——
//   · 候选顺序写反（全局压项目）：用户改仓库里的 AGENTS.md 完全不生效，且没有任何报错，
//     表现是「规则写了模型不看」；本仓唯一的判据就是顺序，只能靠断言钉死。
//   · 命中即止写成「两个都读」：两段指令互相覆盖、token 双份，没人知道哪条赢。
//   · 空正文仍拼段头：注入块里出现一个没有内容的指令段，比不注入更容易诱发臆测。
//   · 截断静默吞尾：预算之后的规则凭空消失且无标记，模型会自信宣称「已遵循项目全部约定」。
// 四条在冒烟里都看不出来（请求照样发、回复照样像模像样），只能逐条断言。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT）：存在性判定经 std::function 注入桩，路径派生经
// LitePaths 的纯口 *For —— 本头零 IO、零 QCoreApplication 依赖，任何环境全量执行。
// 读盘接线（QFile/QFileInfo/缓存命中）不在本套件口径内，验证手段是编译期等价 + 冒烟。

#include "TestHarness.h"

#include "ProjectInstructions.h"
#include "AgentConstants.h"

#include <QString>
#include <QStringList>

namespace {

using TestHarness::check; // 本套件断言密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// 存在性桩：命中集合由测试给定，同时记录探测次数与探测顺序——
// 「命中即止」与「顺序不被重排」这两条判据只能靠调用序列观测，返回值看不出来。
struct Probe {
    QStringList hits;
    QStringList seen;
    int calls = 0;
    bool test(const QString &path)
    {
        ++calls;
        seen.append(path);
        return hits.contains(path);
    }
};

const QString kWorkDir = QStringLiteral("D:/proj/repo");
const QString kConfigRoot = QStringLiteral("D:/pkg/config");
const QString kProjectFile = QStringLiteral("D:/proj/repo/AGENTS.md");
const QString kGlobalFile = QStringLiteral("D:/pkg/config/AGENTS.md");

} // namespace

int tst_projectinstructions()
{
    // 返回本套件失败增量而非裸 0：TestHarness::check 只累全局计数，硬写 return 0 会让
    // main.cpp 的 runSuite 报 fail=0、而全局 failCount 已非零——正是 main 里
    // 「failed != failCount() 视为不通过」那条一致性门禁要抓的形态（本批实测踩过：
    // 两条断言失败时 SUITE 行仍报 fail=0，只有进程退出码诚实）。
    const int failBefore = TestHarness::failCount();
    // ---- ① 候选顺序：工程根在前、存储根在后，互斥回退的两级 ----
    const QStringList both = ProjectInstructions::candidatesFor(kWorkDir, kConfigRoot);
    check(both.size() == 2, "candidatesFor: two levels when both roots given");
    check(both.at(0) == kProjectFile, "candidatesFor: level 0 is <workDir>/AGENTS.md");
    check(both.at(1) == kGlobalFile, "candidatesFor: level 1 is <configRoot>/AGENTS.md");
    // 顺序本身——写反即「全局静默压住项目」，是本头唯一不可让的判据
    check(both.at(0).startsWith(kWorkDir) && both.at(1).startsWith(kConfigRoot),
          "candidatesFor: project root precedes storage root");
    check(!both.at(0).contains(QStringLiteral("..")), "candidatesFor: '..' folded out of level 0");

    // 单根缺位：另一级仍可用，且候选表不掺空串
    const QString onlyProject = kWorkDir + QStringLiteral("/AGENTS.md");
    const QStringList one = ProjectInstructions::candidatesFor(kWorkDir, QString());
    check(one.size() == 1 && one.at(0) == onlyProject,
          "candidatesFor: empty configRoot leaves only the project candidate");
    const QStringList two = ProjectInstructions::candidatesFor(QString(), kConfigRoot);
    check(two.size() == 1 && two.at(0) == kGlobalFile,
          "candidatesFor: empty workDir leaves only the storage candidate");
    // 全空白入参同样被护栏吃掉——空根拼出的 "/AGENTS.md" 在 Windows 解析成驱动器根
    check(ProjectInstructions::candidatesFor(QStringLiteral("   "), kConfigRoot).size() == 1,
          "candidatesFor: whitespace-only workDir is skipped, not joined");
    const QStringList none = ProjectInstructions::candidatesFor(QString(), QString());
    check(none.isEmpty(), "candidatesFor: both roots empty yields no candidate at all");
    check(!none.contains(QString()), "candidatesFor: no empty path ever enters the list");
    // 文件名单源：两级叶子段必须逐字相同，改一处名字就会让另一级静默失效
    check(both.at(0).mid(kWorkDir.size()) == both.at(1).mid(kConfigRoot.size()),
          "candidatesFor: both levels share the same leaf name (single source)");

    // ---- ② 命中即止：第一个存在的候选赢，其后不再探测 ----
    Probe pBoth;
    pBoth.hits = {kProjectFile, kGlobalFile};
    check(ProjectInstructions::selectExisting(
              both, [&pBoth](const QString &p) { return pBoth.test(p); })
              == kProjectFile,
          "selectExisting: project candidate wins when both exist (no merge)");
    check(pBoth.calls == 1, "selectExisting: stops probing after the first hit");

    Probe pGlobalOnly;
    pGlobalOnly.hits = {kGlobalFile};
    check(ProjectInstructions::selectExisting(
              both, [&pGlobalOnly](const QString &p) { return pGlobalOnly.test(p); })
              == kGlobalFile,
          "selectExisting: falls back to storage root when project has none");
    check(pGlobalOnly.calls == 2 && pGlobalOnly.seen.at(0) == kProjectFile
              && pGlobalOnly.seen.at(1) == kGlobalFile,
          "selectExisting: probes in candidate order, project first");

    Probe pNone;
    check(ProjectInstructions::selectExisting(
              both, [&pNone](const QString &p) { return pNone.test(p); })
              .isEmpty(),
          "selectExisting: no hit yields empty path");
    check(pNone.calls == 2, "selectExisting: every candidate probed when none hits");

    Probe pShouldNotRun;
    check(ProjectInstructions::selectExisting(
              QStringList(), [&pShouldNotRun](const QString &p) { return pShouldNotRun.test(p); })
              .isEmpty(),
          "selectExisting: empty candidate list yields empty path");
    check(pShouldNotRun.calls == 0, "selectExisting: empty list probes nothing");
    // 命中即止的语义边界：候选表里重复路径不会让第二次命中被读到（顺序唯一性）
    Probe pDup;
    pDup.hits = {kGlobalFile};
    const QStringList dup = {kGlobalFile, kProjectFile};
    check(ProjectInstructions::selectExisting(
              dup, [&pDup](const QString &p) { return pDup.test(p); })
              == kGlobalFile,
          "selectExisting: order comes from the caller's list, header never re-sorts");

    // ---- ③ 注入段拼装：空段省略、来源可见、正文逐字 ----
    check(ProjectInstructions::formatSection(QString(), QStringLiteral("rules"), 100).isEmpty(),
          "formatSection: empty path yields no section");
    check(ProjectInstructions::formatSection(kProjectFile, QString(), 100).isEmpty(),
          "formatSection: empty text yields no section");
    check(ProjectInstructions::formatSection(kProjectFile, QStringLiteral("  \n\t \n"), 100).isEmpty(),
          "formatSection: whitespace-only text yields no section (trim gate)");

    const QString plain = ProjectInstructions::formatSection(
        kProjectFile, QStringLiteral("Build: cmake --build build"), 100);
    check(plain.startsWith(QStringLiteral("Project instructions (AGENTS.md from ")),
          "formatSection: section opens with the English title (style parity with Skills/Memory)");
    check(plain.contains(kProjectFile), "formatSection: source path is named in the title");
    check(plain.endsWith(QStringLiteral("Build: cmake --build build")),
          "formatSection: body appended verbatim");
    check(!plain.contains(QStringLiteral("characters shown")),
          "formatSection: within budget there is no truncation marker");
    check(!plain.contains(QStringLiteral("<agent_context>")),
          "formatSection: envelope is makeContextInjection's job, not this header's");
    // 多行正文原样保留（AGENTS.md 的段落结构靠换行承载，不能被折叠）
    const QString multiline = ProjectInstructions::formatSection(
        kProjectFile, QStringLiteral("line1\nline2\n\nline4"), 100);
    check(multiline.endsWith(QStringLiteral("line1\nline2\n\nline4")),
          "formatSection: newlines in the body survive untouched");

    // ---- ④ 截断：可见、如实、且只下发前缀 ----
    const QString body = QStringLiteral("0123456789abcdefghij"); // 20 字符
    const QString cut = ProjectInstructions::formatSection(kProjectFile, body, 9);
    check(cut.contains(QStringLiteral("first 9 of 20 characters shown")),
          "formatSection: truncation reports shown/total character counts honestly");
    check(cut.endsWith(QStringLiteral("012345678")),
          "formatSection: only the first budgetChars characters are delivered");
    check(!cut.contains(QStringLiteral("abcdefghij")),
          "formatSection: the over-budget tail is not delivered");
    // 边界：正好等于预算不截断（差一即每轮都挂个假残篇标记）
    const QString exact = ProjectInstructions::formatSection(kProjectFile, body, 20);
    check(!exact.contains(QStringLiteral("characters shown")),
          "formatSection: size == budget is not truncated (off-by-one gate)");
    const QString over = ProjectInstructions::formatSection(kProjectFile, body, 21);
    check(!over.contains(QStringLiteral("characters shown")),
          "formatSection: size < budget is not truncated");
    // 预算 <=0 = 不设预算（全量下发），不是「全丢」
    check(ProjectInstructions::formatSection(kProjectFile, body, 0).endsWith(body),
          "formatSection: zero budget means no cap, not no content");
    check(ProjectInstructions::formatSection(kProjectFile, body, -1).endsWith(body),
          "formatSection: negative budget means no cap, not no content");

    // ---- ⑤ 替换语义：正文与路径里的 '%' 形态串不得被二次展开 ----
    const QString percentBody = QStringLiteral("use %1 and %2 as literals");
    const QString percentSection =
        ProjectInstructions::formatSection(kProjectFile, percentBody, 100);
    check(percentSection.endsWith(percentBody),
          "formatSection: body containing %N is not re-expanded by arg()");
    const QString percentPath = QStringLiteral("D:/100%/repo/AGENTS.md");
    const QString percentPathSection =
        ProjectInstructions::formatSection(percentPath, QStringLiteral("x"), 100);
    check(percentPathSection.contains(percentPath),
          "formatSection: path containing '%' is not re-expanded by arg()");
    // 截断分支单独钉：它需要往标题里塞两个数字，早期写法把 path 也塞进同一条链式
    // .arg(path).arg(N).arg(M)，Qt 每次调用各扫一遍 → 路径里的 "%2" 被第二次 .arg() 吃掉。
    // Windows 目录名允许 '%'，这条不是洁癖。
    const QString cutPercentPath = ProjectInstructions::formatSection(
        QStringLiteral("D:/100%/repo/AGENTS.md"), QStringLiteral("0123456789abcdefghij"), 9);
    check(cutPercentPath.contains(QStringLiteral("D:/100%/repo/AGENTS.md")),
          "formatSection: truncated branch keeps a '%' path intact (no chained arg re-scan)");
    const QString cutPercentBody = ProjectInstructions::formatSection(
        kProjectFile, QStringLiteral("0123456789%1%2abcdefghij"), 14);
    check(cutPercentBody.endsWith(QStringLiteral("0123456789%1%2")),
          "formatSection: truncated branch keeps a body containing %N intact");

    // ---- ⑥ 缓存签名：路径/mtime/size 任一变化即换，稳定输入即同 ----
    const QString sigA = ProjectInstructions::signatureFor(kProjectFile, 1700000000000, 122971);
    const QString sigAagain = ProjectInstructions::signatureFor(kProjectFile, 1700000000000, 122971);
    check(sigA == sigAagain, "signatureFor: identical inputs give an identical signature");
    check(ProjectInstructions::signatureFor(kProjectFile, 1700000000001, 122971) != sigA,
          "signatureFor: mtime change flips the signature");
    check(ProjectInstructions::signatureFor(kProjectFile, 1700000000000, 122972) != sigA,
          "signatureFor: size change flips the signature");
    check(ProjectInstructions::signatureFor(kGlobalFile, 1700000000000, 122971) != sigA,
          "signatureFor: path change flips the signature (workDir switch invalidates)");
    check(sigA.count(QLatin1Char('|')) == 2, "signatureFor: three fields joined by two separators");
    // 同秒改完又改回原大小：路径段仍在，签名不会因两个数字段相同而漏判跨层级切换
    check(ProjectInstructions::signatureFor(kProjectFile, 0, 0)
              != ProjectInstructions::signatureFor(kGlobalFile, 0, 0),
          "signatureFor: zeroed mtime/size still separate the two levels");

    // ---- ⑦ 预算常量契约：非正即「每轮都截成空段」，越界小即「规则永远读不全」----
    check(AgentConst::kProjectInstructionsCharBudget > 0,
          "AgentConst: AGENTS.md char budget is positive");
    check(AgentConst::kProjectInstructionsCharBudget >= 1000,
          "AgentConst: AGENTS.md budget is large enough to carry a real rules preamble");
    // 与预算常量对表：用它跑一次本仓体量级的正文，必须走截断分支而不是静默全量
    const QString big(static_cast<qsizetype>(AgentConst::kProjectInstructionsCharBudget) + 1,
                      QLatin1Char('x'));
    check(ProjectInstructions::formatSection(kProjectFile, big,
                                             AgentConst::kProjectInstructionsCharBudget)
              .contains(QStringLiteral("characters shown")),
          "AgentConst: the registered budget actually truncates an over-sized file");

    return TestHarness::failCount() - failBefore;
}
