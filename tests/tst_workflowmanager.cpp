// tst_workflowmanager.cpp — s16 workflow 纯引擎单测（lcc s16 42a555d 对照）
// 覆盖：身份与校验（stableHash/createRunId/validateRunId/validateMeta）、journal 语义键
//       （agentKey）、极简 schema 校验与填充（validateJsonSchema/fillSchema）、mock runner、
//       模型回复抠 JSON（parseRunnerJson）、注册表与策略与工具装配、预算/上限/任务台账，
//       以及文件产物组（reserveRunId/writeJsonAtomic/readSnapshot/last_run/WfJournal/抢占集合）。
// 纯函数组零夹具、任何环境全跑；文件产物组依赖 ScopedTempRoot 夹具。
// 形制说明（与 tst_messagebus 的整组 SKIP 不同，刻意「纯在前、文件在后」半跳过形）：
// 夹具缺失时仅文件组走 SKIP 通道，纯组断言照常计数——勿改回整组 return 0，
// 否则 s16 纯函数覆盖面会随环境静默蒸发。
// 逐字英文契约串（校验错误文案/注册表 meta/工具 description/通知文案）以 src/WorkflowManager.cpp 为权威。

#include "TestHarness.h"
#include "ScopedTempRoot.h"

#include "WorkflowManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <limits>

namespace {

// ---- 身份：stableHash / createRunId / createTaskId / validateRunId（lcc :68-85）----
void testIdentity()
{
    // sha256 公开测试向量（跨语言基准，验证「全串小写 hex」口径）
    TestHarness::check(
        WorkflowManager::stableHash(QStringLiteral("abc"))
            == QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "stableHash(\"abc\") 应等于 sha256 公开向量全串");
    TestHarness::check(
        WorkflowManager::stableHash(QString())
            == QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "stableHash(\"\") 应等于空串 sha256 公开向量");
    TestHarness::check(WorkflowManager::stableHash(QStringLiteral("dup"))
                           == WorkflowManager::stableHash(QStringLiteral("dup")),
                       "stableHash 同输入必须同输出（进程内确定性）");

    const QString id = WorkflowManager::createRunId(QStringLiteral("review-changes"));
    TestHarness::check(id.startsWith(QStringLiteral("wf_review-changes_")),
                       "createRunId 必须形如 wf_{name}_{16hex}（前缀段）");
    TestHarness::check(id.size() == 3 + 14 + 1 + 16, "createRunId 随机尾段必须恰为 16 字符");
    QString err;
    TestHarness::check(WorkflowManager::validateRunId(id, &err),
                       "createRunId 的产物必须能通过 validateRunId（自洽门）");
    TestHarness::check(
        WorkflowManager::createTaskId(id) == QStringLiteral("local_workflow_") + id,
        "createTaskId 必须逐字 local_workflow_{runId}");

    // 正反例矩阵（lcc RUN_ID_RE :61）
    QString e1;
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("run_0123456789abcdef"), &e1)
                           && e1 == QStringLiteral("invalid workflow runId"),
                       "缺 wf_ 前缀必须拒绝且错误串逐字 invalid workflow runId");
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("wf_a_0123456789ABCDEF"), nullptr),
                       "大写 hex 尾段必须拒绝（[0-9a-f] 严格小写）");
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("wf__0123456789abcdef"), nullptr),
                       "空 name 段必须拒绝（name 首字符必须字母数字）");
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("wf_a_0123456789abcde"), nullptr),
                       "15 位 hex 尾段必须拒绝");
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("wf_a_0123456789abcdef0"), nullptr),
                       "17 位 hex 尾段必须拒绝（任何回溯拆法都不成立）");
    TestHarness::check(!WorkflowManager::validateRunId(QStringLiteral("wf_-a_0123456789abcdef"), nullptr),
                       "name 以 '-' 开头必须拒绝");
    TestHarness::check(WorkflowManager::validateRunId(QStringLiteral("wf_a_0123456789abcdef"), nullptr),
                       "最小合法形态必须放行");
    TestHarness::check(
        WorkflowManager::validateRunId(QStringLiteral("wf_a.b_c-1_0123456789abcdef"), nullptr),
        "name 段允许 . _ -（含内部下划线）必须放行");
    TestHarness::check(
        !WorkflowManager::validateRunId(QStringLiteral("wf_a_0123456789abcdef\n"), nullptr),
        "末尾换行必须拒绝（PCRE $ 放行尾 \\n，lite 以捕获终点对齐补成 fullmatch 等价）");
}

