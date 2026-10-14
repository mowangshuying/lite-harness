// WorkflowManager.cpp — s16 workflow 纯引擎实现（注册表/校验/稳定哈希/台账/文件产物/预留/抢占）。
// 机制与偏差登记见 WorkflowManager.h 头注释；本文件与主目标+测试目标同编译（LiteLog.h 中转头）。
#include "WorkflowManager.h"
#include "LiteLog.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>
#include <limits>

namespace {

// sha256 大数取模（lcc stable_hash :68-71 = int(hexdigest,16)，消费点 %10**10 / %4 / %5）。
// 逐字节折叠 acc=(acc*256+b)%mod：每步不变式「acc ≡ 前缀数值 (mod mod)」成立，与 Python 逐位相等。
// 溢出界：mod ≤ 1e10 → acc*256+b < 2.56e12 ≪ quint64 上限 1.8e19（偏差③，优于任务书「前 8 字节」近似方案）。
quint64 sha256Mod(const QString &text, quint64 mod)
{
    const QByteArray digest = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256);
    quint64 acc = 0;
    for (const char byte : digest)
        acc = (acc * 256u + quint64(quint8(byte))) % mod;
    return acc;
}

// 单 JSON 值解析（偏差⑨：顶层标量与 lcc json.loads 同口径——QJsonDocument 顶层只收
// object/array，包一层单元素数组再取首元素即可承载任意顶层值；空文本/坏文本判失败）。
bool parseJsonValue(const QString &text, QJsonValue *out)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray("[").append(text.toUtf8()).append(']'), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray() || doc.array().isEmpty())
        return false;
    if (out)
        *out = doc.array().at(0);
    return true;
}

// 紧凑 JSON 文本（QJsonDocument 无 QJsonValue 入口：同样经单元素数组包裹再剥壳）。
// 键序=Qt 保证的字典序（json.dumps sort_keys=True 同构，偏差⑥）；数组元素序=插入序（JSON 数组语义）。
QString compactJsonText(const QJsonValue &value)
{
    QJsonArray wrapper;
    wrapper.append(value);
    QByteArray text = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    while (text.endsWith('\n'))   // toJson 换行口径跨版本防御
        text.chop(1);
    const QString arrayText = QString::fromUtf8(text);
    return arrayText.mid(1, arrayText.size() - 2);   // 去掉外层 [ ]
}

// schema 的摘要文本（lcc :319 json.dumps(schema, sort_keys=True)；None→json.dumps 出 "null"）。
QString schemaCanonicalText(const QJsonValue &schema)
{
    if (schema.isNull() || schema.isUndefined())
        return QStringLiteral("null");
    return compactJsonText(schema);
}

// 自 s[start]（须为 '{'）起找第一个深度归零的 '}'；字符串态/转义态内不计。
// 与 lcc json.JSONDecoder().raw_decode :235 语义对照：raw_decode 在第一个合法值处停、
// 容忍其后噪声；本扫描对「合法对象后又出现晚配平的 }」等病态文本会在更晚位置截断——
// 仅影响极端手改文本，正常模型输出无差（刻意差异登记，见 parseRunnerJson 注释）。
int objectEnd(const QString &s, int start)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int i = start; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        if (inString) {
            if (escaped)
                escaped = false;
            else if (ch == QLatin1Char('\\'))
                escaped = true;
            else if (ch == QLatin1Char('"'))
                inString = false;
            continue;
        }
        if (ch == QLatin1Char('"'))
            inString = true;
        else if (ch == QLatin1Char('{'))
            ++depth;
        else if (ch == QLatin1Char('}')) {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return -1;
}

// 行尾空白剥离（lcc str.rstrip() :370/:375；Python 默认集中空白字符，QChar::isSpace 同口径）。
QString rstripText(QString text)
{
    while (!text.isEmpty() && text.at(text.size() - 1).isSpace())
        text.chop(1);
    return text;
}

// 事件值 → 日志文本（lcc f-string 语义近似：数字整数化、bool 小写 true/false——
// Python True/False 首字母大写的差异有意收敛为 JSON 口径，纯观感不入数据）。
QString jsonValueLogText(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Null:
        return QStringLiteral("null");
    case QJsonValue::Double: {
        const double d = value.toDouble();
        double whole = 0.0;
        if (std::modf(d, &whole) == 0.0 && d >= -9.2e18 && d <= 9.2e18)
            return QString::number(qint64(d));
        return QString::number(d);
    }
    case QJsonValue::Array:
    case QJsonValue::Object:
        return compactJsonText(value);
    case QJsonValue::Undefined:
        break;
    }
    return QStringLiteral("null");
}

