#include "McpManager.h"

#include "LiteLog.h"
#include "ToolNames.h"

#include <QJsonArray>
#include <QRegularExpression>

// MCP 机制引擎实现（lcc s14 7b4ac1c + 1ea152e 移植；逐字文案与校验语义对照
// 3rdparty/lcc mcp_manager.py，偏差登记见 McpManager.h 头注释）。

namespace {

// 净化用正则（lcc normalize_mcp_name :63-71 的 re.sub(r"[^a-zA-Z0-9_-]", "_", name)）：
// 静态构造一次，MSVC /W4 下 const 且进程级只读，无线程问题（全仓零线程原则）。
const QRegularExpression &sanitizeRe()
{
    static const QRegularExpression re(QStringLiteral("[^a-zA-Z0-9_-]"));
    return re;
}

// 策略字面量单源（fail-closed 的两个终值；宿主权限钩子比较 "allow" 同源字面量）。
const QLatin1String kPolicyAllow("allow");
const QLatin1String kPolicyConfirm("confirm");

// 拼名总长上限（lcc assemble :143-146：> 64 raise）。
constexpr int kMcpNameMaxLen = 64;

} // namespace

QString McpManager::normalizeName(const QString &name, QString *error)
{
    // lcc :63-71：非白名单字符一律换 '_'；结果空串视为非法（名字全由非法字符组成时）。
    QString safe = name;
    safe.replace(sanitizeRe(), QStringLiteral("_"));
    if (safe.isEmpty())
    {
        if (error)
            *error = QStringLiteral("MCP names cannot normalize to an empty string");
        return QString();
    }
    return safe;
}

QString McpManager::hostPolicy(const QString &serverName, const QString &rawToolName)
{
    // lcc MCP_HOST_POLICY :38-52 的转译：键为 (server 原始名, tool 原始名) → "allow"|"confirm"。
    // 授权只认本表，绝不认 server 自述 annotations（readOnlyHint/destructiveHint 等——组装期
    // 即丢弃，见偏差7）。当前表刻意为空（偏差3：lcc 的 4 条 mock 条目随 mock server 一并弃），
    // 任何未登记键缺省 confirm（fail-closed），与 lcc 对未登记表键的行为逐字一致。
    // 真实 MCP server 接入时在此登记白名单条目，形如：
    //   { qMakePair(QStringLiteral("docs"), QStringLiteral("search")), QStringLiteral("allow") }
    static const QHash<QPair<QString, QString>, QString> policyTable = [] {
        QHash<QPair<QString, QString>, QString> t;
        return t;
    }();

    const auto it = policyTable.constFind(qMakePair(serverName, rawToolName));
    return it == policyTable.constEnd() ? QString(kPolicyConfirm) : it.value();
}

QString McpManager::registerServer(const McpServerDef &server)
{
    // 注册接缝校验（lcc MCPClient.register 四道校验的转译，失败折叠为返回字符串）：
    // 当前全仓零调用点（偏差1：机制在、表空），真实传输层接入时在装配期填充，勿删。
    if (server.name.trimmed().isEmpty())
        return QStringLiteral("MCP server needs a non-empty name");

    QHash<QString, int> seen; // server 内原始名查重（lcc：Duplicate MCP tool name）
    for (const McpToolDef &tool : server.tools)
    {
        if (tool.rawName.isEmpty())
            return QStringLiteral("Every MCP tool needs a non-empty name");
        if (seen.contains(tool.rawName))
            return QStringLiteral("Duplicate MCP tool name on server '%1': '%2'")
                .arg(server.name, tool.rawName);
        seen.insert(tool.rawName, 1);
        if (!tool.handler)
            return QStringLiteral("Missing MCP handler for tool '%1' on server '%2'")
                .arg(tool.rawName, server.name);
    }

    m_registry.insert(server.name, server); // 同名整体替换（lcc clients dict 赋值语义）
    return QString();
}

