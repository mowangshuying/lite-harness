// tst_toolschema.cpp —— 工具 schema 单源直测（src/ToolSchema.h，header-only 零 CMake 改动即入
// ctest，仿 tst_memoryrules / tst_heatmapgrid 先例）。
//
// 抽头动机：`createToolsDefinition()` 的 490 行此前长在 `AgentLoopPrompt.cpp`，该 TU 经
// `AgentLoop.h` 传递 include `QOpenAi.h`（链 Qt6::Network），按「仅允许 QtCore-only 引擎进测试
// 清单」的硬纪律进不了 ctest —— 于是**整套 schema 零直测**。它是典型的「本地全绿、线上炸」：
//   · `required` 写了 `properties` 里没有的键、或 `parameters.type` 不是 "object" → 服务端 400，
//     会话第一个请求就失败，而编译与既有测试全绿；
//   · 类型专属关键字挂错宿主（`items` 挂到 string、`pattern` 挂到 array）→ 同样是被拒或静默忽略；
//   · `function.name` 与 `ToolNames` 单源漂移、或与 `SubAgent` 白名单不交 → 工具能被调用但恒
//     no-op，用户看到「模型说做了但什么都没发生」。
// 这些只能靠断言钉：490 行字面量肉眼扫不出配对漂移。
//
// 已知未覆盖面（诚实登记，勿当作已验）：`todos.status` 的 enum 与**执行侧**三值白名单的交叉
// 断言做不了 —— 执行侧白名单是 `AgentLoopTodo.cpp` 里的硬编码字面量，不在纯头内，链它就要求
// moc + QOpenAi。本套件只钉 schema 侧的 enum 取值，执行侧漂移仍需人工对照。
//
// 分组：全部零夹具恒跑（纯 JSON 结构算术，不发请求、不碰磁盘）。

#include "TestHarness.h"

#include "ToolSchema.h"

#include "AgentConstants.h"
#include "ToolNames.h"
#include "WorkflowManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QSet>

namespace {

// ---- 取用工具 ----

QJsonObject toolByName(const QJsonArray &tools, const QString &name)
{
    for (const QJsonValue &v : tools)
    {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString()
            == name)
            return o;
    }
    return QJsonObject();
}

QString funcName(const QJsonObject &tool)
{
    return tool.value(QStringLiteral("function")).toObject()
        .value(QStringLiteral("name")).toString();
}

QJsonObject funcOf(const QJsonObject &tool)
{
    return tool.value(QStringLiteral("function")).toObject();
}

QJsonObject paramsOf(const QJsonObject &tool)
{
    return funcOf(tool).value(QStringLiteral("parameters")).toObject();
}

QJsonObject propsOf(const QJsonObject &tool)
{
    return paramsOf(tool).value(QStringLiteral("properties")).toObject();
}

QStringList requiredOf(const QJsonObject &tool)
{
    QStringList out;
    for (const QJsonValue &v : paramsOf(tool).value(QStringLiteral("required")).toArray())
        out.append(v.toString());
    return out;
}

// 全仓 27 个工具的名单：26 个来自 ToolNames 单源，workflow 信封来自 WorkflowManager::toolInfo()
QStringList allToolNames()
{
    return {
        ToolNames::BASH, ToolNames::READ_FILE, ToolNames::WRITE_FILE, ToolNames::EDIT_FILE,
        ToolNames::GLOB, ToolNames::TODO_WRITE, ToolNames::TASK, ToolNames::LOAD_SKILL,
        ToolNames::COMPACT, ToolNames::CREATE_TASK, ToolNames::UPDATE_TASK, ToolNames::LIST_TASKS,
        ToolNames::GET_TASK, ToolNames::CLAIM_TASK, ToolNames::COMPLETE_TASK,
        ToolNames::SCHEDULE_CRON, ToolNames::LIST_CRONS, ToolNames::CANCEL_CRON,
        ToolNames::SPAWN_TEAMMATE, ToolNames::LIST_TEAMMATES, ToolNames::SEND_MESSAGE,
        ToolNames::REQUEST_SHUTDOWN, ToolNames::REQUEST_PLAN, ToolNames::REVIEW_PLAN,
        ToolNames::CREATE_WORKTREE, ToolNames::CONNECT_MCP, ToolNames::WORKFLOW,
    };
}

// ---- 组 1：信封骨架与名单同源（服务端 400 / 恒 no-op 的第一道面）----

