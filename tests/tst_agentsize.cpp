// tst_agentsize.cpp —— AGENTS.md 注入窗口闸门（s18.7 文档分层批）。
//
// 为什么必须有这条测试（本轮实测根因，不是假想洁癖）：
//   AGENTS.md 是经 `ProjectInstructions::formatSection` 注入模型的项目指令，注入只取前
//   AgentConst::kProjectInstructionsCharBudget 个码元（src/AgentConstants.h:455，现值 16000），
//   截断点在 src/ProjectInstructions.h:82 的 `text.left(budgetChars)`。本仓 AGENTS.md 搬运前
//   实测 154,145 码元（投递率 10.4%），而 `## 构建` 节那条 CPack 条目**单行即 26,557 码元**、
//   其第 582 码元之后全是逐批次复测数字——整个注入窗口被打包历史吃干：`禁翻` 首现 offset
//   81,093、`tr()` 74,852、`worktree` 43,565，全在窗口外。**纪律写了但模型一条都看不到**，
//   正是 ProjectInstructions.h 头注释自登记的风险「截断静默吞尾」的兑现形态。
//
// 本测试守两条**可判的终态**，不是「文件越短越好」：
//   ① 锚点在窗：会致错的规则必须落在前 budget 个码元之内。只测总长度挡不住「长度没涨但把
//      规则挪去了尾部」，故按锚点判。
//   ② 棘轮：总码元数不得超过登记上限，上限**只准下调、不准上调**——防下一轮再往正文追加
//      一批复测，把规则重新推出窗口。逐批次复测/实跑登记的落点是 docs/agents-ledger.md（不注入）。
//   ③ 搬运不是删除：台账文件必须真的含有那些内容，否则「搬出去」等于把历史记录删了。
//
// 刻意**不**断言「文件已小于预算」：现状仍超预算（约 7.6x），那是已知缺口而非终态。断言一个
// 待修的坏状态会让「真去瘦身」的人当场撞红——判据必须奖励改进，不能把现状焊死。
//
// 为什么进得了 ctest：只读两个文件、只用 QtCore，不碰 AgentLoop/QOpenAi（带 moc + Qt6::Network），
// 符合 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律。仓库根经 CMake 编译期注入
// LITE_REPO_ROOT；取不到或文件缺失时诚实 skip——CI 恒在源码树内构建，多一处 skip 会撞
// LITE_TEST_MAX_SKIP=1 当场暴露，不会静默放行。

#include "TestHarness.h"

#include "AgentConstants.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

#ifndef LITE_REPO_ROOT
#define LITE_REPO_ROOT ""
#endif

