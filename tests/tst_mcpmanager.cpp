// s14 MCP 工具机制层单测（lcc s14 7b4ac1c 移植对照）：直接 include 生产头，测真实编译产物。
//
// 覆盖：normalizeName 净化与空名错误（含 error nullptr 容错）、hostPolicy 缺省
// fail-closed、registerServer 四道校验、connect 三态文案（含 Available 排序与
// "(none)" 回落）、assemble 拼名/OpenAI 信封/schema 补型/三类错误与快照原子性、
// toolPolicy 快照语义、handlerFor 命中与未命中、systemPromptNote、runConnectMcp 边界。
// 纯引擎零夹具：不依赖 LITE_TEST_TMPROOT，任何环境全量执行。

#include "TestHarness.h"

#include "McpManager.h"
#include "ToolNames.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace {

// 造一个带 handler 的合法工具定义（测试夹具，减少样板）
McpToolDef makeTool(const QString &rawName, const QString &ret = QStringLiteral("ok"),
                    const QJsonObject &parameters = QJsonObject())
{
    McpToolDef t;
    t.rawName = rawName;
    t.parameters = parameters;
    t.handler = [ret](const QJsonObject &) { return ret; };
    return t;
}

// 造一个 object 型 parameters（assemble schema 校验的合法输入）
QJsonObject objectSchema()
{
    QJsonObject p;
    p[QStringLiteral("type")] = QStringLiteral("object");
    p[QStringLiteral("properties")] = QJsonObject{};
    return p;
}

void testNormalizeName()
{
    QString err;
    const QString n = McpManager::normalizeName(QStringLiteral("docs-server.v2 beta"), &err);
    TestHarness::check(n == QStringLiteral("docs-server_v2_beta"),
                       "normalizeName: 非法字符→下划线，合法字符保留");
    TestHarness::check(err.isEmpty(), "normalizeName: 合法名不置 error");

    // 净化后是 "____" 而非空——lcc 只对「净化结果为空串」报错，全下划线合法
    const QString cn = McpManager::normalizeName(QStringLiteral("中文名字"), &err);
    TestHarness::check(cn == QStringLiteral("____") && err.isEmpty(),
                       "normalizeName: 非 ASCII 逐字符替换为下划线");

    const QString e = McpManager::normalizeName(QStringLiteral(""), &err);
    TestHarness::check(e.isEmpty()
                           && err == QStringLiteral("MCP names cannot normalize to an empty string"),
                       "normalizeName: 空名报错文案与 lcc 逐字一致");

    // error 出参传 nullptr 不得崩溃（容错约定）
    TestHarness::check(McpManager::normalizeName(QStringLiteral(""), nullptr).isEmpty(),
                       "normalizeName: error 出参可为 nullptr");
}

void testHostPolicy()
{
    // 空表缺省 confirm（fail-closed）：授权只认宿主策略表，未登记一律询问
    TestHarness::check(McpManager::hostPolicy(QStringLiteral("any"), QStringLiteral("tool"))
                           == QStringLiteral("confirm"),
                       "hostPolicy: 未登记一律 confirm，缺省拒绝直放");
}

void testRegisterServer()
{
    McpManager m;

    McpServerDef bad;
    bad.name = QStringLiteral("  ");
    TestHarness::check(!m.registerServer(bad).isEmpty(), "registerServer: server 名空白报错");

    bad.name = QStringLiteral("docs");
    McpToolDef emptyName;
    emptyName.handler = [](const QJsonObject &) { return QString(); };
    bad.tools.append(emptyName);
    TestHarness::check(m.registerServer(bad).contains(QStringLiteral("non-empty name")),
                       "registerServer: 工具空名报错");

    bad.tools.clear();
    bad.tools.append(makeTool(QStringLiteral("search")));
    bad.tools.append(makeTool(QStringLiteral("search")));
    TestHarness::check(m.registerServer(bad).contains(QStringLiteral("Duplicate")),
                       "registerServer: server 内重名报错");

    bad.tools.clear();
    McpToolDef noHandler;
    noHandler.rawName = QStringLiteral("search");
    bad.tools.append(noHandler); // handler 为空 std::function
    TestHarness::check(m.registerServer(bad).contains(QStringLiteral("Missing MCP handler")),
                       "registerServer: handler 缺失报错");

    bad.tools.clear();
    bad.tools.append(makeTool(QStringLiteral("search")));
    TestHarness::check(m.registerServer(bad).isEmpty(), "registerServer: 合法注册返回空串");
    TestHarness::check(m.connect(QStringLiteral("docs")).startsWith(QStringLiteral("Connected to MCP server")),
                       "registerServer: 注册成功后 connect 可达");
}