// ---- meta 校验（lcc validate_meta :88-106，逐字错误串）----
void testValidateMeta()
{
    const QString reqErr = QStringLiteral("meta requires `name` and `description`");
    const QString slugErr = QStringLiteral(
        "meta.name must be a 1-64 character slug using letters, numbers, '.', '_', or '-'");
    const QString phasesErr = QStringLiteral("meta.phases must be a list of non-empty strings");

    QString err;
    WfMeta good;
    good.name = QStringLiteral("ok-slug_1.2");
    good.description = QStringLiteral("d");
    good.phases = {QStringLiteral("A"), QStringLiteral("B")};
    TestHarness::check(WorkflowManager::validateMeta(good, &err) && err.isEmpty(),
                       "合法 meta 必须放行且不出错误串");

    WfMeta noName = good; noName.name.clear();
    TestHarness::check(!WorkflowManager::validateMeta(noName, &err) && err == reqErr,
                       "空 name 必须逐字 meta requires `name` and `description`");
    WfMeta noDesc = good; noDesc.description.clear();
    TestHarness::check(!WorkflowManager::validateMeta(noDesc, &err) && err == reqErr,
                       "空 description 必须同一逐字串");
    WfMeta spaceName = good; spaceName.name = QStringLiteral("Bad Name");
    TestHarness::check(!WorkflowManager::validateMeta(spaceName, &err) && err == slugErr,
                       "含空格 name 必须逐字 slug 错误串");
    WfMeta dashName = good; dashName.name = QStringLiteral("-lead");
    TestHarness::check(!WorkflowManager::validateMeta(dashName, &err) && err == slugErr,
                       "'-' 开头 name 必须逐字 slug 错误串");
    WfMeta longName = good; longName.name = QString(65, QLatin1Char('x'));
    TestHarness::check(!WorkflowManager::validateMeta(longName, &err) && err == slugErr,
                       "65 字符 name 必须越界拒绝（1-64 上界）");
    WfMeta noPhases = good; noPhases.phases.clear();
    TestHarness::check(WorkflowManager::validateMeta(noPhases, &err),
                       "phases 空列表=Python 缺键等价，必须放行");
    WfMeta badPhase = good; badPhase.phases = {QStringLiteral("A"), QString()};
    TestHarness::check(!WorkflowManager::validateMeta(badPhase, &err) && err == phasesErr,
                       "phases 含空串必须逐字 phases 错误串");
}

// ---- journal 语义键（lcc WorkflowJournal.key :316-320）----
void testAgentKey()
{
    const QString k1 = WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("L"),
                                                 QStringLiteral("P"), QJsonValue());
    const QString k2 = WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("L"),
                                                 QStringLiteral("P"), QJsonValue());
    TestHarness::check(k1 == k2, "agentKey 同输入必须同键（resume 命中的根）");
    TestHarness::check(k1.startsWith(QStringLiteral("agent-")), "agentKey 必须以 {kind}- 开头");
    const QString tail = k1.mid(6);
    bool tenDigits = tail.size() == 10;
    for (const QChar ch : tail)
        if (!ch.isDigit()) { tenDigits = false; break; }
    TestHarness::check(tenDigits, "agentKey 尾段必须是 10 位补零十进制数（%10^10:010d 转译）");
    TestHarness::check(
        WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("L2"),
                                  QStringLiteral("P"), QJsonValue())
            != k1,
        "label 变化必须换键（basis 含 label）");
    TestHarness::check(
        WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("L"),
                                  QStringLiteral("P2"), QJsonValue())
            != k1,
        "prompt 变化必须换键（basis 含 prompt）");
    TestHarness::check(
        WorkflowManager::agentKey(QStringLiteral("agent"), QStringLiteral("L"),
                                  QStringLiteral("P"), QJsonValue(QJsonObject{}))
            != k1,
        "null schema（\"null\"）与空对象 schema（\"{}\"）必须不同键");
}

// ---- 极简 schema 校验（lcc SimpleJsonSchema :109-152，逐字错误族）----
void testValidateJsonSchema()
{
    QString err;
    TestHarness::check(WorkflowManager::validateJsonSchema(QStringLiteral("x"), QJsonObject{}, &err),
                       "空 schema=无约束，任意值放行");

    const QJsonObject enumSchema{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("high"), QStringLiteral("medium"),
                                            QStringLiteral("low")}}};
    TestHarness::check(WorkflowManager::validateJsonSchema(QStringLiteral("high"), enumSchema, &err),
                       "enum 命中必须放行");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QStringLiteral("nope"), enumSchema, &err)
            && err == QStringLiteral("expected one of [\"high\",\"medium\",\"low\"]"),
        "enum 未命中必须逐字 expected one of + JSON 紧凑数组文本（Python repr 转译，偏差⑦）");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QJsonValue(5), enumSchema, &err)
            && err.startsWith(QStringLiteral("expected one of")),
        "enum 必须先于 type 判定（数字不走 expected string）");

    const QJsonObject objSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("score"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}}};
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QStringLiteral("s"), objSchema, &err)
            && err == QStringLiteral("expected object"),
        "非对象走 expected object");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QJsonObject{}, objSchema, &err)
            && err == QStringLiteral("missing required key 'name'"),
        "缺必填键走逐字 missing required key 'name'");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(
            QJsonObject{{QStringLiteral("name"), QStringLiteral("v")},
                        {QStringLiteral("score"), QStringLiteral("bad")}},
            objSchema, &err)
            && err == QStringLiteral("score: expected number"),
        "嵌套属性错误必须逐字 {key}: {子错误}");
    TestHarness::check(
        WorkflowManager::validateJsonSchema(
            QJsonObject{{QStringLiteral("name"), QStringLiteral("v")},
                        {QStringLiteral("score"), 1.5}},
            objSchema, &err),
        "合法对象必须放行");

    const QJsonObject arrSchema{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QStringLiteral("s"), arrSchema, &err)
            && err == QStringLiteral("expected array"),
        "非数组走 expected array");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(
            QJsonArray{QStringLiteral("ok"), 5.0}, arrSchema, &err)
            && err == QStringLiteral("[1]: expected string"),
        "元素错误必须逐字 [{i}]: {子错误}");

    TestHarness::check(
        !WorkflowManager::validateJsonSchema(5.0,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}, &err)
            && err == QStringLiteral("expected string"),
        "数字对 string 走 expected string");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QStringLiteral("t"),
            QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}, &err)
            && err == QStringLiteral("expected boolean"),
        "字符串对 boolean 走 expected boolean");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(QStringLiteral("t"),
            QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}, &err)
            && err == QStringLiteral("expected number"),
        "字符串对 number 走 expected number");
    TestHarness::check(
        !WorkflowManager::validateJsonSchema(true,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}, &err)
            && err == QStringLiteral("expected number"),
        "布尔对 number 必须拒绝（Python isinstance bool 排除，QJsonValue 类型分离天然等价）");
    TestHarness::check(
        WorkflowManager::validateJsonSchema(3.0,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}, nullptr),
        "integer 按 number 处理放行，且 error=nullptr 安全");
    TestHarness::check(
        WorkflowManager::validateJsonSchema(QStringLiteral("x"),
            QJsonObject{{QStringLiteral("type"), QStringLiteral("weird")}}, nullptr),
        "未知 type 必须放行（lcc :152 兜底）");
    TestHarness::check(
        WorkflowManager::validateJsonSchema(QJsonArray{1.0},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}}, nullptr),
        "array 无 items 不校验元素，放行");
}

