// todo_write（lcc s06 起无状态）：只校验入参并渲染面板文本，成功时以本次快照发射 todoUpdated。

#include "AgentLoop.h"

#include "AgentConstants.h"

#include <QJsonDocument>
#include <QJsonArray>

QString AgentLoop::renderTodos(const QVector<TodoItem> &items)
{
    // lcc TodoManager.render 等价：空清单 "No todos"；标记 [ ]/[>]；[x] 对应
    // pending/in_progress/completed；lcc 原文完成度统计元素以换行开头，
    // '\n' join 后在清单与统计之间形成一空行
    if (items.isEmpty())
        return QStringLiteral("No todos");

    QStringList lines;
    for (const TodoItem &item : items)
    {
        QString marker = QStringLiteral("[ ]");
        if (item.status == QStringLiteral("in_progress"))
            marker = QStringLiteral("[>]");
        else if (item.status == QStringLiteral("completed"))
            marker = QStringLiteral("[x]");
        lines.append(QStringLiteral("%1 %2").arg(marker, item.content));
    }

    int done = 0;
    for (const TodoItem &item : items)
    {
        if (item.status == QStringLiteral("completed"))
            ++done;
    }
    lines.append(QStringLiteral("\n(%1/%2 completed)").arg(done).arg(items.size()));

    return lines.join(QLatin1Char('\n'));
}

bool AgentLoop::normalizeTodoSnapshot(const QJsonObject &args, QJsonArray *snapshot,
                                      QString *error)
{
    // 自 runTodoWrite 逐字搬移的校验体（s17 缺陷修复：历史回放要重建「任务清单」侧栏终态与
    // 快照卡，校验口径必须与实时执行时一字不差，禁在回放侧重写一套）：错误串族逐字保留，
    // 成功时以本次输入清单快照 [{content, status}, ...] 回填 *snapshot
    QJsonValue todosValue = args.value(QStringLiteral("todos"));

    // 模型偶发把 todos 传成 JSON 字符串（对应 lcc json.loads/ast.literal_eval 兼容
    // 路径；lcc 的 literal_eval 无 Qt 等价，单引号 Python 字面量解析不了）。
    // todos 缺失时 lcc 直接 TypeError 崩溃，此处按"非列表"优雅拒绝（有意偏差）
    if (todosValue.isString())
    {
        QJsonParseError parseError;
        const QJsonDocument parsed =
            QJsonDocument::fromJson(todosValue.toString().toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            if (error)
                *error = QStringLiteral("Error:todos must be a list or JSON array string");
            return false;
        }
        if (!parsed.isArray())
        {
            if (error)
                *error = QStringLiteral("Error:todos must be a list");
            return false;
        }
        todosValue = QJsonValue(parsed.array());
    }

    if (!todosValue.isArray())
    {
        if (error)
            *error = QStringLiteral("Error:todos must be a list");
        return false;
    }

    const QJsonArray todos = todosValue.toArray();
    if (todos.size() > AgentConst::kTodoMaxItems)
    {
        if (error)
            *error = QStringLiteral("Error:Max %1 todos allowed").arg(AgentConst::kTodoMaxItems);
        return false;
    }

    QJsonArray built;
    int inProgressCount = 0;
    for (int index = 0; index < todos.size(); ++index)
    {
        if (!todos.at(index).isObject())
        {
            if (error)
                *error = QStringLiteral("Error:todos[%1] must be an object").arg(index);
            return false;
        }

        const QJsonObject todo = todos.at(index).toObject();
        // lcc str(todo.get("content","")).strip() / str(todo.get("status","pending")).lower()：
        // 非字符串值经 QVariant 转文本；status 仅在键缺失时取默认 pending（空串键值照旧报错）
        const QString content =
            todo.value(QStringLiteral("content")).toVariant().toString().trimmed();
        QString status = QStringLiteral("pending");
        if (todo.contains(QStringLiteral("status")))
            status = todo.value(QStringLiteral("status")).toVariant().toString().toLower();

        if (content.isEmpty())
        {
            if (error)
                *error = QStringLiteral("Error:todos[%1] requires content").arg(index);
            return false;
        }
        if (status != QStringLiteral("pending") && status != QStringLiteral("in_progress")
            && status != QStringLiteral("completed"))
        {
            if (error)
                *error = QStringLiteral("Error:todos[%1] has invalid status '%2'")
                             .arg(index)
                             .arg(status);
            return false;
        }
        if (status == QStringLiteral("in_progress"))
            ++inProgressCount;

        QJsonObject itemObj;
        itemObj[QStringLiteral("content")] = content;
        itemObj[QStringLiteral("status")] = status;
        built.append(itemObj);
    }

    if (inProgressCount > 1)
    {
        if (error)
            *error = QStringLiteral("Error:Only one todo can be in_progress at a time");
        return false;
    }

    *snapshot = built;
    return true;
}

QString AgentLoop::runTodoWrite(const QJsonObject &args)
{
    // lcc s06 update_todos 等价：无状态——只校验并渲染本次入参，不落任何持久清单
    // （lcc s05 的 TodoManager.items 持久化与 'Current Tasks' 控制台面板在 s06 移除）；
    // 任一校验失败返回 "Error:..."（lcc 抛 ValueError、run_todo_write 捕获转 "Error:{e}"）。
    // 校验与快照归一在 normalizeTodoSnapshot 单源（回放共用），本函数只做渲染与广播
    QJsonArray snapshot;
    QString error;
    if (!normalizeTodoSnapshot(args, &snapshot, &error))
        return error;

    // 渲染面板文本用快照反构 items（与原 validated 逐字段同值：content 已 trim、status 已归一）
    QVector<TodoItem> items;
    for (const QJsonValue &value : snapshot)
    {
        const QJsonObject obj = value.toObject();
        items.append(TodoItem{obj.value(QStringLiteral("content")).toString(),
                              obj.value(QStringLiteral("status")).toString()});
    }

    // lcc s06：渲染本次入参并直接返回（无持久化、无 s05 的 'Current Tasks' 控制台面板）
    const QString output = renderTodos(items);

    // 跨车道契约：每次校验通过（含清为空清单）后广播本次输入清单快照 [{content, status}, ...]，
    // 供 TodoCard 渲染（无状态下由模型逐轮重发全量清单维持面板内容）
    emit todoUpdated(snapshot);

    return output;
}