void testEnvelopeAndNames(const QJsonArray &tools)
{
    TestHarness::check(tools.size() == 27, "schema: 恰好 27 个工具");

    QSet<QString> seen;
    int badEnvelopeType = 0;
    int emptyName = 0;
    int duplicateName = 0;
    for (const QJsonValue &v : tools)
    {
        const QJsonObject tool = v.toObject();
        if (tool.value(QStringLiteral("type")).toString() != QStringLiteral("function"))
            ++badEnvelopeType;
        const QString name = funcName(tool);
        if (name.trimmed().isEmpty())
            ++emptyName;
        else if (seen.contains(name))
            ++duplicateName;
        seen.insert(name);
    }
    // 三个计数器分开钉：混成一个失败点就只知道「信封坏了」，不知道是坏在哪一维
    TestHarness::check(badEnvelopeType == 0, "schema: 每个信封 type 恒为 function");
    TestHarness::check(emptyName == 0, "schema: function.name 非空");
    TestHarness::check(duplicateName == 0, "schema: function.name 不重复");

    // 名单必须与 ToolNames 单源完全重合（既不多也不少：漂一个就是发给模型一个不存在的工具，
    // 或执行器有工具却不让模型调）
    const QStringList expected = allToolNames();
    TestHarness::check(expected.size() == 27, "schema: 期望名单 27 项");
    int missing = 0;
    for (const QString &n : expected)
        if (seen.contains(n))
            continue;
        else
            ++missing;
    TestHarness::check(missing == 0, "schema: ToolNames 名单全部出现在 schema 内");
    int extra = 0;
    for (const QString &n : seen)
        if (!expected.contains(n))
            ++extra;
    TestHarness::check(extra == 0, "schema: schema 内无 ToolNames 之外的野名字");

    // 描述非空：空描述的工具模型不会用，等价于静默少一个能力
    int emptyDesc = 0;
    for (const QJsonValue &v : tools)
        if (funcOf(v.toObject()).value(QStringLiteral("description")).toString().trimmed().isEmpty())
            ++emptyDesc;
    TestHarness::check(emptyDesc == 0, "schema: 每个工具 description 非空");
}

// ---- 组 2：required ⊆ properties（服务端 400 的主因）----

void testRequiredSubsetOfProperties(const QJsonArray &tools)
{
    int badRequired = 0;
    int badParamsType = 0;
    int badPropsType = 0;
    QStringList offenders;
    for (const QJsonValue &v : tools)
    {
        const QJsonObject tool = v.toObject();
        const QJsonObject params = paramsOf(tool);
        if (params.value(QStringLiteral("type")).toString() != QStringLiteral("object"))
            ++badParamsType;
        if (!params.contains(QStringLiteral("properties")))
            ++badPropsType;
        const QJsonObject props = propsOf(tool);
        for (const QString &r : requiredOf(tool))
        {
            if (r.trimmed().isEmpty() || !props.contains(r))
            {
                ++badRequired;
                offenders << funcName(tool) + ':' + r;
            }
        }
    }
    TestHarness::check(badParamsType == 0, "schema: parameters.type 恒为 object");
    TestHarness::check(badPropsType == 0, "schema: parameters 恒含 properties 对象");
    TestHarness::check(badRequired == 0,
                       QString("schema: required 每一项都在 properties 内（违例 %1）")
                           .arg(offenders.join(',')).toUtf8().constData());
}

// ---- 组 3：刻意不写 required 的三个工具（与 list_crons 的空 required 是两种形态，不可合并）----

void testNoRequiredShape(const QJsonArray &tools)
{
    // compact / list_tasks / list_teammates：手工构造，properties 为空且**不写 required 键**
    // （源码注释明记「makeTool 恒写 required，故手工构造，勿顺手补齐」）
    for (const QString &name : {ToolNames::COMPACT, ToolNames::LIST_TASKS, ToolNames::LIST_TEAMMATES})
    {
        const QJsonObject tool = toolByName(tools, name);
        TestHarness::check(!tool.isEmpty(), QString("no-required: %1 存在").arg(name).toUtf8().constData());
        TestHarness::check(!paramsOf(tool).contains(QStringLiteral("required")),
                           QString("no-required: %1 不写 required 键").arg(name).toUtf8().constData());
        TestHarness::check(propsOf(tool).isEmpty(),
                           QString("no-required: %1 properties 为空").arg(name).toUtf8().constData());
    }

    // list_crons 走 makeTool：空 props 仍**写** required:[]，与上面三块形态不等价
    const QJsonObject crons = toolByName(tools, ToolNames::LIST_CRONS);
    TestHarness::check(paramsOf(crons).contains(QStringLiteral("required")),
                       "no-required: list_crons 走 makeTool 恒写 required 键");
    TestHarness::check(paramsOf(crons).value(QStringLiteral("required")).toArray().isEmpty(),
                       "no-required: list_crons 的 required 是空数组");
    TestHarness::check(propsOf(crons).isEmpty(), "no-required: list_crons properties 为空");
}