// ---- 确定性填充（lcc fill_schema :155-167）----
void testFillSchema()
{
    TestHarness::check(
        WorkflowManager::fillSchema(QJsonObject{}, QStringLiteral("root/key"))
            == QJsonValue(QStringLiteral("key")),
        "无 type 叶子必须回 seed 末段");
    const QJsonValue reqOnly = WorkflowManager::fillSchema(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")}}},
        QStringLiteral("s"));
    TestHarness::check(
        reqOnly.toObject().size() == 2 && reqOnly.toObject().contains(QStringLiteral("a"))
            && reqOnly.toObject().contains(QStringLiteral("b")),
        "object 键集必须来自 required（required 空则回落 properties）");
    TestHarness::check(
        reqOnly.toObject().value(QStringLiteral("a")) == QJsonValue(QStringLiteral("a")),
        "required 键不在 properties：防御降级为空 schema→叶子=键名（Python KeyError 转译，偏差⑾）");
    const QJsonValue driven = WorkflowManager::fillSchema(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("k"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}}}},
        QStringLiteral("x"));
    TestHarness::check(driven.toObject().contains(QStringLiteral("k")),
                       "properties 驱动键集必须生成该键");
    TestHarness::check(
        WorkflowManager::fillSchema(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                        {QStringLiteral("properties"), QJsonObject{
                            {QStringLiteral("k"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}}}},
            QStringLiteral("x"))
            == driven,
        "同 schema 同 seed 两次填充必须逐值相等（确定性）");
    TestHarness::check(
        WorkflowManager::fillSchema(QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}},
                                    QStringLiteral("seed/b")).isBool(),
        "boolean 槽必须产布尔");
    const QJsonValue num = WorkflowManager::fillSchema(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}, QStringLiteral("seed/n"));
    TestHarness::check(num.isDouble() && num.toDouble() >= 0.0 && num.toDouble() <= 4.0,
                       "number 槽必须产 0..4（hash%5）");
    const QJsonValue arr = WorkflowManager::fillSchema(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("items"), QJsonObject{}}},
        QStringLiteral("s"));
    TestHarness::check(arr.toArray().size() == 1, "array 必须恰产单元素");
    TestHarness::check(arr.toArray().at(0) == QJsonValue(QStringLiteral("0")),
                       "数组元素的 seed 末段必须是 0（seed/0）");
    const QJsonValue nested = WorkflowManager::fillSchema(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("child")}},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("child"), QJsonObject{
                            {QStringLiteral("type"), QStringLiteral("object")},
                            {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}},
                            {QStringLiteral("properties"), QJsonObject{
                                {QStringLiteral("name"), QJsonObject{}}}}}}}}},
        QStringLiteral("s"));
    TestHarness::check(
        nested.toObject().value(QStringLiteral("child")).toObject()
                .value(QStringLiteral("name"))
            == QJsonValue(QStringLiteral("name")),
        "嵌套对象逐层 seed 追加 /{k}，叶子取全链末段");
}

