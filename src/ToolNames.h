#pragma once

// 工具名集中常量（lcc a6d29b9 tool_names.py 移植）：全仓工具名唯一事实源。
// 18 个常量值逐字对齐 lcc tool_names.py（BASH/READ_FILE/WRITE_FILE/EDIT_FILE/GLOB/
// TODO_WRITE/TASK/LOAD_SKILL/COMPACT/CREATE_TASK/UPDATE_TASK/LIST_TASKS/GET_TASK/
// CLAIM_TASK/COMPLETE_TASK/SCHEDULE_CRON/LIST_CRONS/CANCEL_CRON）。
// AgentLoop（schema 注册 / mainToolHandlers / executeTool 特判 / toolSummary / 钩子比较）、
// SubAgent（白名单 / bash 分支）、ToolBlock（标题表）一律引用本文件，不再散写字面量。
//
// 与 lcc 的有意偏差（移植转译，登记在案）：
// - lcc 无 MEMORY 常量：ToolBlock 标题表中的 "memory" 为 UI 伪键（记忆卡片），非模型
//   工具名，保持字面量不入本表；
// - Python 模块属性 → C++ inline const QString（头文件唯一定义，无需 .cpp）。

#include <QString>

namespace ToolNames {

inline const QString BASH = QStringLiteral("bash");
inline const QString READ_FILE = QStringLiteral("read_file");
inline const QString WRITE_FILE = QStringLiteral("write_file");
inline const QString EDIT_FILE = QStringLiteral("edit_file");
inline const QString GLOB = QStringLiteral("glob");
inline const QString TODO_WRITE = QStringLiteral("todo_write");
inline const QString TASK = QStringLiteral("task");
inline const QString LOAD_SKILL = QStringLiteral("load_skill");
inline const QString COMPACT = QStringLiteral("compact");
inline const QString CREATE_TASK = QStringLiteral("create_task");
inline const QString UPDATE_TASK = QStringLiteral("update_task");
inline const QString LIST_TASKS = QStringLiteral("list_tasks");
inline const QString GET_TASK = QStringLiteral("get_task");
inline const QString CLAIM_TASK = QStringLiteral("claim_task");
inline const QString COMPLETE_TASK = QStringLiteral("complete_task");
inline const QString SCHEDULE_CRON = QStringLiteral("schedule_cron");
inline const QString LIST_CRONS = QStringLiteral("list_crons");
inline const QString CANCEL_CRON = QStringLiteral("cancel_cron");

} // namespace ToolNames
