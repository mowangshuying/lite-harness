#pragma once

// MCP 外部工具机制引擎（lcc s14 7b4ac1c + 1ea152e 移植；规格参照 3rdparty/lcc mcp_manager.py 264 行）
//
// 职责：工具面的「发现期动态组装 + 宿主策略授权」机制单源。模型每轮看到静态内置工具 +
// connect_mcp；连接某 server 后，其工具以 mcp__{server}__{tool} 前缀名进入下一轮工具池，
// 使用前由宿主策略表（而非 server 自述 annotations）决定是否要向用户请求确认。
//
// 与 lcc 的偏差登记（方案裁决：机制层 + 空注册表，不移植模拟）：
//   1. lcc 的 MCPClient 进程内替身与 MOCK_SERVERS（docs/deploy 两个假 server）整体不移植；
//      lite 以 registerServer() 留注册接缝，当前注册表恒空——connect_mcp 对任何名字都返回
//      "Unknown server ..." 错误文本回流给模型，机制全链路可运行、可单测，待真实 MCP 传输层
//      （stdio/JSON-RPC）接入时只需填注册表，其余逻辑零改动。
//   2. lcc 的 CONNECT_TOOL schema 带 enum [docs, deploy]（与 MOCK_SERVERS 手工同步，lcc 自认
//      设计债）；lite 注册表为空无枚举源，schema 刻意不带 enum——偏差即裁决，勿"修回"。
//   3. lcc 的 MCP_HOST_POLICY 含 4 条 mock 条目（docs/search=allow 等）；随 mock 一并弃。
//      lite 策略表机制在、条目空：任何 (server, tool) 键未登记即缺省 confirm（fail-closed），
//      行为与 lcc 对未登记表键完全一致。
//   4. lcc assemble 产物是 Anthropic 形态 {name, description, input_schema}；lite 主循环走
//      OpenAI function 信封 {type:function, function:{name, description, parameters}}——
//      拼名/碰撞/长度/校验逻辑逐字保留，只换最后的外壳翻译。
//   5. lcc 发现期错误 raise ValueError（loop 每轮 assemble 处捕获终止本回合）；lite C++ 无异常
//      惯用法，以 bool 返回值 + error 出参转译，宿主（doStartChatRequest）按 lcc 语义收口。
//      lcc 调用期边界 run_connect_mcp 的 try/except 同理转译为「connect 全路径无失败抛出面，
//      失败皆已折叠为返回字符串」，边界函数保留为 handler 入口。
//   6. lcc connect 的未知名文案 "Available: {MOCK_SERVERS 拼列}"；lite 空注册表回落
//      "Available: (none)"——单源在 connect() 内，登记于此。
//   7. server 自述 annotations（readOnlyHint/destructiveHint 等）在组装期刻意丢弃、绝不进
//      策略判定——lcc 承重语义逐字保留（授权只认宿主表），故 McpToolDef 不设该字段。
//
// 零线程原则同仓规：本类纯内存无 IO；日志走 LiteLog 单源。

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// 与 AgentLoop::ToolHandler 同型（std::function<QString(const QJsonObject&)>），可互赋值。
// 单独 typedef 而不用 AgentLoop 的：本类不依赖 AgentLoop.h（防include环），同型即兼容。
using McpToolHandler = std::function<QString(const QJsonObject &args)>;

// 一个 MCP 工具的服务端登记形态：rawName 是净化前的原始名（策略表键用它，lcc 语义：
// 授权认原始名，不认净化后拼名——防止 foo-bar 与 foo__bar 洗成同名后共用 allow 条目）。
struct McpToolDef
{
    QString rawName;        // server 上报的原始工具名（非空，server 内唯一——注册接缝校验）
    QString description;    // 缺省空串（lcc: tool.get("description", "")）
    QJsonObject parameters; // OpenAI JSON Schema 形态的入参对象（lcc 侧即 inputSchema）
    McpToolHandler handler; // 调用执行体；返回文本回流模型，失败折叠为字符串不抛出
};

// 一个可注册 MCP server 的静态描述（注册接缝的数据形态；当前全仓无调用点——真实传输层
// 接入后由宿主在装配期填充，connect() 按 name 查此表）。
struct McpServerDef
{
    QString name;              // 注册名（connect_mcp 的参数按此匹配，精确字符串）
    QList<McpToolDef> tools;   // 该 server 提供的工具清单
};

class McpManager
{
public:
    McpManager() = default;

