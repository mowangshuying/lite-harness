#ifndef TOOLTAGKIND_H
#define TOOLTAGKIND_H

/*
    ToolTagKind.h —— 工具标签（toolTag / pcTag）的语义分类与配色映射

    工具块（ToolBlock）与权限卡（PermissionCard）的标签过去是硬编码的灰色，
    所有工具看起来一模一样，扫一眼时间线分不清「这次动的是文件、是命令、还是
    只是读东西」。这里把「工具名 -> 类别」收敛成一份共享口径，两个控件都把它
    写成标签的 toolTagKind 动态属性，QSS 只需按 [toolTagKind="…"] 给每类一个
    固定前景色 / 淡底色（跟随主题换肤，控件不写死颜色）。

    配色只作「类别提示」，不作状态提示 —— 运行中 / 成功 / 失败 / ASK 仍由
    既有状态色负责，两者互不干扰。

    未识别的工具名（含未来的新工具、memory 沉淀卡等）一律落到 other：宁可保持
    中性灰，也不猜一个彩色误导用户。分类按用户直觉走：read_file 归 read，
    glob 归 search，todo_write / 任务图工具归 plan，SubAgent 与技能加载归
    delegate。
*/

#include <QLabel>
#include <QString>
#include <QStyle>

namespace ToolTagKind
{
    // kind 名要与 stylesheet/<theme>/ToolBlock.qss 及 PermissionCard.qss 里
    // [toolTagKind="…"] 选择器一一对应。
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
            || toolName == QStringLiteral("get_task"))
            return QStringLiteral("read");
        if (toolName == QStringLiteral("todo_write")
            || toolName == QStringLiteral("create_task")
            || toolName == QStringLiteral("update_task")
            || toolName == QStringLiteral("claim_task")
            || toolName == QStringLiteral("complete_task"))
            return QStringLiteral("plan");
        if (toolName == QStringLiteral("task")
            || toolName == QStringLiteral("load_skill"))
            return QStringLiteral("delegate");
        return QStringLiteral("other");
    }

    // 把类别写到标签的动态属性上并强制重算 QSS（Qt 不会自动感知动态属性变化）。
    // 类别不变时直接返回，避免每帧流式刷新都 unpolish/polish。
    inline void applyTo(QLabel* label, const QString& toolName)
    {
        if (!label)
            return;
        const QString kind = nameForTool(toolName);
        if (label->property("toolTagKind").toString() == kind)
            return;
        label->setProperty("toolTagKind", kind);
        if (label->style())
        {
            label->style()->unpolish(label);
            label->style()->polish(label);
        }
        label->update();
    }
}

#endif // TOOLTAGKIND_H