// 从 "wf_{name}_{16hex}" 反取 name 段（固定前缀 "wf_" + 末段恰 16 位 hex，中段整体即 name——
// name 允许 '_'（WORKFLOW_NAME_RE 字符集含下划线），取最后一个 '_' 之前的整段，与 lcc
// 由 meta 直接携带 name 的正向拼接互逆；仅 reserveRunId 撞名重试的 lite 转译需要，见偏差⑱）。
QString runIdNameOf(const QString &runId)
{
    if (!runId.startsWith(QLatin1String("wf_")))
        return QString();
    const qsizetype last = runId.lastIndexOf(QLatin1Char('_'));
    if (last <= 2)
        return QString();
    return runId.mid(3, last - 3);
}

// SimpleJsonSchema.validate 递归（lcc :116-152）。返回空串=通过；非空=逐字错误文本。
// schema 非对象（Python 层不可能发生，defensive 折叠）→ 视为无约束放行。
QString schemaValidate(const QJsonValue &value, const QJsonValue &schemaV)
{
    const QJsonObject schema = schemaV.toObject();
    // enum 先于 type 判定（lcc :118-119）。QJsonValue== 为严格类型相等，
    // Python True==1 的跨型巧合不照搬（JSON 域无实际影响，登记）。
    if (schema.contains(QLatin1String("enum"))) {
        const QJsonValue enumV = schema.value(QLatin1String("enum"));
        if (enumV.isArray()) {
            const QJsonArray options = enumV.toArray();
            bool found = false;
            for (const QJsonValue &option : options) {
                if (option == value) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Python repr 形态（"['high', 'medium']"）转译为 JSON 紧凑数组文本（偏差⑦）。
                return QStringLiteral("expected one of ") + compactJsonText(options);
            }
        }
    }
    const QString type = schema.value(QLatin1String("type")).toString();
    if (type == QLatin1String("object")) {
        if (!value.isObject())
            return QStringLiteral("expected object");
        const QJsonObject object = value.toObject();
        const QJsonValue requiredV = schema.value(QLatin1String("required"));
        if (requiredV.isArray()) {
            for (const QJsonValue &keyV : requiredV.toArray()) {
                const QString key = keyV.toString();
                if (!object.contains(key))
                    return QStringLiteral("missing required key '") + key + QLatin1Char('\'');
            }
        }
        const QJsonObject properties = schema.value(QLatin1String("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            if (!object.contains(it.key()))
                continue;   // 未列出的可选键不校验（lcc :128）
            const QString err = schemaValidate(object.value(it.key()), it.value());
            if (!err.isEmpty())
                return it.key() + QStringLiteral(": ") + err;
        }
        return QString();
    }
    if (type == QLatin1String("array")) {
        if (!value.isArray())
            return QStringLiteral("expected array");
        const QJsonValue itemsV = schema.value(QLatin1String("items"));
        // Python `if items:` 的 falsy 语义：缺省/null/空对象/空数组 → 不查元素（逐条对齐，登记）。
        if (itemsV.isUndefined() || itemsV.isNull()
            || (itemsV.isObject() && itemsV.toObject().isEmpty())
            || (itemsV.isArray() && itemsV.toArray().isEmpty()))
            return QString();
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            const QString err = schemaValidate(array.at(i), itemsV);
            if (!err.isEmpty())
                return QStringLiteral("[") + QString::number(i) + QStringLiteral("]: ") + err;
        }
        return QString();
    }
    if (type == QLatin1String("string"))
        return value.isString() ? QString() : QStringLiteral("expected string");
    if (type == QLatin1String("boolean"))
        return value.isBool() ? QString() : QStringLiteral("expected boolean");
    if (type == QLatin1String("number") || type == QLatin1String("integer"))
        // QJsonValue 的 bool/string 与 double 分型，天然复刻 Python「排除 bool 的数值判定」:150。
        return value.isDouble() ? QString() : QStringLiteral("expected number");
    // 未知/缺省 type 一律放行（lcc :152）。
    return QString();
}

} // namespace

// ---- WorkflowManager：注册表与策略 ----

WorkflowManager::WorkflowManager()
{
    // 内置注册表仅 review-changes（lcc :527-531/:622-624 逐字；执行体属 Lane B 的执行环，
    // 本类只提供 meta 与机械件）。
    WfMeta reviewChanges;
    reviewChanges.name = QStringLiteral("review-changes");
    reviewChanges.description =
        QStringLiteral("Review changed files across dimensions, verify each finding");
    reviewChanges.phases = { QStringLiteral("Review"), QStringLiteral("Verify") };
    m_registry.insert(reviewChanges.name, reviewChanges);
}