// ---- mock runner（lcc MockAgentRunner :177-205）----
void testMockRun()
{
    qint64 tokens = -1;
    const QJsonValue bare = WorkflowManager::mockRun(QStringLiteral("hello prompt"),
                                                     QJsonObject{}, QString(), &tokens);
    TestHarness::check(bare.isString() && bare.toString().startsWith(QStringLiteral("[mock] "))
                           && bare.toString().contains(QStringLiteral("hello prompt")),
                       "空 schema 必须产 [mock] + prompt 文本串（偏差⑳）");
    TestHarness::check(
        WorkflowManager::mockRun(QStringLiteral("p"), QJsonObject{}, QStringLiteral("lab"), &tokens)
            == QJsonValue(QStringLiteral("[mock] lab")),
        "label 非空必须优先于 prompt");
    TestHarness::check(
        WorkflowManager::mockRun(QStringLiteral("p"), QJsonObject{},
                                 QString(80, QLatin1Char('L')), &tokens)
                .toString()
                .size()
            == 7 + 60,
        "head 必须截 60（lcc [:60]，偏差⑳）");

    const QJsonObject findingsSchema{{QStringLiteral("properties"),
                                     QJsonObject{{QStringLiteral("findings"), QJsonObject{}}}}};
    const QJsonValue found = WorkflowManager::mockRun(QStringLiteral("prompt A"),
                                                      findingsSchema, QString(), &tokens);
    TestHarness::check(found.isObject() && found.toObject().contains(QStringLiteral("findings")),
                       "properties 含 findings 必须走 findings 分支");
    const QJsonArray list = found.toObject().value(QStringLiteral("findings")).toArray();
    TestHarness::check(list.size() == 1 || list.size() == 2, "findings 条数必须 1+hash%2 ∈ {1,2}");
    const QJsonObject f0 = list.at(0).toObject();
    TestHarness::check(f0.value(QStringLiteral("title")).toString().startsWith(QStringLiteral("audit #")),
                       "label 缺省 title 基准必须是 audit（lcc :191）");
    const QString sev = f0.value(QStringLiteral("severity")).toString();
    TestHarness::check(sev == QStringLiteral("high") || sev == QStringLiteral("medium")
                           || sev == QStringLiteral("low"),
                       "severity 必须落在 high/medium/low 域");

    const QJsonObject verdictSchema{{QStringLiteral("properties"),
                                    QJsonObject{{QStringLiteral("isReal"), QJsonObject{}}}}};
    const QJsonValue verdict = WorkflowManager::mockRun(QStringLiteral("prompt B"),
                                                        verdictSchema, QString(), &tokens);
    TestHarness::check(verdict.toObject().value(QStringLiteral("isReal")).isBool(),
                       "isReal 分支必须产布尔判定");
    const QString reason = verdict.toObject().value(QStringLiteral("reason")).toString();
    TestHarness::check(reason == QStringLiteral("reproduced")
                           || reason == QStringLiteral("could not reproduce"),
                       "reason 必须逐字二选一（lcc :198）");

    const QJsonValue generic = WorkflowManager::mockRun(
        QStringLiteral("prompt C"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("k")}}},
        QString(), &tokens);
    TestHarness::check(generic.toObject().contains(QStringLiteral("k")),
                       "无特征键 schema 必须回落 fillSchema 通用填充");
    TestHarness::check(tokens > 0, "tokensOut 必须为正（prompt/4 + 序列化文本/4）");
    qint64 tokens2 = -1;
    WorkflowManager::mockRun(QStringLiteral("prompt C"),
                             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                         {QStringLiteral("required"), QJsonArray{QStringLiteral("k")}}},
                             QString(), &tokens2);
    TestHarness::check(tokens2 == tokens, "同输入 tokens 必须可复现（journal 回放的前提）");
}

// ---- 模型回复抠 JSON（lcc parse_runner_json :217-239）----
void testParseRunnerJson()
{
    bool ok = false;
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("{\"a\": 1}"), &ok).isObject() && ok,
        "裸对象必须直接放行");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("```json\n{\"a\": 1}\n```"), &ok)
                .toObject()
                .value(QStringLiteral("a"))
                .toDouble()
            == 1.0
            && ok,
        "``` 围栏（首行语言标签+尾行闭合）必须剥离后解析成功");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("```json\n{\"a\": 1}\nfoo"), &ok).isObject()
            && ok,
        "围栏缺失闭合行时仍须经逐 '{' 候选扫描救回对象");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("Sure! Here:\n{\"a\": 1}"), &ok)
                .toObject()
                .value(QStringLiteral("a"))
                .toDouble()
            == 1.0
            && ok,
        "前置噪声文本必须抠出第一个 JSON 对象（raw_decode 等价）");
    const QJsonValue tail =
        WorkflowManager::parseRunnerJson(QStringLiteral("{\"a\": 1} trailing words"), &ok);
    TestHarness::check(ok && tail.isObject() && !tail.toObject().contains(QStringLiteral("b")),
                       "尾噪声容忍（raw_decode 止于对象终点），且不得吃进后文");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("{\"a\":1} mid {\"b\":2}"), &ok)
                .toObject()
                .value(QStringLiteral("a"))
                .toDouble()
            == 1.0
            && ok,
        "两个候选对象必须首个胜出（逐位置扫描序）");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("[1, 2]"), &ok).isArray() && ok,
        "顶层数组整串解析必须放行");
    TestHarness::check(
        WorkflowManager::parseRunnerJson(QStringLiteral("42"), &ok).toDouble() == 42.0 && ok,
        "顶层标量必须放行（json.loads 任意顶层值等价，偏差⑨包裹法）");
    QString err;
    TestHarness::check(WorkflowManager::parseRunnerJson(QString(), &ok, &err).isUndefined() && !ok,
                       "空文本必须判失败");
    const QJsonValue dead =
        WorkflowManager::parseRunnerJson(QStringLiteral("no braces here"), &ok, &err);
    TestHarness::check(!ok && err == QStringLiteral("workflow agent returned invalid JSON"),
                       "全败必须逐字 workflow agent returned invalid JSON（偏差⑰出参形）");
    TestHarness::check(dead.isUndefined(), "全败返回值必须是空 QJsonValue 占位");
    TestHarness::check(WorkflowManager::parseRunnerJson(QStringLiteral("garbage"), nullptr).isUndefined(),
                       "ok/error 双出参 nullptr 必须安全");
}

