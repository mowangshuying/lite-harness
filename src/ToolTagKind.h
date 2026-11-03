#ifndef TOOLTAGKIND_H
#define TOOLTAGKIND_H

/*
    ToolTagKind.h —— 把工具标签（toolTag / pcTag）的语义分类写到控件上

    工具块（ToolBlock）与权限卡（PermissionCard）的标签过去是硬编码的灰色，
    所有工具看起来一模一样，扫一眼时间线分不清「这次动的是文件、是命令、还是
    只是读东西」。分类口径本身在 ToolTagKindNames.h（纯 QtCore，可单测）；
    本头只留「写动态属性 + 强制重算 QSS」这一步（需要 Qt6::Gui）。

    配色只作「类别提示」，不作状态提示 —— 运行中 / 成功 / 失败 / ASK 仍由
    既有状态色负责，两者互不干扰。
*/

#include <QLabel>
#include <QString>
#include <QStyle>

#include "ToolTagKindNames.h" // 工具名 → 类别（纯映射，P3-1 拆出以便单测）

namespace ToolTagKind
{
    // 把类别写到标签的动态属性上并强制重算 QSS（Qt 不会自动感知动态属性变化）。
    // 类别不变时直接返回，避免每帧流式刷新都 unpolish/polish。
    inline void applyTo(QLabel* label, const QString& toolName)
    {
        if (!label)
            return;
        const QString kind = ToolTagKindNames::nameForTool(toolName);
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
} // namespace ToolTagKind

#endif // TOOLTAGKIND_H