void WorkflowManager::setStoreDir(const QString &dir)
{
    m_storeDir = dir;
}

QString WorkflowManager::storeDir() const
{
    return m_storeDir;
}

QStringList WorkflowManager::workflowNames() const
{
    return m_registry.keys();
}

bool WorkflowManager::hasWorkflow(const QString &name) const
{
    return m_registry.contains(name);
}

WfMeta WorkflowManager::metaFor(const QString &name) const
{
    return m_registry.value(name);   // 查无=默认空 WfMeta（lcc dict.get 语义）
}

QString WorkflowManager::systemPromptNote() const
{
    if (m_registry.isEmpty())
        return QString();   // Python None 转译空串（偏差⑬）
    QStringList lines;
    for (auto it = m_registry.constBegin(); it != m_registry.constEnd(); ++it)
        lines << (QStringLiteral("- ") + it.value().name + QStringLiteral(": ") + it.value().description);
    // 逐字：标题行自带尾 \n，条目以 \n 连、无尾随（lcc :643-646）。
    return QStringLiteral("Registered workflows (call the workflow tool with one of these names):\n")
           + lines.join(QLatin1Char('\n'));
}

QJsonObject WorkflowManager::toolInfo() const
{
    // OpenAI function 信封转译 lcc WORKFLOW_TOOL（Anthropic input_schema 形 :595-612），
    // 仿 McpManager 工具装配先例（偏差㉒）。description 逐字（禁翻区，发往模型）。
    QJsonObject properties;
    properties.insert(QStringLiteral("name"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    properties.insert(QStringLiteral("args"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}});
    properties.insert(QStringLiteral("resume_from_run_id"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    QJsonObject parameters;
    parameters.insert(QStringLiteral("type"), QStringLiteral("object"));
    parameters.insert(QStringLiteral("properties"), properties);
    parameters.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("name")});
    parameters.insert(QStringLiteral("additionalProperties"), false);
    QJsonObject function;
    // 工具名 "workflow"：不在 ToolNames.h 名单（禁区），装配与分发接线属 Lane C，登记。
    function.insert(QStringLiteral("name"), QStringLiteral("workflow"));
    function.insert(QStringLiteral("description"), QStringLiteral(
        "Run a saved workflow by name. The orchestration script lives in the host registry; "
        "you only pick the workflow and pass its input in args. "
        "Pass resume_from_run_id to reuse a previous run's journal."));
    function.insert(QStringLiteral("parameters"), parameters);
    QJsonObject envelope;
    envelope.insert(QStringLiteral("type"), QStringLiteral("function"));
    envelope.insert(QStringLiteral("function"), function);
    return envelope;
}

QString WorkflowManager::policyFor(const QString &name) const
{
    if (name.isEmpty())
        return QStringLiteral("confirm");   // lcc :651 非 str/空串→confirm（C++ 强类型只剩空串可达）
    // 硬 deny 表：当前为空，保留形态与策略表的手工同步债（lcc WORKFLOW_DENY_LIST :591，§10-6）。
    static const QStringList denyList;
    if (denyList.contains(name))
        return QStringLiteral("deny");
    // 宿主策略表（lcc WORKFLOW_HOST_POLICY :586-588）：未登记一律 confirm（fail-closed :655）。
    static const QHash<QString, QString> policy = {
        { QStringLiteral("review-changes"), QStringLiteral("allow") },
    };
    return policy.value(name, QStringLiteral("confirm"));
}

// ---- 静态纯函数（公开供单测直调，仿 McpManager::normalizeName 纪律）----

QString WorkflowManager::stableHash(const QString &text)
{
    // sha256 十六进制全串小写（lcc :68-71 的 int(...) 是同一 digest 的数值形态；
    // 文本形态供日志/测试，数值消费经 sha256Mod 逐位等价达成）。
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString WorkflowManager::createRunId(const QString &name)
{
    // "wf_{name}_" + 16 位小写 hex（lcc create_run_id :74-75，secrets.token_hex(8) 同量级随机）。
    const quint64 raw = QRandomGenerator::system()->generate64();
    QByteArray bytes(8, '\0');
    for (int i = 0; i < 8; ++i)
        bytes[i] = char((raw >> ((7 - i) * 8)) & 0xFFu);   // 大端 8 字节 → toHex 小写
    return QStringLiteral("wf_") + name + QLatin1Char('_') + QString::fromLatin1(bytes.toHex());
}

QString WorkflowManager::createTaskId(const QString &runId)
{
    return QStringLiteral("local_workflow_") + runId;   // lcc :78-79
}

bool WorkflowManager::validateRunId(const QString &runId, QString *error)
{
    static const QRegularExpression re(
        QStringLiteral("^wf_[A-Za-z0-9][A-Za-z0-9._-]{0,63}_[0-9a-f]{16}$"));   // lcc :61 逐字
    const QRegularExpressionMatch match = re.match(runId);
    // PCRE '$' 允许尾随 \n；fullmatch 语义补「消费到串尾」判据对齐 Python。
    const bool ok = match.hasMatch() && match.capturedEnd(0) == runId.size();
    if (!ok && error)
        *error = QStringLiteral("invalid workflow runId");   // lcc :84 逐字
    return ok;
}

bool WorkflowManager::validateMeta(const WfMeta &meta, QString *error)
{
    // lcc :91-92「非 dict」与 :99-100「description 非 str」在 C++ 强类型下不可达：两条逐字串
    // 不声明，五门实际可达三门；调用点若从 JSON 组 meta，类型门应在更外层折叠（登记于此）。
    if (meta.name.isEmpty() || meta.description.isEmpty()) {
        if (error)
            *error = QStringLiteral("meta requires `name` and `description`");   // lcc :94 逐字
        return false;
    }
    static const QRegularExpression nameRe(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));   // lcc WORKFLOW_NAME_RE :60 逐字
    const QRegularExpressionMatch match = nameRe.match(meta.name);
    if (!match.hasMatch() || match.capturedEnd(0) != meta.name.size()) {
        if (error)
            *error = QStringLiteral(
                "meta.name must be a 1-64 character slug using letters, numbers, '.', '_', or '-'");  // lcc :97 逐字
        return false;
    }
    // phases 空列表=Python 缺省放行（:101 的 "phases" in meta 为假）；任一条目为空即拒（:102-105）。
    for (const QString &phase : meta.phases) {
        if (phase.isEmpty()) {
            if (error)
                *error = QStringLiteral("meta.phases must be a list of non-empty strings");   // :105 逐字
            return false;
        }
    }
    return true;
}

QString WorkflowManager::agentKey(const QString &kind, const QString &label,
                                  const QString &prompt, const QJsonValue &schema)
{
    // basis 与模数逐字对照 lcc WorkflowJournal.key :319-320：
    // "{kind}|{label}|{prompt}|{schema 紧凑文本}" → sha256 数值 %10**10 补零 10 位。
    const QString basis = kind + QLatin1Char('|') + label + QLatin1Char('|')
                          + prompt + QLatin1Char('|') + schemaCanonicalText(schema);
    const quint64 mod = sha256Mod(basis, 10000000000ULL);   // 1e10 恰 < 2^53，quint64 折叠无溢出
    return kind + QLatin1Char('-')
           + QString::number(mod).rightJustified(10, QLatin1Char('0'));
}

bool WorkflowManager::validateJsonSchema(const QJsonValue &value, const QJsonObject &schema,
                                         QString *error)
{
    const QString err = schemaValidate(value, QJsonValue(schema));
    if (!err.isEmpty() && error)
        *error = err;
    return err.isEmpty();
}

QJsonValue WorkflowManager::fillSchema(const QJsonObject &schema, const QString &seed)
{
    // lcc fill_schema :155-167：确定性 mock 填充器（同 seed 同值，供离线验证）。
    const QString type = schema.value(QLatin1String("type")).toString();
    if (type == QLatin1String("object")) {
        QStringList keys;
        const QJsonValue requiredV = schema.value(QLatin1String("required"));
        if (requiredV.isArray() && !requiredV.toArray().isEmpty()) {
            for (const QJsonValue &keyV : requiredV.toArray())
                keys << keyV.toString();
        } else {
            keys = schema.value(QLatin1String("properties")).toObject().keys();
        }
        const QJsonObject properties = schema.value(QLatin1String("properties")).toObject();
        QJsonObject object;
        for (const QString &key : keys) {
            // Python schema["properties"][k] 缺键 KeyError（:160）；lite 防御折叠：缺键 →
            // 空子 schema → 落叶子取 seed 尾段=key 名（偏差⑾，注释登记）。
            object.insert(key, fillSchema(properties.value(key).toObject(),
                                          seed + QLatin1Char('/') + key));
        }
        return object;
    }
    if (type == QLatin1String("array")) {
        QJsonArray array;
        array.append(fillSchema(schema.value(QLatin1String("items")).toObject(),
                                seed + QStringLiteral("/0")));
        return array;
    }
    if (type == QLatin1String("boolean"))
        return sha256Mod(seed, 4) != 0;
    if (type == QLatin1String("number") || type == QLatin1String("integer"))
        return double(sha256Mod(seed, 5));   // JSON 数字无整/浮之别，值域 0..4 语义一致
    return seed.section(QLatin1Char('/'), -1);   // 叶子：seed 末段（rsplit("/",1)[-1]）
}

QJsonValue WorkflowManager::mockRun(const QString &prompt, const QJsonObject &schema,
                                    const QString &label, qint64 *tokensOut)
{
    // lcc MockAgentRunner.run :177-205。Python schema=None ⇔ lite 空对象 {}（偏差⑳）。
    QJsonValue value;
    if (schema.isEmpty()) {
        const QString head = label.isEmpty() ? prompt : label;
        value = QStringLiteral("[mock] ") + head.left(60);   // [:60] 切片（lcc :184）
    } else {
        const QJsonObject properties = schema.value(QLatin1String("properties")).toObject();
        if (properties.contains(QLatin1String("findings"))) {
            const int count = 1 + int(sha256Mod(prompt, 2));
            const QString base = label.isEmpty() ? QStringLiteral("audit") : label;
            static const char *const severities[] = {"high", "medium", "low"};
            QJsonArray findings;
            for (int i = 0; i < count; ++i) {
                QJsonObject finding;
                finding.insert(QStringLiteral("title"),
                               base + QStringLiteral(" #") + QString::number(i + 1));
                finding.insert(QStringLiteral("severity"), QString::fromLatin1(
                    severities[sha256Mod(prompt + QString::number(i), 3)]));
                findings.append(finding);
            }
            QJsonObject object;
            object.insert(QStringLiteral("findings"), findings);
            value = object;
        } else if (properties.contains(QLatin1String("isReal"))) {
            const bool real = sha256Mod(prompt, 4) != 0;
            QJsonObject object;
            object.insert(QStringLiteral("isReal"), real);
            object.insert(QStringLiteral("reason"),
                          real ? QStringLiteral("reproduced")
                               : QStringLiteral("could not reproduce"));   // lcc :197-198 逐字
            value = object;
        } else {
            value = fillSchema(schema, prompt);
        }
    }
    if (tokensOut) {
        // _tokens :203-205：len(prompt)//4 + len(json.dumps(result))//4。整除、UTF-16 单元口径
        // 与 Python 码点/转义文本长度有差（偏差⑯，仅离线 mock 用，不进模型对话）。
        *tokensOut = prompt.length() / 4 + compactJsonText(value).length() / 4;
    }
    return value;
}

QJsonValue WorkflowManager::parseRunnerJson(const QString &text, bool *ok, QString *error)
{
    // lcc parse_runner_json :217-239。先剥 ``` 围栏（首行整删 + 尾行恰 ``` 才删，:221-226）。
    QString stripped = text.trimmed();
    if (stripped.startsWith(QLatin1String("```"))) {
        QStringList lines = stripped.split(QLatin1Char('\n'));
        if (!lines.isEmpty())
            lines.removeFirst();
        if (!lines.isEmpty() && lines.back().trimmed() == QLatin1String("```"))
            lines.removeLast();
        stripped = lines.join(QLatin1Char('\n')).trimmed();
    }
    // 整串 parse：任意顶层值皆可（object/array/标量，json.loads 同口径，偏差⑨）。
    QJsonValue value;
    if (parseJsonValue(stripped, &value)) {
        if (ok)
            *ok = true;
        return value;
    }
    // 失败再逐 '{' 位置取配平候选，首个合法对象胜出（lcc raw_decode :231-238 的 lite 等价，
    // objectEnd 注释登记病态文本差异；候选须为 object——Python 只扫 '{' 起点，同口径）。
    for (int position = 0; position < stripped.size(); ++position) {
        if (stripped.at(position) != QLatin1Char('{'))
            continue;
        const int end = objectEnd(stripped, position);
        if (end < 0)
            continue;
        QJsonValue candidate;
        if (parseJsonValue(stripped.mid(position, end - position + 1), &candidate)
            && candidate.isObject()) {
            if (ok)
                *ok = true;
            return candidate;
        }
    }
    if (ok)
        *ok = false;
    if (error)
        *error = QStringLiteral("workflow agent returned invalid JSON");   // :239 逐字（偏差⑰）
    // 失败占位刻意用 Undefined 而非默认构造（Qt6 的 QJsonValue() 是 Null 型，非「无值」）。
    return QJsonValue(QJsonValue::Undefined);
}

QString WorkflowManager::responseText(const QString &raw)
{
    // lcc response_text :208-214 的 text 块 join 在 lite 无对应层（AsyncRequest 已回纯文本），
    // 仅保留 .strip() 收口（偏差㉑，登记）。
    return raw.trimmed();
}

// ---- 文件产物与预留 ----

bool WorkflowManager::reserveRunId(QString *runIdInOut, QString *error)
{
    if (!runIdInOut) {
        if (error)
            *error = QStringLiteral("invalid workflow runId");
        return false;
    }
    if (m_storeDir.isEmpty()) {
        // lite-only 守卫串（lcc 靠 env 恒有 store，无对应文案，非 lcc 串登记）。
        if (error)
            *error = QStringLiteral("workflow store dir is not configured");
        return false;
    }
    QString candidate = *runIdInOut;
    if (!validateRunId(candidate, error))
        return false;   // 候选本身非法：lcc validate 即 raise 的形态，不进重试环
    QDir().mkpath(m_storeDir);   // lcc _ensure_store :659-661
    const QString name = runIdNameOf(candidate);
    for (int attempt = 0; attempt < kWorkflowRunIdReserveAttempts; ++attempt) {
        if (attempt > 0)
            candidate = createRunId(name);   // 撞名换全新高随机 id（lcc 每轮 create_run_id :675）
        QFile file(m_storeDir + QLatin1Char('/') + candidate + QStringLiteral(".json"));
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {   // O_CREAT|O_EXCL 独占（:678）
            file.close();
            *runIdInOut = candidate;
            return true;
        }
        // 撞名之外的打开失败（只读目录等）一并折入重试→耗尽文案；
        // lcc 仅捕 FileExistsError、其它 OSError 直抛——lite 无异常通道，折叠为判定串（登记）。
    }
    if (error)
        *error = QStringLiteral("could not allocate a unique workflow runId");   // :683 逐字
    return false;
}

bool WorkflowManager::reserveRunId(const QString &name, QString *runIdOut, QString *error)
{
    // lcc reserve_run_id(meta) :670-683 同形：首轮候选即 create_run_id(meta)（偏差⑱，包装 in/out 形）。
    QString candidate = createRunId(name);
    if (!reserveRunId(&candidate, error))
        return false;
    if (runIdOut)
        *runIdOut = candidate;
    return true;
}

bool WorkflowManager::writeJsonAtomic(const QString &path, const QJsonValue &value)
{
    // lcc _writeJson :742-748：.tmp+os.replace → QSaveFile 同原子语义（偏差⑩）；indent=2 对
    // Qt Indented（宽度观感差异）。顶层标量落紧凑文本（Python json.dumps 任意值同口径，偏差⑨）。
    QDir().mkpath(QFileInfo(path).absolutePath());
    QByteArray text;
    if (value.isObject())
        text = QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented);
    else if (value.isArray())
        text = QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented);
    else
        text = (compactJsonText(value) + QLatin1Char('\n')).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(text) != text.size())
        return false;
    return file.commit();
}