QString McpManager::connect(const QString &name)
{
    // lcc connect :97-124 逐字文案。
    if (m_connected.contains(name))
        return QStringLiteral("MCP server '%1' already connected").arg(name);

    if (!m_registry.contains(name))
    {
        // 未知名：报可用清单。空注册表回落 "(none)"（偏差6；lcc 拼 MOCK_SERVERS 键列）。
        QStringList available = m_registry.keys();
        available.sort();
        const QString availText =
            available.isEmpty() ? QStringLiteral("(none)") : available.join(QStringLiteral(", "));
        return QStringLiteral("Unknown server '%1'. Available: %2").arg(name, availText);
    }

    m_connected.append(name);
    const McpServerDef &def = m_registry.value(name);
    QStringList toolNames;
    for (const McpToolDef &tool : def.tools)
        toolNames.append(tool.rawName);
    const QString names = toolNames.join(QStringLiteral(", "));
    LOG_INFO << QStringLiteral("[mcp] connected: %1 -> %2").arg(name, names);
    return QStringLiteral("Connected to MCP server '%1'. Discovered %2 tools: %3")
        .arg(name)
        .arg(def.tools.size())
        .arg(names);
}

bool McpManager::assemble(const QStringList &baseToolNames, QString *error)
{
    // lcc assemble :126-198 的转译：发现期校验全保留，任何一项不过即置 error 返回 false，
    // 且**三个产物快照保持上一次成功组装的状态不变**（lcc 的 raise 发生在末尾替换之前）。
    // 名字碰撞检测 origins 溯源表：种子 = 内置工具名（lcc {tool["function"]["name"]:
    // "built-in tool '...'"}——我们的 base 是 OpenAI 信封，取 name 由宿主传入扁平清单，见参数注释）。
    QHash<QString, QString> origins;
    for (const QString &baseName : baseToolNames)
        origins.insert(baseName, QStringLiteral("built-in tool '%1'").arg(baseName));

    QJsonArray newTools;
    QHash<QString, McpToolHandler> newHandlers;
    QHash<QString, QString> newPolicies;

    for (const QString &serverName : m_connected)
    {
        // 一律按名现查 m_registry（不存指针/副本，防 rehash 悬垂）。已连名字查不到定义
        // = 注册表热替换/清空后的边缘态：防御性跳过并留日志（lcc clients 持对象引用无此态）。
        if (!m_registry.contains(serverName))
        {
            LOG_WARN << QStringLiteral("[mcp] assembled server vanished from registry: %1").arg(serverName);
            continue;
        }
        const McpServerDef &def = m_registry.value(serverName);

        QString serverErr;
        const QString safeServer = normalizeName(def.name, &serverErr);
        if (safeServer.isEmpty())
        {
            if (error)
                *error = serverErr;
            return false;
        }

        for (const McpToolDef &tool : def.tools)
        {
            QString toolErr;
            const QString safeTool = normalizeName(tool.rawName, &toolErr);
            if (safeTool.isEmpty())
            {
                if (error)
                    *error = toolErr;
                return false;
            }

            // 拼名规则承重（lcc :141）：MCP_PREFIX + safe(server) + "__" + safe(tool)。
            const QString prefixed =
                ToolNames::MCP_PREFIX + safeServer + QStringLiteral("__") + safeTool;
            if (prefixed.size() > kMcpNameMaxLen)
            {
                if (error)
                    *error = QStringLiteral("MCP tool name is longer than 64 characters: %1").arg(prefixed);
                return false;
            }

            const QString origin =
                QStringLiteral("MCP tool '%1'/'%2'").arg(def.name, tool.rawName);
            if (origins.contains(prefixed))
            {
                // 归一后碰撞：两个不同原始名洗成同一拼名，或撞上内置工具名。静默覆盖
                // 会让调用打到错误出处，必须失败（lcc :152-161）。
                if (error)
                    *error = QStringLiteral("MCP tool name collision after normalization: '%1' maps both %2 and %3")
                                 .arg(prefixed, origins.value(prefixed), origin);
                return false;
            }
            origins.insert(prefixed, origin);

            // parameters 校验（lcc :163-173 inputSchema 校验的 OpenAI 域转译，偏差4）：
            // 必须是 object 形态 JSON Schema；缺 "type" 键视为 object（lcc schema.get("type",
            // "object") 缺省语义），显式非 "object" 拒绝。
            const QString schemaType = tool.parameters.value(QStringLiteral("type")).toString();
            if (!tool.parameters.isEmpty() && !schemaType.isEmpty()
                && schemaType != QLatin1String("object"))
            {
                if (error)
                    *error = QStringLiteral("Invalid input schema for %1").arg(origin);
                return false;
            }
            // 空 parameters 视为无参 object：补标准空 schema，避免下游 properties 缺键。
            QJsonObject parameters = tool.parameters;
            if (parameters.isEmpty())
            {
                parameters[QStringLiteral("type")] = QStringLiteral("object");
                parameters[QStringLiteral("properties")] = QJsonObject();
            }

            // OpenAI function 信封（lcc 产物是 Anthropic {name, description, input_schema}，
            // 此处外壳翻译为 lite 主循环口径，校验逻辑逐字保留——偏差4）。
            QJsonObject fn;
            fn[QStringLiteral("name")] = prefixed;
            fn[QStringLiteral("description")] = tool.description;
            fn[QStringLiteral("parameters")] = parameters;
            QJsonObject entry;
            entry[QStringLiteral("type")] = QStringLiteral("function");
            entry[QStringLiteral("function")] = fn;
            newTools.append(entry);

            // handler 绑定：按值捕获 tool.handler（lcc lambda 默认参数捕获防闭包晚绑定的
            // 同语义——迭代中不引用循环变量别名，快照替换前每条都已定格）。
            newHandlers.insert(prefixed, tool.handler);

            // 策略快照：键用**原始名**对查宿主表（非净化拼名，防 foo-bar/foo__bar 洗同名
            // 共用条目——lcc :38-52 注释的承重语义），值域 allow|confirm，未登记缺省 confirm。
            newPolicies.insert(prefixed, hostPolicy(def.name, tool.rawName));
        }
    }

    // 全部校验通过才整体替换三快照（lcc 末尾原子成对替换；失败路径已在上方 return，快照不动）。
    m_dynamicTools = newTools;
    m_dynamicHandlers = newHandlers;
    m_toolPolicies = newPolicies;
    return true;
}