// ---- 组 4：类型专属关键字不得挂错宿主（挂错即被服务端拒或静默忽略）----

void testTypeKeywordPairing(const QJsonArray &tools)
{
    const QSet<QString> primitiveTypes = {QStringLiteral("string"), QStringLiteral("number"),
                                          QStringLiteral("integer"), QStringLiteral("boolean"),
                                          QStringLiteral("array"), QStringLiteral("object")};
    int badType = 0;
    int arrayKwOnNonArray = 0;
    int stringKwOnNonString = 0;
    QStringList offenders;
    for (const QJsonValue &v : tools)
    {
        const QJsonObject tool = v.toObject();
        const QJsonObject props = propsOf(tool);
        for (auto it = props.constBegin(); it != props.constEnd(); ++it)
        {
            const QJsonObject schema = it.value().toObject();
            const QString type = schema.value(QStringLiteral("type")).toString();
            const QString where = funcName(tool) + '.' + it.key();
            if (!primitiveTypes.contains(type))
                ++badType;
            for (const QString &kw : {QStringLiteral("items"), QStringLiteral("minItems"),
                                      QStringLiteral("maxItems")})
            {
                if (schema.contains(kw) && type != QStringLiteral("array"))
                {
                    ++arrayKwOnNonArray;
                    offenders << where + '/' + kw;
                }
            }
            for (const QString &kw : {QStringLiteral("enum"), QStringLiteral("pattern"),
                                      QStringLiteral("minLength"), QStringLiteral("maxLength")})
            {
                if (schema.contains(kw) && type != QStringLiteral("string"))
                {
                    ++stringKwOnNonString;
                    offenders << where + '/' + kw;
                }
            }
            // type==array 必须有 items：无 items 的数组 schema 等于没声明元素类型
            if (type == QStringLiteral("array") && !schema.contains(QStringLiteral("items")))
            {
                ++arrayKwOnNonArray;
                offenders << where + "/missing-items";
            }
        }
    }
    TestHarness::check(badType == 0, "types: 每个参数 type 都是 JSON Schema 原始类型");
    TestHarness::check(arrayKwOnNonArray == 0,
                       QString("types: 数组专属关键字只挂 array（违例 %1）")
                           .arg(offenders.join(',')).toUtf8().constData());
    TestHarness::check(stringKwOnNonString == 0, "types: 字符串专属关键字只挂 string");
}

// ---- 组 5：todo_write 的跨字段配对（enum/maxItems 与执行侧同源常量）----