QJsonObject WorkflowManager::readSnapshot(const QString &storeDir, const QString &runId,
                                          QString *error)
{
    // lcc _readSnapshot :750-760（path 函数内拼，同 lcc 形，偏差⑱）。
    const QString path = storeDir + QLatin1Char('/') + runId + QStringLiteral(".json");
    QFile file(path);
    if (!file.exists()) {
        if (error)
            *error = QStringLiteral("resume snapshot not found for ") + runId;   // :753 逐字
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        // 存在却读不开：lcc read_text 直抛 OSError；lite 折进 invalid 串（登记）。
        if (error)
            *error = QStringLiteral("invalid resume snapshot for ") + runId;
        return QJsonObject();
    }
    QJsonValue snapshot;
    if (!parseJsonValue(QString::fromUtf8(file.readAll()), &snapshot) || !snapshot.isObject()) {
        if (error)
            *error = QStringLiteral("invalid resume snapshot for ") + runId;   // :757/:759 逐字
        return QJsonObject();
    }
    return snapshot.toObject();
}

void WorkflowManager::saveLastRun(const QString &runId)
{
    // lcc _saveLastRun :762-764：内存值优先于盘（readLastRun 侧），盘写裸 runId 无换行。
    m_lastRunId = runId;
    if (m_storeDir.isEmpty())
        return;   // lite：storeDir 未注入时静默只保内存值（lcc env 恒有目录，无此态）
    QDir().mkpath(m_storeDir);
    QFile file(m_storeDir + QLatin1Char('/') + QStringLiteral("last_run.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;   // 写失败静默（内存已是权威；登记：lcc 直抛 OSError）
    file.write(runId.toUtf8());
}

QString WorkflowManager::readLastRun() const
{
    // lcc readLastRun :766-771：进程内优先，其次磁盘（resume 入口）。
    if (!m_lastRunId.isEmpty())
        return m_lastRunId;
    if (m_storeDir.isEmpty())
        return QString();
    QFile file(m_storeDir + QLatin1Char('/') + QStringLiteral("last_run.txt"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();   // 缺失=None→空串（偏差⑬）；存在却读不开同样折空（登记）
    return QString::fromUtf8(file.readAll()).trimmed();
}

// ---- 进程内抢占（替代 lcc 两层锁；lcc :706-738，偏差①）----

bool WorkflowManager::tryAcquireRunActive(const QString &runId)
{
    // Qt6 QSet::insert 返回 iterator（非 bool，与原注释口径不符=本轮实测修正）；
    // contains+insert 两步在零线程单 GUI 下与原子抢占等价。
    if (m_activeRuns.contains(runId))
        return false;
    m_activeRuns.insert(runId);
    return true;
}

void WorkflowManager::releaseRunActive(const QString &runId)
{
    m_activeRuns.remove(runId);
}

QJsonObject WorkflowManager::serializeTask(const WfTask &task) const
{
    // lcc serialize_task :497-507（meta['name'] 扁平为 task.workflowName 成员，偏差⑿）。
    QJsonObject usage;
    usage.insert(QStringLiteral("agents"), task.agents);
    usage.insert(QStringLiteral("tokens"), double(task.tokens));
    QJsonObject object;
    object.insert(QStringLiteral("taskId"), task.taskId);
    object.insert(QStringLiteral("taskType"), QStringLiteral("local_workflow"));
    object.insert(QStringLiteral("runId"), task.runId);
    object.insert(QStringLiteral("workflowName"), task.workflowName);
    object.insert(QStringLiteral("status"), task.status);
    object.insert(QStringLiteral("usage"), usage);
    object.insert(QStringLiteral("progress"), task.progress);
    return object;
}

// ---- WfTask（台账；lcc LocalWorkflowTask :356-375）----

void WfTask::event(const QString &name, const QStringList &kvLines)
{
    // lcc :368-370：log_info("wf", f"{name} {k=v 空格连}".rstrip())。kv 串由调用方组好传入。
    LOG_INFO << "[wf] " << rstripText(name + QLatin1Char(' ') + kvLines.join(QLatin1Char(' ')));
}

void WfTask::progressEvent(const QString &ptype, const QJsonObject &data)
{
    // lcc :372-375：progress 追加 {"type":ptype, **data}；日志 "progress {ptype} {k=v}".rstrip()。
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), ptype);
    QStringList kvLines;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        entry.insert(it.key(), it.value());   // 同名键覆盖 "type"（Python **data 同构）
        kvLines << (it.key() + QLatin1Char('=') + jsonValueLogText(it.value()));
    }
    progress.append(entry);
    LOG_INFO << "[wf] " << rstripText(QStringLiteral("progress ") + ptype + QLatin1Char(' ')
                                      + kvLines.join(QLatin1Char(' ')));
}

// ---- WfBudget（lcc Budget :334-353）----

WfBudget::WfBudget(qint64 total)
    : m_total(total)   // -1=不限（Python total=None 转译）
{
}

bool WfBudget::add(qint64 n, QString *error)
{
    // 超支即拒、绝不入账（lcc :342-347 raise-before-+=）。
    if (m_total >= 0 && m_spent + n > m_total) {
        if (error)
            *error = QStringLiteral("token budget exceeded (")
                     + QString::number(m_spent + n) + QStringLiteral(" > ")
                     + QString::number(m_total) + QLatin1Char(')');   // :345 逐字格式
        return false;
    }
    m_spent += n;
    return true;
}

qint64 WfBudget::remaining() const
{
    // float('inf') 无整型对应→int64 上限（偏差登记：Lane B 判预算请用 total()<0，勿比较上限）。
    if (m_total < 0)
        return std::numeric_limits<qint64>::max();
    return qMax(qint64(0), m_total - m_spent);   // lcc max(0, total-spent) :353
}

qint64 WfBudget::spent() const
{
    return m_spent;
}

qint64 WfBudget::total() const
{
    return m_total;
}

// ---- WfExecutionLimits（lcc ExecutionLimits :378-388 的 cap 计数部分；semaphore 属 Lane B）----

bool WfExecutionLimits::claimAgent(QString *error)
{
    // lcc :385-388 先增后判：超限那次 used 也停在 cap+1（口径逐字保持）。
    ++m_agents;
    if (m_agents > kWorkflowAgentCap) {
        if (error)
            *error = QStringLiteral("agent() cap reached (")
                     + QString::number(kWorkflowAgentCap) + QLatin1Char(')');   // :388 逐字格式
        return false;
    }
    return true;
}

int WfExecutionLimits::agentsUsed() const
{
    return m_agents;
}

// ---- WfJournal（追加式 jsonl 缓存；lcc WorkflowJournal :286-331）----

WfJournal::WfJournal() = default;

WfJournal::~WfJournal()
{
    close();   // RAII：对应 lcc 执行环 finally 中的 journal.close()（转译登记见头注释）
}

bool WfJournal::open(const QString &storeDir, const QString &runId, bool resume, QString *error)
{
    close();
    m_cache.clear();
    if (storeDir.isEmpty()) {
        if (error)
            *error = QStringLiteral("workflow store dir is not configured");   // lite-only 守卫
        return false;
    }
    QDir().mkpath(storeDir);   // lcc :291 mkdir(parents=True, exist_ok=True)
    const QString path = storeDir + QLatin1Char('/') + runId + QStringLiteral(".journal.jsonl");
    if (resume) {
        QFile probe(path);
        if (!probe.exists()) {
            if (error)
                *error = QStringLiteral("resume journal not found for ") + runId;   // :297 逐字
            return false;
        }
        if (!probe.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot open workflow journal");   // lite 折叠串（lcc OSError 直抛，登记）
            return false;
        }
        QStringList lines = QString::fromUtf8(probe.readAll()).split(QLatin1Char('\n'));
        probe.close();
        if (!lines.isEmpty() && lines.back().isEmpty())
            lines.removeLast();   // splitlines 尾换行不产生空行条目（对齐 :298-299）
        for (int index = 0; index < lines.size(); ++index) {
            // 坏行 fail-closed：绝不当作「没跑过」静默重放（lcc :301-310）。
            QJsonValue record;
            if (!parseJsonValue(lines.at(index), &record) || !record.isObject()) {
                if (error)
                    *error = QStringLiteral("invalid resume journal record at line ")
                             + QString::number(index + 1);   // :309 逐字（1-based）
                return false;
            }
            const QJsonObject recordObject = record.toObject();
            const QJsonValue key = recordObject.value(QLatin1String("key"));
            if (!key.isString() || !recordObject.contains(QLatin1String("value"))) {
                if (error)
                    *error = QStringLiteral("invalid resume journal record at line ")
                             + QString::number(index + 1);   // :303-310 结构门同串
                return false;
            }
            m_cache.insert(key.toString(), recordObject.value(QLatin1String("value")));
        }
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {   // resume 追加不截断（:312）
            if (error)
                *error = QStringLiteral("cannot open workflow journal");
            return false;
        }
        return true;
    }
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {     // 新运行直接截断（:314）
        if (error)
            *error = QStringLiteral("cannot open workflow journal");
        return false;
    }
    return true;
}

bool WfJournal::cached(const QString &key, QJsonValue *out) const
{
    // lcc MISS 哨兵（:59，区分合法 None）在 lite 由 bool 返回值表达：命中 true+出参。
    const auto it = m_cache.constFind(key);
    if (it == m_cache.constEnd())
        return false;
    if (out)
        *out = it.value();
    return true;
}

void WfJournal::record(const QString &key, const QJsonValue &value)
{
    if (!m_file.isOpen())
        return;   // 未开即写：静默丢弃（宿主流程恒 open→record→close；登记防御）
    QJsonObject recordObject;
    recordObject.insert(QStringLiteral("key"), key);
    recordObject.insert(QStringLiteral("value"), value);
    QByteArray line = compactJsonText(QJsonValue(recordObject)).toUtf8();
    line.append('\n');   // 一行一 JSON（jsonl，MessageBus 先例形）
    m_file.write(line);
    m_file.flush();      // lcc :327：逐条 flush，崩溃中途外部亦可见
    m_cache.insert(key, value);
}

void WfJournal::close()
{
    if (m_file.isOpen())
        m_file.close();
}