void testConnectStates()
{
    McpManager m;
    // 空注册表：Unknown + Available (none)（偏差 6）
    TestHarness::check(m.connect(QStringLiteral("docs"))
                           == QStringLiteral("Unknown server 'docs'. Available: (none)"),
                       "connect: 空注册表回落 Available: (none)");

    McpServerDef beta;
    beta.name = QStringLiteral("beta");
    beta.tools.append(makeTool(QStringLiteral("t1"), QStringLiteral("x")));
    m.registerServer(beta);
    McpServerDef alpha;
    alpha.name = QStringLiteral("alpha");
    m.registerServer(alpha);

    // Available 列表按字典序（注册序 beta→alpha，展示序 alpha, beta）
    TestHarness::check(m.connect(QStringLiteral("zzz"))
                           == QStringLiteral("Unknown server 'zzz'. Available: alpha, beta"),
                       "connect: Unknown 文案按名排序列出可选项");

    TestHarness::check(m.connect(QStringLiteral("alpha"))
                           .startsWith(QStringLiteral("Connected to MCP server 'alpha'")),
                       "connect: 成功文案 Connected + Discovered");
    TestHarness::check(m.connect(QStringLiteral("alpha"))
                           == QStringLiteral("MCP server 'alpha' already connected"),
                       "connect: 重复连接短路 already connected");
}

void testAssembleHappyPath()
{
    McpManager m;
    QStringList base;
    base << QStringLiteral("bash") << QStringLiteral("read_file");

    // 未连接：assemble 成功且增量为空（空注册表逐字节等价现状）
    QString err;
    TestHarness::check(m.assemble(base, &err) && m.dynamicTools().isEmpty(),
                       "assemble: 无连接时增量为空且成功");

    McpServerDef def;
    def.name = QStringLiteral("docs");
    McpToolDef t = makeTool(QStringLiteral("search docs"), QStringLiteral("hit"), objectSchema());
    t.description = QStringLiteral("Search");
    def.tools.append(t);
    def.tools.append(makeTool(QStringLiteral("get_version"), QStringLiteral("v1"))); // 缺 type 视为 object
    m.registerServer(def);
    m.connect(QStringLiteral("docs"));

    TestHarness::check(m.assemble(base, &err) && err.isEmpty(), "assemble: 正常组装成功");
    TestHarness::check(m.dynamicTools().size() == 2, "assemble: 两工具进增量表");

    const QJsonObject tool0 = m.dynamicTools().at(0).toObject();
    TestHarness::check(tool0.value(QStringLiteral("type")).toString() == QStringLiteral("function"),
                       "assemble: OpenAI type=function 信封");
    const QJsonObject fn0 = tool0.value(QStringLiteral("function")).toObject();
    TestHarness::check(fn0.value(QStringLiteral("name")).toString()
                           == QStringLiteral("mcp__docs__search_docs"),
                       "assemble: 拼名 mcp__{server}__{tool}（净化后）");
    TestHarness::check(fn0.value(QStringLiteral("description")).toString() == QStringLiteral("Search"),
                       "assemble: description 透传");
    TestHarness::check(fn0.contains(QStringLiteral("parameters")),
                       "assemble: 字段名为 parameters（Anthropic→OpenAI 转译）");

    const QJsonObject fn1 = m.dynamicTools().at(1).toObject().value(QStringLiteral("function")).toObject();
    TestHarness::check(fn1.value(QStringLiteral("name")).toString()
                           == QStringLiteral("mcp__docs__get_version"),
                       "assemble: 第二工具拼名正确");
    TestHarness::check(fn1.value(QStringLiteral("parameters")).toObject()
                           .value(QStringLiteral("type")).toString()
                           == QStringLiteral("object"),
                       "assemble: 缺 type 视为 object");

    // 策略快照：未登记 → confirm
    TestHarness::check(m.toolPolicy(QStringLiteral("mcp__docs__search_docs")) == QStringLiteral("confirm"),
                       "toolPolicy: 快照缺省 confirm");
    // handler 绑定可调用
    const McpToolHandler h = m.handlerFor(QStringLiteral("mcp__docs__search_docs"));
    TestHarness::check(static_cast<bool>(h) && h(QJsonObject{}) == QStringLiteral("hit"),
                       "handlerFor: 命中并执行返回");
    TestHarness::check(!static_cast<bool>(m.handlerFor(QStringLiteral("mcp__docs__ghost"))),
                       "handlerFor: 未命中返回空 std::function");
    TestHarness::check(!static_cast<bool>(m.handlerFor(QStringLiteral("bash"))),
                       "handlerFor: 内置名不在动态表（不拦截 base 路由）");

    TestHarness::check(m.systemPromptNote() == QStringLiteral("Connected MCP servers: docs"),
                       "systemPromptNote: 已连清单只报 server 名");
}

