#pragma once

// s13 Agent Teams 宿主装配（P3b）单源 TU 的公开面。
// AgentLoop 自身的团队成员函数（initTeamEngine / leadToolCwd / injectTeamEvents /
// leadTurnEndSettlement / settleLeadLease / onTeammateTurnRequested /
// onTeammateFinished / settleTeamOnExit）声明在 AgentLoop.h、实现在 AgentLoopTeam.cpp；
// 本头只暴露非成员的工具 schema 工厂，供该 TU 内部与后续测试复用。

#include <QJsonArray>

namespace AgentLoopTeam
{
// 队友侧 10 工具 schema（lcc TEAMMATE_TOOLS agent_teams_manager.py :100-212 逐字：
// base 五件 bash/read_file/write_file/edit_file/glob + send_message + submit_plan +
// list_tasks/claim_task/complete_task 借用件）。
// 决策 D5：与 AgentLoop::createToolsDefinition 的主表缓存分立——主表是 Lead 面 25 工具
// 的禁翻区静态缓存，队友面刻意不含 create_task/update_task/spawn/worktree/load_skill/
// compact/todo_write（只有 Lead 能改依赖图、派活、开隔离区；lcc :188 注释同义）。
// 信封为 lite OpenAI function 形态（lcc anthropic input_schema 的转写，口径同主表）。
QJsonArray teammateToolsDefinition();

} // namespace AgentLoopTeam
