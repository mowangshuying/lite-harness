// tst_toolnames.cpp —— 工具名常量表契约（src/ToolNames.h + ToolTagKindNames.h 交叉）
// 被测：常量非空、无重复值、命名格式、与标签映射的覆盖关系。
//
// 为什么必须测：这张表是全仓工具名的唯一事实源，schema 注册 / handler 表 / 钩子比较 /
// 标签映射都引用它。重复值或空值不会编译失败，只会让某个工具在某一处静默失配
// （典型：两个常量同值 → 标签映射分支被前者吃掉，后者永远走不到）。
//
// 零夹具依赖，任何环境全量执行。

#include "TestHarness.h"

#include "ToolNames.h"
#include "ToolTagKindNames.h"
#include "CompactStageNames.h" // s18.2：本地四段压缩的 UI 伪名（⑦ 组断言）

#include <QString>
#include <QStringList>

namespace {

using TestHarness::check;

// 常量清单集中在此：新增工具名必须同步加一行，否则下面的「非空/唯一」聚合断言
// 覆盖不到它（这也是本套件存在的一半价值——清单本身就是一处需要维护的事实）
QStringList allToolNameConstants()
{
    return {
        ToolNames::BASH,          ToolNames::READ_FILE,      ToolNames::WRITE_FILE,
        ToolNames::EDIT_FILE,     ToolNames::GLOB,           ToolNames::TODO_WRITE,
        ToolNames::TASK,          ToolNames::LOAD_SKILL,     ToolNames::COMPACT,
        ToolNames::CREATE_TASK,   ToolNames::UPDATE_TASK,    ToolNames::LIST_TASKS,
        ToolNames::GET_TASK,      ToolNames::CLAIM_TASK,     ToolNames::COMPLETE_TASK,
        ToolNames::SCHEDULE_CRON, ToolNames::LIST_CRONS,     ToolNames::CANCEL_CRON,
        ToolNames::SPAWN_TEAMMATE, ToolNames::LIST_TEAMMATES, ToolNames::SEND_MESSAGE,
        ToolNames::REQUEST_SHUTDOWN, ToolNames::REQUEST_PLAN, ToolNames::REVIEW_PLAN,
        ToolNames::CREATE_WORKTREE, ToolNames::CONNECT_MCP,  ToolNames::WORKFLOW,
    };
}

} // namespace