namespace {

using TestHarness::check;

// 读文本并按 UTF-16 码元计长——必须与生产同口径：formatSection 比的是 QString::size()
// （UTF-16 码元），不是字节数。中文 3 字节/1 码元，按字节判会得出完全不同的结论。
bool readUtf16(const QString &relPath, QString *out)
{
    const QString root = QString::fromUtf8(LITE_REPO_ROOT);
    if (root.isEmpty())
        return false;
    QFile f(root + QLatin1Char('/') + relPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray raw = f.readAll();
    if (raw.isEmpty())
        return false;
    *out = QString::fromUtf8(raw);
    return true;
}

// 棘轮上限：s18.7 文档分层批搬运后实测 121,531 码元，取整留约 500 余量。
// **要抬高此数必须先真的把内容搬进 docs/agents-ledger.md**，否则本条即红。
// 体量棘轮：本批落地后实测 121,944，封顶 123,000——留约 1K 正常编辑余量，但 1-3K 的一整段
// 复测登记仍会撞线（设紧是刻意的：增长应走 docs/agents-ledger.md，真要涨本文件须显式改此常量、
// 在 diff 里可见）。上一登记值 122,000 只剩 56 码元余量，会让下批任何正常编辑误红。
constexpr qsizetype kAgentsSizeRatchet = 123000;

// 锚点用 UTF-8 字节串写，避免源码里出现需要转义的引号组合（本仓已登记 PS/编码类坑同族防线）。
struct Anchor {
    const char *needle; // UTF-8
    const char *why;    // 漏出窗口会犯什么错
};

static const Anchor kMustBeInWindow[] = {
    { "\xe5\x8f\xaa\xe5\x9c\xa8\xe7\x94\xa8\xe6\x88\xb7\xe6\x9c\xac\xe8\xbd\xae"
      "\xe6\x98\x8e\xe7\xa1\xae\xe8\xa6\x81\xe6\xb1\x82\xe6\x97\xb6\xe6\x89\xa7\xe8\xa1\x8c",
      "release authorization gate -- self-bump at batch close is the registered bad habit" },
    { "## \xe5\x85\xb3\xe9\x94\xae\xe7\xba\xa6\xe5\xae\x9a",
      "key conventions -- single-source discipline and tool-failure folding" },
    { "## \xe5\x8f\x91\xe5\xb8\x83",
      "release section -- tag naming and annotated-tag requirement" },
    { "## \xe6\x9e\x84\xe5\xbb\xba",
      "build section -- cmake truth from CMakeCache, full-target build" },
    { "ctest",
      "test entry point -- how to run the unit tests" },
    { "MIN_PASS",
      "assertion-count gate discipline (ledger and CI comment written together)" },
};

// 只属于「逐批次复测」历史的指纹串：出现在 AGENTS.md = 台账又堆回正文了；
// 同时出现在两处 = 双写源（改一处另一处静默失配）。
static const char *const kLedgerFingerprints[] = {
    "\xe4\xb8\x8a\xe9\x9d\xa2\xe5\x90\x84\xe7\xbb\x84\xe7\x99\xbb\xe8\xae\xb0"
    "\xe5\x8e\x9f\xe6\xa0\xb7\xe4\xbf\x9d\xe7\x95\x99",                    // 上面各组登记原样保留
    "\xe4\xb8\x8d\xe5\xa4\x8d\xe7\x94\xa8\xe5\x85\xb6\xe6\x95\xb0\xe5\xad\x97", // 不复用其数字
};

} // namespace

int tst_agentsize()
{
    const int before = TestHarness::failCount();

    QString agents;
    if (!readUtf16(QStringLiteral("AGENTS.md"), &agents)) {
        TestHarness::skip("agentsize: SKIP (AGENTS.md not reachable via LITE_REPO_ROOT)\n");
        return TestHarness::failCount() - before;
    }
    QString ledger;
    const bool haveLedger = readUtf16(QStringLiteral("docs/agents-ledger.md"), &ledger);

    // ---- 预算本身必须为正，否则下面所有窗口判据退化成「整文件」而恒真 ----
    check(AgentConst::kProjectInstructionsCharBudget > 0,
          "agentsize: injection budget is positive");

    // ---- ① 锚点在窗 ----
    const QString window = agents.left(AgentConst::kProjectInstructionsCharBudget);
    for (const Anchor &a : kMustBeInWindow) {
        const QString needle = QString::fromUtf8(a.needle);
        if (window.contains(needle)) {
            ++TestHarness::passCount();
        } else {
            std::printf("FAIL: agentsize: rule anchor outside injection window: %s\n", a.why);
            ++TestHarness::failCount();
        }
    }

    // ---- ② 棘轮 ----
    check(agents.size() <= kAgentsSizeRatchet,
          "agentsize: AGENTS.md code-unit count within ratchet ceiling (move per-batch "
          "ledgers into docs/agents-ledger.md instead of raising this number)");

    // ---- ③ 台账落点存在、非空、且 AGENTS.md 留了指针 ----
    check(haveLedger, "agentsize: docs/agents-ledger.md is reachable");
    check(ledger.size() > 1000, "agentsize: ledger actually holds the moved content");
    check(agents.contains(QStringLiteral("agents-ledger")),
          "agentsize: AGENTS.md points at the ledger file (otherwise the next batch has "
          "nowhere to put its retest and stacks it back into the body)");

    // ---- ④ 搬运是「移动」不是「删除」，也不是「复制」 ----
    for (const char *fp : kLedgerFingerprints) {
        const QString needle = QString::fromUtf8(fp);
        check(ledger.contains(needle),
              "agentsize: ledger really contains the moved per-batch history");
        check(!agents.contains(needle),
              "agentsize: per-batch ledger marker is back inside AGENTS.md (it eats the "
              "injection window again)");
    }

    return TestHarness::failCount() - before;
}