// ---- 注册表 / 策略 / 注入提示 / 工具装配（lcc :578-655）----
void testRegistryPolicyNoteTool()
{
    WorkflowManager mgr;
    TestHarness::check(mgr.workflowNames().size() == 1
                           && mgr.workflowNames().contains(QStringLiteral("review-changes")),
                       "内置注册表必须恰含 review-changes（lcc :622-624）");
    TestHarness::check(mgr.hasWorkflow(QStringLiteral("review-changes")),
                       "hasWorkflow 命中必须为真");
    TestHarness::check(!mgr.hasWorkflow(QStringLiteral("ghost")), "查无工作流必须为假");
    const WfMeta meta = mgr.metaFor(QStringLiteral("review-changes"));
    TestHarness::check(
        meta.description
            == QStringLiteral("Review changed files across dimensions, verify each finding"),
        "review-changes description 必须逐字（进注入提示）");
    TestHarness::check(meta.phases == QStringList{QStringLiteral("Review"), QStringLiteral("Verify")},
                       "phases 必须逐字 Review/Verify（lcc :530）");
    TestHarness::check(mgr.metaFor(QStringLiteral("ghost")).name.isEmpty(),
                       "查无名必须回空 meta（不抛不崩）");
    TestHarness::check(mgr.policyFor(QStringLiteral("review-changes")) == QStringLiteral("allow"),
                       "策略表命中必须 allow（lcc :586-588）");
    TestHarness::check(mgr.policyFor(QStringLiteral("unlisted")) == QStringLiteral("confirm"),
                       "未登记工作流必须缺省 confirm（fail-closed）");
    TestHarness::check(mgr.policyFor(QString()) == QStringLiteral("confirm"),
                       "空名必须 confirm（lcc :651 非 str/空串同形）");
    TestHarness::check(
        mgr.systemPromptNote()
            == QStringLiteral(
                   "Registered workflows (call the workflow tool with one of these names):\n"
                   "- review-changes: Review changed files across dimensions, verify each finding"),
        "注入提示必须逐字（头句 + 单条 '- name: description'，无尾换行）");

    const QJsonObject info = mgr.toolInfo();
    TestHarness::check(info.value(QStringLiteral("type")).toString() == QStringLiteral("function"),
                       "信封必须为 OpenAI function 形（Anthropic 转译，偏差㉒）");
    const QJsonObject fn = info.value(QStringLiteral("function")).toObject();
    TestHarness::check(fn.value(QStringLiteral("name")).toString() == QStringLiteral("workflow"),
                       "function.name 必须 workflow（名入 ToolNames.h 属 Lane C 装配层）");
    TestHarness::check(
        fn.value(QStringLiteral("description")).toString()
            == QStringLiteral(
                   "Run a saved workflow by name. The orchestration script lives in the host "
                   "registry; you only pick the workflow and pass its input in args. "
                   "Pass resume_from_run_id to reuse a previous run's journal."),
        "工具 description 必须逐字（lcc :597-601）");
    const QJsonObject params = fn.value(QStringLiteral("parameters")).toObject();
    TestHarness::check(params.value(QStringLiteral("required")).toArray()
                           == QJsonArray{QStringLiteral("name")},
                       "required 必须恰为 [name]");
    TestHarness::check(params.value(QStringLiteral("additionalProperties")) == QJsonValue(false),
                       "additionalProperties 必须 false（模型塞不进元数据）");
    const QJsonObject props = params.value(QStringLiteral("properties")).toObject();
    TestHarness::check(
        props.size() == 3
            && props.value(QStringLiteral("name")).toObject().value(QStringLiteral("type"))
                   .toString()
                   == QStringLiteral("string")
            && props.value(QStringLiteral("args")).toObject().value(QStringLiteral("type"))
                   .toString()
                   == QStringLiteral("object")
            && props.value(QStringLiteral("resume_from_run_id")).toObject()
                   .value(QStringLiteral("type"))
                   .toString()
                   == QStringLiteral("string"),
        "参数表必须恰 name/args/resume_from_run_id 三键且类型 string/object/string");
}

// ---- responseText（lcc response_text :208-214 的 lite 转译=trim 收口）----
void testResponseText()
{
    TestHarness::check(
        WorkflowManager::responseText(QStringLiteral("\n\n hi \n")) == QStringLiteral("hi"),
        "responseText 必须去首尾空白");
    TestHarness::check(
        WorkflowManager::responseText(QStringLiteral("keep  inner  spaces"))
            == QStringLiteral("keep  inner  spaces"),
        "内部空白不得被触碰（只收口首尾）");
}

