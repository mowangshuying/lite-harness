// tst_tooltagkind.cpp —— 工具名 → 标签类别映射直测（src/ToolTagKindNames.h，纯 QtCore）
// 被测：ToolTagKindNames::nameForTool 的每个分支 + 未识别回落。
//
// 为什么必须测：映射结果直接喂给 QSS 的 [toolTagKind="…"] 选择器，拼错一个类别名
// 不会报错、不会崩，只是那个标签静默变灰 —— 而「所有工具看起来一样」正是这份映射
// 当初要解决的问题。分支多（16 个工具名）、改动分散（每加一个工具就要加一行），
// 靠冒烟运行看不全。
//
// 零夹具依赖，任何环境全量执行。

#include "TestHarness.h"

#include "ToolTagKindNames.h"

#include <QString>

namespace {

using TestHarness::check;
using ToolTagKindNames::nameForTool;

} // namespace

int tst_tooltagkind()
{
    const int before = TestHarness::failCount();

    // ---- ① write：写文件类 ----
    check(nameForTool(QStringLiteral("write_file")) == QStringLiteral("write"), "tag: write_file -> write");
    check(nameForTool(QStringLiteral("edit_file")) == QStringLiteral("write"), "tag: edit_file -> write");

    // ---- ② run：执行命令 ----
    check(nameForTool(QStringLiteral("bash")) == QStringLiteral("run"), "tag: bash -> run");

    // ---- ③ search：检索 ----
    check(nameForTool(QStringLiteral("glob")) == QStringLiteral("search"), "tag: glob -> search");

    // ---- ④ read：只读查询 ----
    check(nameForTool(QStringLiteral("read_file")) == QStringLiteral("read"), "tag: read_file -> read");
    check(nameForTool(QStringLiteral("list_tasks")) == QStringLiteral("read"), "tag: list_tasks -> read");
    check(nameForTool(QStringLiteral("get_task")) == QStringLiteral("read"), "tag: get_task -> read");
    check(nameForTool(QStringLiteral("list_teammates")) == QStringLiteral("read"), "tag: list_teammates -> read");

    // ---- ⑤ plan：计划与编排（任务图四件 + 团队编排四件）----
    check(nameForTool(QStringLiteral("todo_write")) == QStringLiteral("plan"), "tag: todo_write -> plan");
    check(nameForTool(QStringLiteral("create_task")) == QStringLiteral("plan"), "tag: create_task -> plan");
    check(nameForTool(QStringLiteral("update_task")) == QStringLiteral("plan"), "tag: update_task -> plan");
    check(nameForTool(QStringLiteral("claim_task")) == QStringLiteral("plan"), "tag: claim_task -> plan");
    check(nameForTool(QStringLiteral("complete_task")) == QStringLiteral("plan"), "tag: complete_task -> plan");
    check(nameForTool(QStringLiteral("spawn_teammate")) == QStringLiteral("plan"), "tag: spawn_teammate -> plan");
    check(nameForTool(QStringLiteral("request_plan")) == QStringLiteral("plan"), "tag: request_plan -> plan");
    check(nameForTool(QStringLiteral("review_plan")) == QStringLiteral("plan"), "tag: review_plan -> plan");
    check(nameForTool(QStringLiteral("create_worktree")) == QStringLiteral("plan"), "tag: create_worktree -> plan");

    // ---- ⑥ delegate：委派 ----
    check(nameForTool(QStringLiteral("task")) == QStringLiteral("delegate"), "tag: task -> delegate");
    check(nameForTool(QStringLiteral("load_skill")) == QStringLiteral("delegate"), "tag: load_skill -> delegate");
    check(nameForTool(QStringLiteral("send_message")) == QStringLiteral("delegate"), "tag: send_message -> delegate");
    check(nameForTool(QStringLiteral("request_shutdown")) == QStringLiteral("delegate"),
          "tag: request_shutdown -> delegate");

    // ---- ⑦ other：未识别一律中性灰（宁可不错色，也不猜）----
    check(nameForTool(QString()) == QStringLiteral("other"), "tag: empty -> other");
    check(nameForTool(QStringLiteral("some_future_tool")) == QStringLiteral("other"), "tag: unknown -> other");
    check(nameForTool(QStringLiteral("memory")) == QStringLiteral("other"), "tag: memory (UI pseudo key) -> other");
    // 大小写敏感：工具名来自 schema 常量（全小写），不做归一化；"BASH" 不是合法工具名
    check(nameForTool(QStringLiteral("BASH")) == QStringLiteral("other"), "tag: uppercase BASH -> other (case-sensitive)");
    check(nameForTool(QStringLiteral("read-file")) == QStringLiteral("other"), "tag: hyphen variant -> other");

    return TestHarness::failCount() - before;
}