McpToolHandler McpManager::handlerFor(const QString &prefixedName) const
{
    // 组装产物快照查询：未命中返回空 std::function，调用侧判 ! 后回 Unknown tool。
    return m_dynamicHandlers.value(prefixedName);
}

QString McpManager::toolPolicy(const QString &prefixedName) const
{
    // lcc policy_for :200-206：查快照，查不到一律 confirm（fail-closed——含从未组装过
    // 的幻觉名，也含组装失败回退到空快照后的任意 mcp__ 调用）。
    const auto it = m_toolPolicies.constFind(prefixedName);
    return it == m_toolPolicies.constEnd() ? QString(kPolicyConfirm) : it.value();
}

QString McpManager::systemPromptNote() const
{
    // lcc system_prompt_note :252-262：无已连 server 返回空串（lcc 返回 None，消费侧
    // join 前判空，语义等价）；有则只报 server 名单、保连接序，不报工具清单。
    if (m_connected.isEmpty())
        return QString();
    return QStringLiteral("Connected MCP servers: %1").arg(m_connected.join(QStringLiteral(", ")));
}

QString McpManager::runConnectMcp(const QString &serverName)
{
    // lcc run_connect_mcp :238-250 的转译（偏差5）：lcc 在此边界 try/except 把意外异常
    // 折叠为 "Error: ..." 字符串；lite 的 connect() 全路径失败均已折叠为返回字符串、无抛出
    // 面，边界保留只为 handler 入口兜住「参数缺失」这一类调用期输入错误。
    if (serverName.trimmed().isEmpty())
        return QStringLiteral("Error: connect_mcp requires a non-empty 'name'");
    return connect(serverName);
}