    // ---- 注册接缝（方案裁决：机制在、表空。以下两函数当前全仓零调用点，勿删）----

    // 注册/替换一个 server 定义。server 名为空或工具名净化后为空/name 在 server 内重复
    // 时拒绝并返回错误文本（lcc register 四道校验中「handler 缺失 raise」在 C++ 下退化为
    // 未定义 std::function——调用前以 handler 非空把关，折叠于此）。成功返回空串。
    QString registerServer(const McpServerDef &server);

    // ---- 生命周期（lcc connect :97-124 逐字语义）----

    // 连接指定 server 并缓存其工具清单。返回值即交还模型的文本：
    //   已连 → "MCP server 'x' already connected"
    //   未知名 → "Unknown server 'x'. Available: ..."（空注册表回落 "(none)"，偏差6）
    //   成功 → "Connected to MCP server 'x'. Discovered N tools: a, b"（+LOG_INFO [mcp]）
    QString connect(const QString &name);

    // ---- 组装（lcc assemble :126-198 逐字语义；发现期校验全保留）----

    // 依当前连接重算三个产物快照（动态 schema 数组 / handler 表 / 策略快照）。
    // baseToolNames 为本轮基础工具池的名字清单（进 origins 溯源表种子，内置名被 MCP 拼名
    // 碰撞时用于报错文案）。校验失败（净化空名 / 拼名 >64 字符 / 归一后碰撞 / parameters
    // 非 object 或 type≠object）返回 false 且 error 填文案——**此时三个快照保持上一次
    // 成功组装的状态不变**（lcc raise 在替换之前，同语义）。
    // 拼名规则（承重，lcc :141）：MCP_PREFIX + safe(server) + "__" + safe(tool)。
    bool assemble(const QStringList &baseToolNames, QString *error);

    // ---- 产物读取（每轮组装后、请求与执行两侧消费）----

    // 动态工具的 OpenAI 信封数组（空注册表 / 未连接时为空数组）。
    const QJsonArray &dynamicTools() const { return m_dynamicTools; }

    // 按净化拼名查动态 handler；未命中返回空 std::function（调用侧判 ! 后走 Unknown tool）。
    McpToolHandler handlerFor(const QString &prefixedName) const;

    // 权限钩子消费的快照查询（lcc policy_for :200-206）：查不到一律 "confirm"
    // （fail-closed——含从未组装过的幻觉名）。
    QString toolPolicy(const QString &prefixedName) const;

    // ---- 宿主/模型感知通道 ----

    // system/注入块动态段（lcc system_prompt_note :252-262）：无已连 server 返回空串；
    // 有 → "Connected MCP servers: a, b"（只报 server 名单，不报工具）。
    QString systemPromptNote() const;

    // connect_mcp 工具 handler 入口（lcc run_connect_mcp :238-250 的转译，偏差5）：
    // 参数缺失/类型不符的兜底折叠在此（args 由宿主 parseToolCall 保证是对象），name 空
    // 视为缺参错误文本回流。
    QString runConnectMcp(const QString &serverName);

    // ---- 静态工具函数（公开供单测直接调用）----

    // 名字净化（lcc normalize_mcp_name :63-71）：[^a-zA-Z0-9_-] → '_'；结果为空置 error
    // "MCP names cannot normalize to an empty string" 并返回空串。
    static QString normalizeName(const QString &name, QString *error);

    // 宿主策略表查询（lcc MCP_HOST_POLICY :38-52）：键为 (server 原始名, tool 原始名)，
    // 值 "allow"|"confirm"；**未登记键缺省 "confirm"（fail-closed）**。
    // 当前表刻意为空（偏差3）：真实 server 接入时在此登记，授权只认本表、绝不认 server
    // 自述 annotations（承重语义，lcc 组装期丢弃 annotations 的原因）。
    static QString hostPolicy(const QString &serverName, const QString &rawToolName);

private:
    QHash<QString, McpServerDef> m_registry;   // 注册接缝：name → 定义（当前恒空）
    QVector<QString> m_connected;              // 已连 server 名（保序）。组装/查询一律按名现查
                                               // m_registry——不存值副本也不存指针（防 rehash
                                               // 悬垂），lcc clients dict 同构语义。

    QJsonArray m_dynamicTools;                                  // 组装产物快照 1
    QHash<QString, McpToolHandler> m_dynamicHandlers;           // 组装产物快照 2
    QHash<QString, QString> m_toolPolicies;                     // 组装产物快照 3（prefixed → allow|confirm）
};