void testAssembleFailureAtomicity()
{
    McpManager m;
    QStringList base;
    base << QStringLiteral("bash");

    McpServerDef good;
    good.name = QStringLiteral("ok");
    good.tools.append(makeTool(QStringLiteral("t")));
    m.registerServer(good);
    QString err;
    TestHarness::check(m.connect(QStringLiteral("ok")).startsWith(QStringLiteral("Connected")),
                       "assemble 前置: connect 成功");
    TestHarness::check(m.assemble(base, &err), "assemble 前置: 首个 server 组装成功");
    TestHarness::check(m.dynamicTools().size() == 1, "assemble 前置: 增量表 1 项");

    // 超长名触发 >64 错误（server 32 字符 + tool 53 字符 + 前缀与分隔 → 必然 >64）
    McpServerDef long1;
    long1.name = QStringLiteral("very_long_server_name_that_goes_on_past_thirty_chars");
    long1.tools.append(makeTool(QStringLiteral("tool_name_also_really_long_to_break_the_sixty_four_limit")));
    m.registerServer(long1);
    TestHarness::check(m.connect(long1.name).startsWith(QStringLiteral("Connected")),
                       "assemble 前置: 超长名 server 注册并连接");
    err.clear();
    TestHarness::check(!m.assemble(base, &err), "assemble: 超 64 字符名失败");
    TestHarness::check(err.startsWith(QStringLiteral("MCP tool name is longer than 64 characters")),
                       "assemble: 超长错误文案与 lcc 逐字一致");
    TestHarness::check(m.dynamicTools().size() == 1,
                       "assemble 原子性: 失败不触快照（仍为上次成功 1 项）");
    TestHarness::check(m.systemPromptNote().contains(QStringLiteral("ok")),
                       "assemble 原子性: connect 状态不受组装失败影响");

    // 净化后碰撞：两个 server 名净化后同名（"a.b" 与 "a_b" → "a_b"；
    // 注意 '-' 在 lcc 白名单 [a-zA-Z0-9_-] 内、「a-b」净化后不变、判不了撞名）
    McpManager m2;
    McpServerDef s1;
    s1.name = QStringLiteral("a.b");
    s1.tools.append(makeTool(QStringLiteral("x")));
    McpServerDef s2;
    s2.name = QStringLiteral("a_b");
    s2.tools.append(makeTool(QStringLiteral("x")));
    m2.registerServer(s1);
    m2.registerServer(s2);
    m2.connect(QStringLiteral("a.b"));
    m2.connect(QStringLiteral("a_b"));
    err.clear();
    TestHarness::check(!m2.assemble(base, &err)
                           && err.startsWith(QStringLiteral("MCP tool name collision after normalization"))
                           && err.contains(QStringLiteral("maps both")),
                       "assemble: 净化后碰撞报错（lcc 逐字文案）");

    // schema 校验：type 非 object
    McpManager m3;
    McpServerDef s3;
    s3.name = QStringLiteral("s");
    QJsonObject arrType;
    arrType[QStringLiteral("type")] = QStringLiteral("array");
    s3.tools.append(makeTool(QStringLiteral("c"), QStringLiteral("ok"), arrType));
    m3.registerServer(s3);
    m3.connect(QStringLiteral("s"));
    err.clear();
    TestHarness::check(!m3.assemble(base, &err)
                           && err.startsWith(QStringLiteral("Invalid input schema for")),
                       "assemble: parameters.type≠object 报错");

    // 与 base 毒名碰撞：base 里恰好已有同拼名 → origins 种子必须判撞
    McpManager m4;
    McpServerDef s4;
    s4.name = QStringLiteral("p");
    s4.tools.append(makeTool(QStringLiteral("q")));
    m4.registerServer(s4);
    m4.connect(QStringLiteral("p"));
    QStringList poisoned;
    poisoned << QStringLiteral("mcp__p__q");
    err.clear();
    TestHarness::check(!m4.assemble(poisoned, &err) && err.contains(QStringLiteral("built-in tool")),
                       "assemble: 与 base 工具拼名碰撞报错（origins 种子含 built-in 来源）");
}

void testToolPolicyDefaults()
{
    McpManager m;
    // 从未组装过：任何 mcp__ 名都缺省 confirm（fail-closed 的最后一道）
    TestHarness::check(m.toolPolicy(QStringLiteral("mcp__ghost__tool")) == QStringLiteral("confirm"),
                       "toolPolicy: 无快照时任何名缺省 confirm");
    // 空串名同样不为 allow
    TestHarness::check(m.toolPolicy(QString()) == QStringLiteral("confirm"),
                       "toolPolicy: 空名缺省 confirm");
}

void testRunConnectMcp()
{
    McpManager m;
    TestHarness::check(m.runConnectMcp(QStringLiteral("  "))
                           == QStringLiteral("Error: connect_mcp requires a non-empty 'name'"),
                       "runConnectMcp: 空名返回 Error: 前缀（判定为失败交还模型）");
    // 未注册名返回 Unknown server 文案——注意：不带 Error: 前缀，与 lcc run_connect_mcp
    // 逐字一致（lcc 只对异常加 Error:，connect 的 Unknown 是正常返回值回流不终止回合）。
    // isToolFailure 不判失败，属有意保真，勿顺手加前缀。
    TestHarness::check(m.runConnectMcp(QStringLiteral("nope")).startsWith(QStringLiteral("Unknown server")),
                       "runConnectMcp: 未注册名返回 Unknown server 文案（非 Error: 族）");
}

} // namespace

int tst_mcpmanager()
{
    const int before = TestHarness::failCount();
    testNormalizeName();
    testHostPolicy();
    testRegisterServer();
    testConnectStates();
    testAssembleHappyPath();
    testAssembleFailureAtomicity();
    testToolPolicyDefaults();
    testRunConnectMcp();
    return TestHarness::failCount() - before;
}