// ---- 预算 / agent 上限 / 任务台账（lcc Budget :334-353 / ExecutionLimits :378-388 /
//      LocalWorkflowTask :356-375 / serialize_task :497-507；纯内存组，零夹具）----
void testBudgetLimitsTask()
{
    QString err;
    WfBudget inf;
    TestHarness::check(inf.total() == -1, "缺省构造必须 total=-1（Python None 转译=不限）");
    TestHarness::check(inf.add(3, &err) && inf.spent() == 3, "不限预算 add 必须放行入账");
    WfBudget tight(5);
    TestHarness::check(tight.add(3, &err), "5 预算先花 3 必须放行");
    TestHarness::check(!tight.add(3, &err)
                           && err == QStringLiteral("token budget exceeded (6 > 5)"),
                       "超支必须逐字 token budget exceeded (6 > 5)");
    TestHarness::check(tight.spent() == 3, "超支那次绝不入账（lcc raise 先于 +=）");
    TestHarness::check(tight.remaining() == 2, "有限预算 remaining=max(0,total-spent)");
    TestHarness::check(inf.remaining() == std::numeric_limits<qint64>::max(),
                       "不限预算 remaining 必须回 int64 上限（float('inf') 转译）");
    TestHarness::check(inf.add(std::numeric_limits<qint64>::max() / 2, &err)
                           && inf.spent() == 3 + std::numeric_limits<qint64>::max() / 2,
                       "不限预算大额入账必须放行且累计");

    WfExecutionLimits limits;
    bool allOk = true;
    for (int i = 0; i < kWorkflowAgentCap; ++i)
        if (!limits.claimAgent(&err)) { allOk = false; break; }
    TestHarness::check(allOk && limits.agentsUsed() == kWorkflowAgentCap,
                       "1000 次 claim 必须全放行（cap 含第 1000）");
    TestHarness::check(!limits.claimAgent(&err)
                           && err == QStringLiteral("agent() cap reached (1000)"),
                       "第 1001 次必须逐字 agent() cap reached (1000)");
    TestHarness::check(limits.agentsUsed() == kWorkflowAgentCap + 1,
                       "失败时计数仍 +1（lcc 先加后判 :386-388 等价）");

    WfTask task;
    task.taskId = QStringLiteral("local_workflow_wf_a_0123456789abcdef");
    task.runId = QStringLiteral("wf_a_0123456789abcdef");
    task.workflowName = QStringLiteral("review-changes");
    task.progressEvent(QStringLiteral("start"), QJsonObject{});
    TestHarness::check(task.progress.size() == 1
                           && task.progress.at(0).toObject().value(QStringLiteral("type"))
                                  .toString()
                                  == QStringLiteral("start"),
                       "progressEvent 必须入册 {type,...}（lcc :373）");
    task.progressEvent(QStringLiteral("phase"),
                       QJsonObject{{QStringLiteral("phase"), QStringLiteral("Review")}});
    TestHarness::check(
        task.progress.at(1).toObject().value(QStringLiteral("phase")).toString()
            == QStringLiteral("Review"),
        "progressEvent 的 data 必须并入条目");
    task.progressEvent(QStringLiteral("x"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("y")}});
    TestHarness::check(
        task.progress.at(2).toObject().value(QStringLiteral("type")).toString()
            == QStringLiteral("y"),
        "data 携 type 时覆盖首参（Python {\"type\":t,**data} 同形）");
    task.event(QStringLiteral("tick"), {QStringLiteral("a=1")});   // 纯日志，不应扰动台账
    TestHarness::check(task.progress.size() == 3, "event 只打日志不追加 progress");
    task.agents = 2;
    task.tokens = 1234;

    WorkflowManager mgr;
    const QJsonObject snap = mgr.serializeTask(task);
    TestHarness::check(
        snap.value(QStringLiteral("taskId")).toString() == task.taskId
            && snap.value(QStringLiteral("runId")).toString() == task.runId
            && snap.value(QStringLiteral("workflowName")).toString() == task.workflowName,
        "serializeTask 身份三键必须逐值透传");
    TestHarness::check(
        snap.value(QStringLiteral("taskType")).toString() == QStringLiteral("local_workflow")
            && snap.value(QStringLiteral("status")).toString() == QStringLiteral("running"),
        "taskType 必须逐字 local_workflow；status 缺省 running");
    const QJsonObject usage = snap.value(QStringLiteral("usage")).toObject();
    TestHarness::check(usage.value(QStringLiteral("agents")) == QJsonValue(2)
                           && usage.value(QStringLiteral("tokens")) == QJsonValue(1234.0),
                       "usage 必须 {agents,tokens} 双键（flatten，偏差⑿）");
    TestHarness::check(snap.value(QStringLiteral("progress")).toArray().size() == 3,
                       "progress 必须整体透传");
}

// ============ 文件产物组（ScopedTempRoot 夹具门后执行）============

void testFileReserve(WorkflowManager &mgr, const QString &store)
{
    QString err;
    QString id;
    TestHarness::check(mgr.reserveRunId(QStringLiteral("review-changes"), &id, &err),
                       "name 形 reserve 必须成功");
    TestHarness::check(WorkflowManager::validateRunId(id, nullptr),
                       "预留出的 id 必须自过 validateRunId");
    TestHarness::check(QFile::exists(store + QLatin1Char('/') + id + QStringLiteral(".json")),
                       "reserve 必须以独占新建落 {id}.json 占位文件");
    // 撞名换新：拿已占用的 id 当候选再预留 → 必须换个新 id、原文件不动
    QString candidate = id;
    TestHarness::check(mgr.reserveRunId(&candidate, &err) && candidate != id,
                       "候选撞名必须换新随机 id 且出参回传实际名");
    TestHarness::check(WorkflowManager::validateRunId(candidate, nullptr)
                           && QFile::exists(store + QLatin1Char('/') + candidate
                                            + QStringLiteral(".json"))
                           && QFile::exists(store + QLatin1Char('/') + id + QStringLiteral(".json")),
                       "新旧两个占位文件必须并存");
    QString bad = QStringLiteral("bogus");
    TestHarness::check(!mgr.reserveRunId(&bad, &err)
                           && err == QStringLiteral("invalid workflow runId"),
                       "非法候选必须直判 invalid workflow runId（lcc validate raise 形，不重试）");
    TestHarness::check(!QFile::exists(store + QLatin1Char('/') + QStringLiteral("bogus.json")),
                       "非法候选不得落盘");
    WorkflowManager homeless;   // 未 setStoreDir
    TestHarness::check(!homeless.reserveRunId(QStringLiteral("review-changes"), &candidate, &err)
                           && err == QStringLiteral("workflow store dir is not configured"),
                       "storeDir 未注入必须 lite-only 守卫串拒绝（非 lcc 串，偏差④登记）");
}

