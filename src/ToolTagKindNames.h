#ifndef TOOLTAGKINDNAMES_H
#define TOOLTAGKINDNAMES_H

/*
    ToolTagKindNames.h —— 工具名 → 标签类别的纯映射（只依赖 Qt6::Core）

    为什么从 ToolTagKind.h 拆出来（P3-1）：映射本身是纯字符串函数，但原头文件把
    它与 `applyTo(QLabel*, ...)`（需要 Qt6::Gui）放在一起，导致单测目标（只链接
    Qt6::Core，见 CMakeLists 单测节）无法 include 它 —— 于是这份「改错一个分支就
    让时间线标签静默变色」的映射一直没有任何测试。拆成纯头后 tst_tooltagkind
    可以直接钉死每个分支。

    契约（与 stylesheet/<theme>/ToolBlock.qss、PermissionCard.qss 的
    [toolTagKind="…"] 选择器一一对应）：
      write    —— 写文件类（write_file / edit_file）
      run      —— 执行命令（bash）
      search   —— 检索（glob）
      read     —— 只读查询（read_file / list_tasks / get_task / list_teammates）
      plan     —— 计划与编排（todo_write / 任务图四件 / 团队编排四件）
      delegate —— 委派（task / load_skill / send_message / request_shutdown）
      other    —— 未识别一律落这里（宁可中性灰，也不猜一个彩色误导用户）

    大小写敏感：工具名来自 schema 常量，全小写下划线，不做归一化。
*/

#include <QString>

namespace ToolTagKindNames
{
    inline QString nameForTool(const QString& toolName)
    {
        if (toolName == QStringLiteral("write_file")
            || toolName == QStringLiteral("edit_file"))
            return QStringLiteral("write");
        if (toolName == QStringLiteral("bash"))
            return QStringLiteral("run");
        if (toolName == QStringLiteral("glob"))
            return QStringLiteral("search");
        if (toolName == QStringLiteral("read_file")
            || toolName == QStringLiteral("list_tasks")
            || toolName == QStringLiteral("get_task")
            || toolName == QStringLiteral("list_teammates"))
            return QStringLiteral("read");
        if (toolName == QStringLiteral("todo_write")
            || toolName == QStringLiteral("create_task")
            || toolName == QStringLiteral("update_task")
            || toolName == QStringLiteral("claim_task")
            || toolName == QStringLiteral("complete_task")
            // s13 团队（P3）：编排/闸门类归 plan，不新增 kind（避免动三主题 QSS 面）
            || toolName == QStringLiteral("spawn_teammate")
            || toolName == QStringLiteral("request_plan")
            || toolName == QStringLiteral("review_plan")
            || toolName == QStringLiteral("create_worktree"))
            return QStringLiteral("plan");
        if (toolName == QStringLiteral("task")
            || toolName == QStringLiteral("load_skill")
            // s13 团队：对队友的通信/关停属委派语义
            || toolName == QStringLiteral("send_message")
            || toolName == QStringLiteral("request_shutdown"))
            return QStringLiteral("delegate");
        return QStringLiteral("other");
    }
} // namespace ToolTagKindNames

#endif // TOOLTAGKINDNAMES_H