void testTodoWritePairing(const QJsonArray &tools)
{
    const QJsonObject todo = toolByName(tools, ToolNames::TODO_WRITE);
    const QJsonObject props = propsOf(todo);
    TestHarness::check(props.contains(QStringLiteral("todos")), "todo: 有 todos 参数");
    TestHarness::check(requiredOf(todo) == QStringList{QStringLiteral("todos")},
                       "todo: required 恰为 [todos]");

    const QJsonObject todos = props.value(QStringLiteral("todos")).toObject();
    TestHarness::check(todos.value(QStringLiteral("type")).toString() == QStringLiteral("array"),
                       "todo: todos 是 array");
    // maxItems 与执行器同源：AgentConst::kTodoMaxItems（执行侧超限报 "Error:Max %1 todos allowed"）
    TestHarness::check(todos.value(QStringLiteral("maxItems")).toInt() == AgentConst::kTodoMaxItems,
                       "todo: maxItems == AgentConst::kTodoMaxItems（非硬编码字面量）");

    const QJsonObject item = todos.value(QStringLiteral("items")).toObject();
    TestHarness::check(item.value(QStringLiteral("type")).toString() == QStringLiteral("object"),
                       "todo: items 是 object");
    // items **刻意不写 required**（对齐 lcc：status 缺键由执行侧取默认 pending，见
    // AgentLoopTodo.cpp 的「status 仅在键缺失时取默认 pending」登记）。钉住这个形态：一旦有人
    // 「顺手补齐」成 required:[content,status]，模型提交缺 status 的条目就会从「执行侧兜默认」
    // 变成「服务端 400」，那是行为退化而非修复。
    TestHarness::check(!item.contains(QStringLiteral("required")),
                       "todo: items 不写 required（缺 status 由执行侧兜默认，勿顺手补齐）");
    const QJsonObject itemProps = item.value(QStringLiteral("properties")).toObject();
    TestHarness::check(itemProps.contains(QStringLiteral("content"))
                           && itemProps.contains(QStringLiteral("status")),
                       "todo: items.properties 含 content 与 status");

    const QJsonObject content = itemProps.value(QStringLiteral("content")).toObject();
    TestHarness::check(content.value(QStringLiteral("type")).toString() == QStringLiteral("string"),
                       "todo: content 是 string");
    TestHarness::check(content.value(QStringLiteral("minLength")).toInt() == 1,
                       "todo: content minLength 1（空正文条目会被执行器拒）");

    const QJsonObject status = itemProps.value(QStringLiteral("status")).toObject();
    TestHarness::check(status.value(QStringLiteral("type")).toString() == QStringLiteral("string"),
                       "todo: status 是 string");
    QStringList enumValues;
    for (const QJsonValue &e : status.value(QStringLiteral("enum")).toArray())
        enumValues.append(e.toString());
    TestHarness::check(enumValues == QStringList({QStringLiteral("pending"),
                                                  QStringLiteral("in_progress"),
                                                  QStringLiteral("completed")}),
                       "todo: status enum 恰为 pending/in_progress/completed 且顺序稳定");
}

// ---- 组 6：schema 侧无 default 键（钉住「enum↔default 在 schema 内无配对」这一实测事实）----

void testNoDefaultKey(const QJsonArray &tools)
{
    int defaults = 0;
    for (const QJsonValue &v : tools)
    {
        const QJsonObject props = propsOf(v.toObject());
        for (auto it = props.constBegin(); it != props.constEnd(); ++it)
            if (it.value().toObject().contains(QStringLiteral("default")))
                ++defaults;
    }
    // 本仓「enum↔默认值」的真实对应物是「enum↔运行时默认值」：执行侧缺键时取 pending，
    // 而非 schema 写 default。钉住零出现，未来若加 default 必须是有意识的改动。
    TestHarness::check(defaults == 0, "schema: 全部参数均不写 default 键");
}

// ---- 组 7：additionalProperties:false 的四把锁（含 workflow 委托单源）----

void testAdditionalProperties(const QJsonArray &tools)
{
    for (const QString &name : {ToolNames::CREATE_TASK, ToolNames::UPDATE_TASK,
                                ToolNames::CREATE_WORKTREE, ToolNames::WORKFLOW})
    {
        const QJsonObject tool = toolByName(tools, name);
        TestHarness::check(paramsOf(tool).value(QStringLiteral("additionalProperties")).toBool() == false,
                           QString("addProps: %1 显式 additionalProperties=false")
                               .arg(name).toUtf8().constData());
    }
    // 未列入的四把锁之外不得出现该键（否则是无声收紧模型可用参数面）
    int others = 0;
    for (const QJsonValue &v : tools)
    {
        const QString name = funcName(v.toObject());
        if (name == ToolNames::CREATE_TASK || name == ToolNames::UPDATE_TASK
            || name == ToolNames::CREATE_WORKTREE || name == ToolNames::WORKFLOW)
            continue;
        if (paramsOf(v.toObject()).contains(QStringLiteral("additionalProperties")))
            ++others;
    }
    TestHarness::check(others == 0, "addProps: 仅上述四工具写 additionalProperties");
}

// ---- 组 8：workflow 信封与 WorkflowManager::toolInfo() 同源（防手工二次书写漂移）----

void testWorkflowSingleSource(const QJsonArray &tools)
{
    const WorkflowManager prototype;
    const QJsonObject expected = prototype.toolInfo();
    const QJsonObject actual = toolByName(tools, ToolNames::WORKFLOW);
    TestHarness::check(!expected.isEmpty(), "workflow: toolInfo() 产出非空");
    TestHarness::check(actual == expected,
                       "workflow: schema 内 workflow 信封逐字段等于 WorkflowManager::toolInfo()");
}