void testFileSnapshot(WorkflowManager &mgr, const QString &store)
{
    Q_UNUSED(mgr);
    QString err;
    const QJsonObject obj{{QStringLiteral("runId"), QStringLiteral("snap-run")},
                          {QStringLiteral("status"), QStringLiteral("completed")}};
    TestHarness::check(WorkflowManager::writeJsonAtomic(
                           store + QLatin1Char('/') + QStringLiteral("snap-run.json"), obj),
                       "writeJsonAtomic 必须成功（QSaveFile 原子形）");
    TestHarness::check(QFile::exists(store + QLatin1Char('/') + QStringLiteral("snap-run.json")),
                       "目标文件必须存在且无 .tmp 残留路径语义（commit 即改名）");
    TestHarness::check(WorkflowManager::readSnapshot(store, QStringLiteral("snap-run"), &err) == obj,
                       "快照往返必须逐值相等");
    TestHarness::check(
        WorkflowManager::readSnapshot(store, QStringLiteral("ghost-run"), &err).isEmpty()
            && err == QStringLiteral("resume snapshot not found for ghost-run"),
        "缺快照必须逐字 resume snapshot not found for {runId}");
    {   // 手工造坏文件：非 JSON 文本
        QFile f(store + QLatin1Char('/') + QStringLiteral("corrupt-run.json"));
        if (f.open(QIODevice::WriteOnly)) { f.write("not json"); f.close(); }
    }
    const QJsonObject corrupt =
        WorkflowManager::readSnapshot(store, QStringLiteral("corrupt-run"), &err);
    TestHarness::check(corrupt.isEmpty()
                           && err == QStringLiteral("invalid resume snapshot for corrupt-run"),
                       "坏快照必须逐字 invalid resume snapshot for {runId}");
    {   // 顶层数组也判 invalid（Python 非 dict 同判）
        QFile f(store + QLatin1Char('/') + QStringLiteral("arr-run.json"));
        if (f.open(QIODevice::WriteOnly)) { f.write("[1,2]"); f.close(); }
    }
    TestHarness::check(
        WorkflowManager::readSnapshot(store, QStringLiteral("arr-run"), nullptr).isEmpty(),
        "数组快照必须判非法回空对象（error=nullptr 安全）");
}

void testFileLastRun(WorkflowManager &mgr, const QString &store)
{
    const QString id = mgr.readLastRun().isEmpty() ? QStringLiteral("wf_last_0123456789abcdef")
                                                   : mgr.readLastRun();
    mgr.saveLastRun(id);
    TestHarness::check(mgr.readLastRun() == id, "readLastRun 内存值必须优先且逐字回读");
    TestHarness::check(
        QFile::exists(store + QLatin1Char('/') + QStringLiteral("last_run.txt")),
        "saveLastRun 必须落 last_run.txt 裸文本");
    WorkflowManager fresh;
    fresh.setStoreDir(store);
    TestHarness::check(fresh.readLastRun() == id, "新实例必须能从盘读回（进程内优先、其次磁盘）");
    WorkflowManager homeless;
    homeless.saveLastRun(QStringLiteral("wf_memo_0123456789abcdef"));
    TestHarness::check(homeless.readLastRun() == QStringLiteral("wf_memo_0123456789abcdef"),
                       "未配置 storeDir 时 saveLastRun 只落内存不得崩");
    TestHarness::check(WorkflowManager().readLastRun().isEmpty(),
                       "全新实例（无 storeDir）readLastRun 必须回空串（None 转译）");
}