int tst_toolnames()
{
    const int before = TestHarness::failCount();

    const QStringList names = allToolNameConstants();

    // ---- ① 非空 ----
    bool allNonEmpty = true;
    for (const QString &n : names)
    {
        if (n.isEmpty())
            allNonEmpty = false;
    }
    check(allNonEmpty, "toolnames: no empty constant");

    // ---- ② 无重复值（重复会让某分支永远走不到）----
    bool allUnique = true;
    for (int i = 0; i < names.size(); ++i)
    {
        for (int j = i + 1; j < names.size(); ++j)
        {
            if (names.at(i) == names.at(j))
                allUnique = false;
        }
    }
    check(allUnique, "toolnames: all values unique");

    // ---- ③ 命名格式：全小写、无空格（schema 里就是这种形态）----
    bool allLowercase = true;
    bool noSpaces = true;
    for (const QString &n : names)
    {
        if (n != n.toLower())
            allLowercase = false;
        if (n.contains(QLatin1Char(' ')))
            noSpaces = false;
    }
    check(allLowercase, "toolnames: all lowercase");
    check(noSpaces, "toolnames: no spaces");

    // ---- ④ 与标签映射交叉：每个工具名都必须落到合法类别（7 选 1）----
    // 注意不断言「非 other」：compact/workflow/cron 三件与 mcp 入口刻意不在标签映射表里
    // （它们不是文件/命令/检索类动作），落 other 是设计选择而非漏配。
    const QStringList validKinds = {QStringLiteral("write"), QStringLiteral("run"),
                                    QStringLiteral("search"), QStringLiteral("read"),
                                    QStringLiteral("plan"), QStringLiteral("delegate"),
                                    QStringLiteral("other")};
    bool allKindsValid = true;
    for (const QString &n : names)
    {
        if (!validKinds.contains(ToolTagKindNames::nameForTool(n)))
            allKindsValid = false;
    }
    check(allKindsValid, "toolnames: every name maps to a valid tag kind");

    // ---- ⑤ 映射表覆盖的工具名必须都在本表里（反向一致性）----
    // 若映射表写了 ToolNames 没有的字面量，说明有人绕过了单源表 —— 那正是本表要防的
    const QStringList mappedNames = {
        QStringLiteral("write_file"),    QStringLiteral("edit_file"),
        QStringLiteral("bash"),          QStringLiteral("glob"),
        QStringLiteral("read_file"),     QStringLiteral("list_tasks"),
        QStringLiteral("get_task"),      QStringLiteral("list_teammates"),
        QStringLiteral("todo_write"),    QStringLiteral("create_task"),
        QStringLiteral("update_task"),   QStringLiteral("claim_task"),
        QStringLiteral("complete_task"), QStringLiteral("spawn_teammate"),
        QStringLiteral("request_plan"),  QStringLiteral("review_plan"),
        QStringLiteral("create_worktree"), QStringLiteral("task"),
        QStringLiteral("load_skill"),    QStringLiteral("send_message"),
        QStringLiteral("request_shutdown"),
    };
    bool mappedAllPresent = true;
    for (const QString &m : mappedNames)
    {
        if (!names.contains(m))
            mappedAllPresent = false;
    }
    check(mappedAllPresent, "toolnames: every tag-mapped name exists in ToolNames");

    // ---- ⑥ MCP 前缀是纯前缀（不含占位符，拼接方自己补 server/tool）----
    check(ToolNames::MCP_PREFIX == QStringLiteral("mcp__"), "toolnames: mcp prefix is exactly 'mcp__'");

    // ---- ⑦ 本地四段压缩的 UI 伪名（CompactStageNames.h，s18.2）----
    // 为什么必须测：这四个名字是「卡片发得出来」与「标题取得到」之间的唯一契约——
    // 发射侧 CompactManager::emitLocalCard 与呈现侧 ToolBlock::toolTitleText 都引用本头。
    // 任一侧被改回散写字面量、或名字漂移，卡片不会报错，只会静默回退成「已执行」——
    // 四张卡长得一模一样，正是本批要消除的形态。故字面量逐条钉死 + 撞名聚合断言。
    const QStringList stageNames = {
        CompactStageNames::BUDGET, CompactStageNames::SNIP,
        CompactStageNames::MICRO,  CompactStageNames::FIT,
    };
    check(CompactStageNames::BUDGET == QStringLiteral("compact_budget"),
          "stagenames: budget literal is 'compact_budget'");
    check(CompactStageNames::SNIP == QStringLiteral("compact_snip"),
          "stagenames: snip literal is 'compact_snip'");
    check(CompactStageNames::MICRO == QStringLiteral("compact_micro"),
          "stagenames: micro literal is 'compact_micro'");
    check(CompactStageNames::FIT == QStringLiteral("compact_fit"),
          "stagenames: fit literal is 'compact_fit'");

    // 非空 / 全小写 / 无空格 / compact_ 前缀 / 互不重复 / 与真工具名及既有 memory 伪键零交集
    // （撞名的后果与 ② 同款：ToolBlock 标题表里靠前的分支吃掉靠后的，后者永远走不到）
    bool stagesWellFormed = true;
    for (int i = 0; i < stageNames.size(); ++i)
    {
        const QString &s = stageNames.at(i);
        if (s.isEmpty() || s != s.toLower() || s.contains(QLatin1Char(' '))
            || !s.startsWith(QStringLiteral("compact_")))
            stagesWellFormed = false;
        if (names.contains(s) || s == QLatin1String("memory"))
            stagesWellFormed = false;
        for (int j = i + 1; j < stageNames.size(); ++j)
        {
            if (s == stageNames.at(j))
                stagesWellFormed = false;
        }
    }
    check(stagesWellFormed,
          "stagenames: well-formed, unique, no collision with ToolNames or 'memory'");

    // 标签类别：四个伪名刻意不在映射表里 → 一律 other 中性灰（同 compact / memory 的设计选择，
    // 不新增 kind 即三主题 QSS 零改动）。若有人给它们配了彩色类别，这条会红。
    check(ToolTagKindNames::nameForTool(CompactStageNames::BUDGET) == QStringLiteral("other"),
          "stagenames: budget -> other (neutral)");
    check(ToolTagKindNames::nameForTool(CompactStageNames::SNIP) == QStringLiteral("other"),
          "stagenames: snip -> other (neutral)");
    check(ToolTagKindNames::nameForTool(CompactStageNames::MICRO) == QStringLiteral("other"),
          "stagenames: micro -> other (neutral)");
    check(ToolTagKindNames::nameForTool(CompactStageNames::FIT) == QStringLiteral("other"),
          "stagenames: fit -> other (neutral)");

    return TestHarness::failCount() - before;
}