// ---- 组 9：pattern ↔ maxLength 自洽，且 required 不含将被 SubAgent 剥掉的键 ----

void testPatternAndSubTools(const QJsonArray &tools)
{
    // create_worktree.name：pattern 允许 1..64，maxLength 必须同界
    const QJsonObject wt = toolByName(tools, ToolNames::CREATE_WORKTREE);
    const QJsonObject name = propsOf(wt).value(QStringLiteral("name")).toObject();
    const QString pattern = name.value(QStringLiteral("pattern")).toString();
    TestHarness::check(name.value(QStringLiteral("type")).toString() == QStringLiteral("string"),
                       "worktree: name 是 string");
    TestHarness::check(pattern.contains(QStringLiteral("{0,63}"))
                           && name.value(QStringLiteral("maxLength")).toInt() == 64,
                       "worktree: pattern {0,63}+首字符 与 maxLength 64 同界");
    TestHarness::check(pattern.contains(QStringLiteral("(?!.*\\.\\.}")) || pattern.contains(QStringLiteral("(?!.*\\.\\.")),
                       "worktree: pattern 内含 .. 负向断言（防目录穿越）");

    // spawn_teammate：task_id / require_plan 必须不在 required —— SubAgent 侧 filterSubTools
    // 会剥参数，required 里留着就会让子代理的请求恒不合法
    const QJsonObject spawn = toolByName(tools, ToolNames::SPAWN_TEAMMATE);
    const QStringList spawnRequired = requiredOf(spawn);
    TestHarness::check(!spawnRequired.contains(QStringLiteral("task_id")),
                       "spawn: task_id 不在 required（与 SubAgent 剥参纪律一致）");
    TestHarness::check(!spawnRequired.contains(QStringLiteral("require_plan")),
                       "spawn: require_plan 不在 required");

    // bash.run_in_background 同理：required 只含 command，SubAgent 删它才不会破坏 schema
    const QJsonObject bash = toolByName(tools, ToolNames::BASH);
    TestHarness::check(requiredOf(bash) == QStringList{QStringLiteral("command")},
                       "bash: required 恰为 [command]（run_in_background 可选）");
    TestHarness::check(propsOf(bash).contains(QStringLiteral("run_in_background")),
                       "bash: properties 含 run_in_background");

    // SubAgent 白名单必须是 schema 产出名的子集，否则子代理拿到一个不存在的工具定义
    const QStringList subWhitelist = {ToolNames::BASH, ToolNames::READ_FILE, ToolNames::WRITE_FILE,
                                      ToolNames::EDIT_FILE, ToolNames::GLOB};
    QSet<QString> produced;
    for (const QJsonValue &v : tools)
        produced.insert(funcName(v.toObject()));
    int absent = 0;
    for (const QString &n : subWhitelist)
        if (!produced.contains(n))
            ++absent;
    TestHarness::check(absent == 0, "subagent: 白名单五个工具全部在 schema 内");
}

// ---- 组 10：惰性缓存语义（同一进程内两次取值必须等价，且不得随调用漂移）----

void testCacheStability(const QJsonArray &tools)
{
    const QJsonArray again = ToolSchema::createToolsDefinition();
    TestHarness::check(again.size() == tools.size(), "cache: 两次取值工具数一致");
    TestHarness::check(again == tools, "cache: 两次取值逐字段一致（无运行期插值）");
}

} // namespace

int tst_toolschema()
{
    const int before = TestHarness::failCount();

    const QJsonArray tools = ToolSchema::createToolsDefinition();
    if (tools.isEmpty())
    {
        TestHarness::check(false, "schema: createToolsDefinition() 产出非空");
        return TestHarness::failCount() - before;
    }

    testEnvelopeAndNames(tools);
    testRequiredSubsetOfProperties(tools);
    testNoRequiredShape(tools);
    testTypeKeywordPairing(tools);
    testTodoWritePairing(tools);
    testNoDefaultKey(tools);
    testAdditionalProperties(tools);
    testWorkflowSingleSource(tools);
    testPatternAndSubTools(tools);
    testCacheStability(tools);

    return TestHarness::failCount() - before;
}