void testFileJournal(const QString &store)
{
    QString err;
    {
        WfJournal journal;
        TestHarness::check(journal.open(store, QStringLiteral("jr"), false, &err),
                           "新建 journal 必须成功（截断形）");
        journal.record(QStringLiteral("k1"), QJsonObject{{QStringLiteral("n"), 1}});
        QJsonValue out;
        TestHarness::check(journal.cached(QStringLiteral("k1"), &out)
                               && out == QJsonValue(QJsonObject{{QStringLiteral("n"), 1}}),
                           "record 后同会话 cached 必须命中且值相等");
        TestHarness::check(!journal.cached(QStringLiteral("nope"), nullptr),
                           "未记录 key 必须 MISS=false（哨兵转译，偏差⑮）");
        journal.record(QStringLiteral("k2"), QStringLiteral("v2"));
    }   // 作用域结束 = RAII close
    {
        QFile file(store + QLatin1Char('/') + QStringLiteral("jr.journal.jsonl"));
        TestHarness::check(file.open(QIODevice::ReadOnly), "journal 文件必须落盘为 jsonl");
        const QList<QByteArray> lines = file.readAll().split('\n');
        const QJsonDocument doc = QJsonDocument::fromJson(lines.at(0));
        TestHarness::check(!doc.isNull() && doc.object().contains(QStringLiteral("key"))
                               && doc.object().contains(QStringLiteral("value")),
                           "每行必须恰为 {key,value} 单行 JSON");
        TestHarness::check(lines.at(1).startsWith("{\"key\":\"k2\""),
                           "第二条记录必须逐行追加（jsonl 语义）");
    }
    {
        WfJournal resumed;
        TestHarness::check(resumed.open(store, QStringLiteral("jr"), true, &err),
                           "resume 已有 journal 必须成功");
        QJsonValue out;
        TestHarness::check(resumed.cached(QStringLiteral("k1"), &out)
                               && resumed.cached(QStringLiteral("k2"), nullptr),
                           "resume 必须回放全部缓存");
        resumed.record(QStringLiteral("k3"), 3.0);
    }
    {
        WfJournal again;
        TestHarness::check(again.open(store, QStringLiteral("jr"), true, &err)
                               && again.cached(QStringLiteral("k3"), nullptr),
                           "resume 后追加的 k3 必须二次可见（append 生效）");
        again.close();
        WfJournal truncated;
        TestHarness::check(truncated.open(store, QStringLiteral("jr"), false, &err)
                               && !truncated.cached(QStringLiteral("k1"), nullptr),
                           "非 resume 重开必须截断（新运行不回放，lcc 'w' :314）");
    }
    {
        WfJournal ghost;
        TestHarness::check(!ghost.open(store, QStringLiteral("ghost-journal"), true, &err)
                               && err == QStringLiteral("resume journal not found for ghost-journal"),
                           "resume 缺文件必须逐字 resume journal not found for {runId}");
    }
    {   // 手工造坏行账本：第 1 行合法、第 2 行烂 → 逐行 fail-closed
        QFile f(store + QLatin1Char('/') + QStringLiteral("bad.journal.jsonl"));
        if (f.open(QIODevice::WriteOnly)) {
            f.write("{\"key\":\"a\",\"value\":1}\ngarbage\n");
            f.close();
        }
        WfJournal bad;
        TestHarness::check(!bad.open(store, QStringLiteral("bad"), true, &err)
                               && err == QStringLiteral("invalid resume journal record at line 2"),
                           "坏行必须逐字 invalid resume journal record at line {n}（行号 1 基）");
    }
    {
        WfJournal j;
        j.record(QStringLiteral("silent"), 1.0);   // 未 open：静默丢弃防御
        TestHarness::check(!j.cached(QStringLiteral("silent"), nullptr),
                           "未 open 时 record 必须静默不崩、不入缓存");
        j.open(store, QStringLiteral("nl"), false, &err);
        const QJsonValue nullVal =
            QJsonDocument::fromJson("{\"a\":null}").object().value(QStringLiteral("a"));
        j.record(QStringLiteral("kn"), nullVal);
        QJsonValue out;
        TestHarness::check(j.cached(QStringLiteral("kn"), &out) && out.isNull(),
                           "合法 null 值必须可缓存命中（MISS 哨兵转译不吞 null）");
        j.close();
    }
}

void testFileActiveRuns(WorkflowManager &mgr)
{
    TestHarness::check(mgr.tryAcquireRunActive(QStringLiteral("ra")), "空闲 runId 必须抢占成功");
    TestHarness::check(!mgr.tryAcquireRunActive(QStringLiteral("ra")),
                       "active 中重复抢占必须失败（lcc 两层锁折叠为进程内集合，偏差①）");
    TestHarness::check(mgr.tryAcquireRunActive(QStringLiteral("rb")),
                       "不同 runId 必须互不阻塞");
    mgr.releaseRunActive(QStringLiteral("ra"));
    TestHarness::check(mgr.tryAcquireRunActive(QStringLiteral("ra")),
                       "释放后必须可再抢占");
}

} // namespace

int tst_workflowmanager()
{
    const int before = TestHarness::failCount();

    // ---- 纯函数组：零夹具，任何环境全跑 ----
    testIdentity();
    testValidateMeta();
    testAgentKey();
    testValidateJsonSchema();
    testFillSchema();
    testMockRun();
    testParseRunnerJson();
    testRegistryPolicyNoteTool();
    testResponseText();
    testBudgetLimitsTask();

    // ---- 文件产物组：夹具缺失只 SKIP 本组（半跳过形，见文件头登记）----
    ScopedTempRoot tmp("workflowmanager");
    if (!tmp.isValid()) {
        TestHarness::skip("SKIP: LITE_TEST_TMPROOT unset/unwritable\n");
        return TestHarness::failCount() - before;
    }
    WorkflowManager mgr;
    mgr.setStoreDir(tmp.path() + QStringLiteral("/workflows"));
    const QString store = mgr.storeDir();
    testFileReserve(mgr, store);
    testFileSnapshot(mgr, store);
    testFileLastRun(mgr, store);
    testFileJournal(store);
    testFileActiveRuns(mgr);

    return TestHarness::failCount() - before;
}
