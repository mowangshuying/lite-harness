#pragma once

// ============================================================================
// 多 Provider 模型服务的数据层单源（header-only，纯 QtCore：零 Widgets / 零 FluentUI）
//
// 【本头为什么存在】
// 改造前模型服务是「一个全局端点 + 一份扁平模型名清单」：config.json 的
// apiBaseUrl / apiToken / modelOptions 三个单数键，全进程一份凭据。加第二家服务商
// 只能整体替换端点与密钥，原模型立刻不可用；且加一个模型要在一个输入框里重打整串
// 逗号分隔文本。本头把 provider 提为一等公民（各自 url / key / 模型清单），并保证
// **旧三键继续读得回**——老配置文件不改一字也能跑，不做强制迁移。
//
// 【路由形态：按模型名反查，而不是「切换当前 provider」】
// 这是本头存在的最重要理由，也是它必须与 QOpenAi 的并发形态一起看的原因。
// QOpenAi 是进程级单例（QOpenAi.cpp 的 OpenAiClient &client()），而主循环 / 子代理 /
// 队友 / workflow / 压缩 / 记忆六条链会**并发**发请求。若 provider 做成「当前选中」的
// 全局状态，A 会话切到服务商甲的瞬间，正在跑的 B 会话下一发请求就拿着甲的 key 去打
// 乙的端点——静默 401，界面上复现不出来。
// 而每条请求的 body 里本来就带着 model（AgentLoopRequest.cpp / SubAgent.cpp），
// 所以凭据可以**由模型名反查得到**：route() 给出归属，调用方取那一份 url/token。
// 如此六条链一行不改，history.json 与会话索引里的 model 仍是纯模型名（盘上格式
// 完全向后兼容），代价是模型名必须跨 provider 全局唯一——由 duplicateModels()
// 在配置期拦下，绝不静默让第一个命中者胜出。
//
// 【为什么纯函数一律收数据、不收配置】
// 判据与 IO 分开才能进 ctest：tests 目标只链 Qt6::Core，且 tst_agentconst 明确
// 不调用任何 *_value() 取值函数（那些经 AppSettings::reader() 要求 QCoreApplication
// 实例、路径还会钉死在测试 exe 目录）。故本头的解析/摊平/路由/校验全部收参数，
// 读盘只留 loadProviders() 一个薄壳，测试不碰它。
//
// 【落盘形态】config.json 键 app/providers，值是 QVariantList of QVariantMap：
//   [{"id":"p-1a2b3c4d","name":"阿里云 MAAS","baseUrl":"https://…/v1",
//     "token":"sk-…","models":["qwen3.8-flash","qwen3.8-max"]}, …]
// 键缺失 / 空数组 / 形态不认识 → 一律回落旧三键合成的单 provider（legacyProvider），
// 绝不因为「新键没配」就让下拉变空。
// ============================================================================

// 读盘薄壳（本节末尾）经 AppSettings::value/setValue 走宿主键单源，与 AgentConstants.h
// 同形态：头本身 inline-only，不 include 进 tests 目标也不会引入新链接依赖；
// tst_providerspec 只调纯函数、不调 loadProviders，故不需要 QCoreApplication 实例。
#include "AppSettings.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaType> // providerFromMap 判 models 的裸逗号串形态
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace ProviderSpec {

// ---- 键名与上限（单点登记，消费点一律引这些常量，勿在别处再写一遍字面量）----
inline const QString kProvidersKey = QStringLiteral("providers");
inline const QString kLegacyBaseUrlKey = QStringLiteral("apiBaseUrl");
inline const QString kLegacyTokenKey = QStringLiteral("apiToken");
inline const QString kLegacyModelOptionsKey = QStringLiteral("modelOptions");

constexpr int kProvidersMax = 16;          // provider 条数上限（防手工篡改塞进超长数组）
constexpr int kModelsPerProviderMax = 64;  // 单个 provider 的模型条数上限
constexpr int kModelsTotalMax = 128;       // 摊平后进下拉的总条数上限（下拉宽度按最长名自适应，条数也要有界）
constexpr int kModelNameMax = 128;         // 单条模型名长度上限
constexpr int kProviderNameMax = 40;       // provider 显示名长度上限
constexpr int kModelsResponseMax = 512;    // /models 响应解析条数上限（服务端可能报几百个）

// ---- 数据本体 ----
struct Provider
{
    QString id;        // 稳定标识：只用于 provider 之间的区分，不进请求体、不进下拉文本
    QString name;      // 显示名（设置页与「provider/model」复合展示用）
    QString baseUrl;   // OpenAI 兼容基址，不含 /chat/completions；已去尾部斜杠
    QString token;     // Bearer 凭据（明文存本机 config.json 属既有用户裁决，见 AppSettings.h）
    QStringList models;// 该 provider 下可选的模型名，顺序即下拉顺序
};

// ---- 归一化小工具（纯函数）----

// 基址归一：去掉全部尾部斜杠。与 QOpenAi 端点拼接同源——两处各写一份必然漂移，
// 故这里交出唯一实现，QOpenAi 的 endpointFor 与本头的 modelsEndpointFor 都走它。
inline QString normalizeBaseUrl(const QString &input)
{
    QString base = input.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base;
}

// 基址合法性：须为绝对 URL 且 scheme 是 http/https。QUrl 对裸主机名给出空 scheme，
// 天然落回拒绝。写进配置只会让请求必失败，故在录入期就拦下。
inline bool isValidBaseUrl(const QString &input)
{
    const QUrl url(input.trimmed());
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

// 由基址拼 /models 端点（B 的拉取用）。与 chat/completions 的拼法同构：
// 都是「去尾斜杠的基址 + 固定后缀」，故基址该以 /v1 这类版本路径结尾、不含端点名。
inline QString modelsEndpointFor(const QString &baseUrl)
{
    return normalizeBaseUrl(baseUrl) + QLatin1String("/models");
}

// 模型名清洗：trim + 丢空 + 截断长度。保序去重交给上层（摊平时统一做）。
inline QString normalizeModelName(const QString &input)
{
    QString name = input.trimmed();
    if (name.size() > kModelNameMax)
        name = name.left(kModelNameMax);
    return name;
}

// provider 显示名清洗：空则回落到「服务商 N」形态，由调用方给序号；本函数只做
// trim 与截断，不猜名字（猜出来的名字会写进配置，用户下次看到会不认识）。
inline QString normalizeProviderName(const QString &input)
{
    QString name = input.trimmed();
    if (name.size() > kProviderNameMax)
        name = name.left(kProviderNameMax);
    return name;
}

// 由种子产出稳定短 id（形如 p-1a2b3c4d）。刻意用 FNV-1a 而非 QRandomGenerator：
// 随机 id 不可测（测试要断言往返一致），而 id 的语义只是「同一份输入得到同一个值」。
// 碰撞概率由调用方靠 uniqueProviderId() 兜，本函数不保证唯一。
inline QString makeProviderId(const QString &seed)
{
    quint32 h = 0x811C9DC5u;
    const QByteArray raw = seed.toUtf8();
    for (const QByteArray::value_type b : raw)
    {
        h ^= static_cast<quint8>(b);
        h *= 0x01000193u;
    }
    return QStringLiteral("p-%1").arg(h, 8, 16, QLatin1Char('0'));
}

// 在已有 id 集合上生成不碰撞的 id：撞了就对 seed 追加计数再散列。
inline QString uniqueProviderId(const QString &seed, const QStringList &taken)
{
    QString candidate = makeProviderId(seed);
    for (int n = 2; taken.contains(candidate); ++n)
        candidate = makeProviderId(seed + QLatin1Char('#') + QString::number(n));
    return candidate;
}

// ---- 解析：盘上形态 → Provider 列表（纯函数，容错优先）----

// 单条 provider 的 map → 结构体。字段缺失/类型不对一律取空值而非判失败——
// 坏一条不该拖垮整表（与 SessionStore「坏形态一律降级为空索引」的既有口径一致）。
inline Provider providerFromMap(const QVariantMap &map)
{
    Provider p;
    p.id = map.value(QStringLiteral("id")).toString().trimmed();
    p.name = normalizeProviderName(map.value(QStringLiteral("name")).toString());
    p.baseUrl = normalizeBaseUrl(map.value(QStringLiteral("baseUrl")).toString());
    p.token = map.value(QStringLiteral("token")).toString();

    // models 的两种形态都要认：json 数组（QVariantList）与用户手改成裸逗号串（QString）。
    // 这不是假想的宽容——AgentConst::normalizedText 的头注释记着同一条教训：合流成 json 后
    // 数组落 QVariantList 而非 QStringList，只判一种形态就会被误判「未配置」而静默回退。
    // 判据必须**先按类型分流**再取值，不能写成「先 toStringList()、空了再判 QString」：
    // Qt 6.9 实测 QString 的 toStringList() 返回**含整串的单元素列表**而非空表，那种写法
    // 永远进不到 split 分支，会把 "m-a,m-b" 当成一个叫 "m-a,m-b" 的模型（tst_providerspec 抓出）。
    const QVariant modelsVar = map.value(QStringLiteral("models"));
    QStringList models;
    if (modelsVar.typeId() == QMetaType::QString)
        models = modelsVar.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    else
        models = modelsVar.toStringList();
    for (const QString &m : models)
    {
        const QString name = normalizeModelName(m);
        if (name.isEmpty() || p.models.contains(name))
            continue;
        p.models.append(name);
        if (p.models.size() >= kModelsPerProviderMax)
            break;
    }
    return p;
}

// 盘上的 QVariantList → Provider 列表。非 map 的条目跳过；id 缺失的按 seed 补一个
// （否则按 id 定位 provider 的编辑/删除操作会全部命中第一条）；条数截 kProvidersMax。
inline QList<Provider> parseProviders(const QVariant &stored)
{
    QList<Provider> result;
    const QVariantList list = stored.toList();
    QStringList takenIds;
    for (const QVariant &item : list)
    {
        if (!item.canConvert<QVariantMap>())
            continue;
        Provider p = providerFromMap(item.toMap());
        if (p.id.isEmpty())
            p.id = uniqueProviderId(p.name + QLatin1Char('|') + p.baseUrl, takenIds);
        if (takenIds.contains(p.id))
            p.id = uniqueProviderId(p.id, takenIds);
        takenIds.append(p.id);
        result.append(p);
        if (result.size() >= kProvidersMax)
            break;
    }
    return result;
}

// Provider 列表 → 可落盘的 QVariantList。与 parseProviders 成对，供测试断言往返一致。
// 空 models 也照写（语义是「这个 provider 还没配模型」，不是「没有这个 provider」）。
inline QVariantList toVariantList(const QList<Provider> &providers)
{
    QVariantList list;
    list.reserve(providers.size());
    for (const Provider &p : providers)
    {
        QVariantMap map;
        map.insert(QStringLiteral("id"), p.id);
        map.insert(QStringLiteral("name"), p.name);
        map.insert(QStringLiteral("baseUrl"), p.baseUrl);
        map.insert(QStringLiteral("token"), p.token);
        map.insert(QStringLiteral("models"), QVariant::fromValue(p.models));
        list.append(map);
    }
    return list;
}

// ---- 旧三键兼容：合成单 provider ----

// 由旧 apiBaseUrl / apiToken / modelOptions 合成一个 provider。
// 固定 id 而非散列生成：旧配置升上来时 id 必须可预测，否则用户手写的
// defaultModel 之外任何按 id 的引用都会在两次读取间漂移。
inline Provider legacyProvider(const QString &baseUrl, const QString &token, const QString &modelOptionsCsv)
{
    Provider p;
    p.id = QStringLiteral("legacy");
    p.name = QStringLiteral("默认");
    p.baseUrl = normalizeBaseUrl(baseUrl);
    p.token = token;
    const QStringList parts = modelOptionsCsv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts)
    {
        const QString name = normalizeModelName(part);
        if (name.isEmpty() || p.models.contains(name))
            continue;
        p.models.append(name);
        if (p.models.size() >= kModelsPerProviderMax)
            break;
    }
    return p;
}

// 生效 provider 表：新键解析结果为空即回落到旧三键合成的单条。
// 回落判据刻意用「表空」而不是「新键不存在」——用户把 providers 删空数组，
// 语义上等同于「没配」，此时必须还能用旧三键，不能把凭据弄丢。
inline QList<Provider> effectiveProviders(const QList<Provider> &parsed,
                                          const QString &legacyBaseUrl,
                                          const QString &legacyToken,
                                          const QString &legacyModelOptions)
{
    if (!parsed.isEmpty())
        return parsed;
    QList<Provider> result;
    const Provider legacy = legacyProvider(legacyBaseUrl, legacyToken, legacyModelOptions);
    // 旧三键全空 = 什么都没配，返回空表（消费方据此判「未配置」，与改造前语义一致）
    if (!legacy.baseUrl.isEmpty() || !legacy.token.isEmpty() || !legacy.models.isEmpty())
        result.append(legacy);
    return result;
}

// ---- 摊平与路由 ----

// 跨 provider 摊平成一份下拉清单：保序去重（同名模型只留第一次出现的条目，
// 但重名本身由 duplicateModels() 在写入期拦下，这里只是读取期的兜底防御）。
inline QStringList allModels(const QList<Provider> &providers)
{
    QStringList result;
    for (const Provider &p : providers)
    {
        for (const QString &m : p.models)
        {
            if (result.contains(m))
                continue;
            result.append(m);
            if (result.size() >= kModelsTotalMax)
                return result;
        }
    }
    return result;
}

// 模型名 → provider 下标。空模型名与查无一律 -1（调用方回落到全局旧凭据，
// 保证「没配 provider 但配了旧三键 + MODEL_ID 环境变量」这条既有通路不断）。
inline int route(const QList<Provider> &providers, const QString &model)
{
    const QString name = model.trimmed();
    if (name.isEmpty())
        return -1;
    for (int i = 0; i < providers.size(); ++i)
    {
        if (providers.at(i).models.contains(name))
            return i;
    }
    return -1;
}

// ---- 请求凭据装配：多 Provider 路由的唯一落点（s18.3 抽头，串钥修复批）----
// 为什么在这里而不是 QOpenAi.cpp：路由判据（route）与「这一发请求该用哪份凭据」必须同处
// 可测。曾有的缺陷形态：端点按 model 路由去了乙家，Authorization 却仍拿全局旧三键的 token
// ——请求打到乙家端点却携带甲家的 key，轻则静默 401、重则把甲家密钥泄露给第三方。故把
// 「凭据反查」与「头部构造」都收进本头：头部只能由路由出的 creds 构造，接线层拿不到别的
// token 来源。QOpenAi::sendRequest 是唯一消费点；六条链并发发请求，凭据跟着请求体里的
// model 走，不维护「当前 provider」全局状态（理由详见本头顶部「路由形态」节）。
struct RequestCreds
{
    QString baseUrl;
    QString token;
};

// 按模型名反查这一发该用的基址与密钥；查不到（providers 未配、或该 model 不在任何清单里）
// 一律回落全局旧三键，保住「只配旧三键」与「MODEL_ID 环境变量」两条既有通路行为逐字不变。
inline RequestCreds credsForModel(const QList<Provider> &providers, const QString &model,
                                  const QString &fallbackBaseUrl, const QString &fallbackToken)
{
    const int idx = route(providers, model);
    if (idx >= 0)
        return { providers.at(idx).baseUrl, providers.at(idx).token };
    return { fallbackBaseUrl, fallbackToken };
}

// Authorization 头部单源：只能由路由出的 creds 构造（防「端点路由了、token 没路由」的串钥形态）。
inline QByteArray authorizationHeader(const RequestCreds &creds)
{
    return QByteArray("Bearer ") + creds.token.toUtf8();
}

// provider id → 下标；查无 -1。
inline int indexOfId(const QList<Provider> &providers, const QString &id)
{
    for (int i = 0; i < providers.size(); ++i)
    {
        if (providers.at(i).id == id)
            return i;
    }
    return -1;
}

// 模型名 → 归属服务商 id。这是「名字跨服务商唯一」那条不变量的**读取侧口径**：
// 弹窗据此报「已属于服务商 X」而不是含糊的「已在清单中」（含糊的报错正是用户把
// 「甲加了模型、乙也显示」读成「服务商共用模型」的原因），也据此把「移除」限定在
// 本服务商、不牵连别家同名条目。
// 空入参名与查无一律返回空串。入参先 trim——与 route() 同口径，否则同一个名字在
// 「路由到哪家」与「归属哪家」两问上会给出不同答案（表里存的是归一后的名字，
// 而调用方手里可能是用户刚输入的带空白串）。
// 表里若真出现重名（手改 config.json 写出来的），返回**首个**命中——与 allModels()
// 的保序去重同口径，两处不会给出不同归属。
inline QString ownerIdOfModel(const QList<Provider> &providers, const QString &model)
{
    const QString name = model.trimmed();
    if (name.isEmpty())
        return QString();
    for (const Provider &provider : providers)
    {
        if (provider.models.contains(name))
            return provider.id;
    }
    return QString();
}

// 跨 provider 重名的模型名集合（写入期校验用）。只报重名项，不报第一次出现。
inline QStringList duplicateModels(const QList<Provider> &providers)
{
    QStringList seen;
    QStringList dupes;
    for (const Provider &p : providers)
    {
        for (const QString &m : p.models)
        {
            if (seen.contains(m))
            {
                if (!dupes.contains(m))
                    dupes.append(m);
                continue;
            }
            seen.append(m);
        }
    }
    return dupes;
}

// 展示名：「provider 名/模型名」。provider 无名时回落 id，两者都空则只给模型名。
// 下拉本体不用它（下拉必须显示服务端认得的裸模型名），只给设置页与日志用。
inline QString displayLabel(const Provider &provider, const QString &model)
{
    const QString owner = provider.name.isEmpty() ? provider.id : provider.name;
    if (owner.isEmpty())
        return model;
    return owner + QLatin1Char('/') + model;
}

// ---- B：/models 响应解析（纯函数，网络层只负责把字节交进来）----

// 解析 OpenAI 兼容的模型列表响应 → 模型名清单。容错三种形态：
//   {"data":[{"id":"gpt-x"},…]}   标准 OpenAI 形态
//   {"models":["gpt-x",…]}        部分网关的简化形态
//   [{"id":"gpt-x"},…]            顶层直接给数组
// 条目里 id 优先、name 兜底；保序去重；截 kModelsResponseMax。
// 为什么要容三种：本头不假设对面是哪家，而拉取失败时用户仍需能手填——
// 解析器越宽容，「拉不到」就越少退化成「只能去抄文档」。
inline QStringList parseModelsResponse(const QByteArray &payload)
{
    QStringList result;
    QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (doc.isNull() || doc.isEmpty())
        return result;

    QJsonArray items;
    if (doc.isArray())
    {
        items = doc.array();
    }
    else if (doc.isObject())
    {
        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("data")).isArray())
            items = root.value(QStringLiteral("data")).toArray();
        else if (root.value(QStringLiteral("models")).isArray())
            items = root.value(QStringLiteral("models")).toArray();
    }

    for (const QJsonValue &value : items)
    {
        QString name;
        if (value.isString())
        {
            name = value.toString();
        }
        else if (value.isObject())
        {
            const QJsonObject obj = value.toObject();
            name = obj.value(QStringLiteral("id")).toString();
            if (name.trimmed().isEmpty())
                name = obj.value(QStringLiteral("name")).toString();
        }
        name = normalizeModelName(name);
        if (name.isEmpty() || result.contains(name))
            continue;
        result.append(name);
        if (result.size() >= kModelsResponseMax)
            break;
    }
    return result;
}

// 从拉取结果里挑出可追加进某 provider 的新模型：丢掉已存在的、丢掉与别的 provider
// 撞名的（撞名会让 route() 歧义，宁可不加也不能静默双写）。
inline QStringList mergeableModels(const Provider &target,
                                   const QStringList &fetched,
                                   const QList<Provider> &all)
{
    QStringList merged = target.models;
    QStringList added;
    for (const QString &m : fetched)
    {
        const QString name = normalizeModelName(m);
        if (name.isEmpty() || merged.contains(name))
            continue;
        bool clashes = false;
        for (const Provider &p : all)
        {
            if (p.id == target.id)
                continue;
            if (p.models.contains(name))
            {
                clashes = true;
                break;
            }
        }
        if (clashes)
            continue;
        merged.append(name);
        added.append(name);
        if (merged.size() >= kModelsPerProviderMax)
            break;
    }
    return added;
}

// ---- 表级增删改（纯函数：收旧表、返新表，不触盘、不改入参）----

// 新增/更新一条 provider，返回新表。两处判据必须在纯头里而非 UI 层：
//  ① 撞名拦截：route() 靠模型名唯一才成立，同名模型挂两个 provider 会让凭据归属变成
//     「看遍历顺序」，是并发下最难查的错。故**拒绝写入**并回抛冲突项，而不是静默去重
//     （静默去重会让用户以为两个 provider 都有这个模型）。
//  ② id 分配：新增走 uniqueProviderId，更新必须**沿用原 id**——id 是 provider 的稳定
//     标识，改 id 等于换了一条记录，会让「编辑后删除失败」这类问题无法归因。
// error 非空即表示被拒绝，调用方须把原因显示给用户（settings 页不得吞掉）。
inline QList<Provider> upsertProvider(const QList<Provider> &current,
                                      const Provider &incoming,
                                      QString &error)
{
    error.clear();

    const QString baseUrl = normalizeBaseUrl(incoming.baseUrl);
    if (!baseUrl.isEmpty() && !isValidBaseUrl(baseUrl))
    {
        error = QStringLiteral("invalid-base-url");
        return current;
    }

    // 撞名：与**别的** provider 重名才拦。自己改自己（同 id）不算撞。
    QStringList clashes;
    for (const Provider &other : current)
    {
        if (!incoming.id.isEmpty() && other.id == incoming.id)
            continue;
        for (const QString &m : incoming.models)
        {
            if (other.models.contains(m) && !clashes.contains(m))
                clashes.append(m);
        }
    }
    if (!clashes.isEmpty())
    {
        error = QStringLiteral("duplicate-model:") + clashes.join(QLatin1Char(','));
        return current;
    }

    QList<Provider> next = current;
    Provider normalized = incoming;
    normalized.baseUrl = baseUrl;
    normalized.name = normalizeProviderName(normalized.name);

    QStringList takenIds;
    for (const Provider &p : next)
        takenIds.append(p.id);

    // id 规范化**必须在 indexOfId 之前**（顺序错过一次，被 tst_providerspec 当场抓到）：
    // 若先查后补，incoming.id 为空时 indexOfId 会拿空串去比表内 id，而手改 config.json
    // 完全能在表里留下一条 id 为空的行——空串命中空串，新服务商就地覆盖那一行，
    // 用户点「新增」却抹掉了既有配置且无任何报错。先补 id 后，incoming.id 恒非空，
    // 空 id 行永不匹配，只会走追加；表内那条畸形行原样留着，交给读侧 parseProviders 下次修正。
    const bool isNew = normalized.id.isEmpty();
    if (isNew)
        normalized.id = uniqueProviderId(normalized.name + QLatin1Char('|') + normalized.baseUrl, takenIds);

    const int at = isNew ? -1 : indexOfId(next, normalized.id);
    if (at >= 0)
    {
        normalized.id = next.at(at).id; // 更新沿用原 id
        next[at] = normalized;
        return next;
    }
    if (next.size() >= kProvidersMax)
    {
        error = QStringLiteral("providers-cap");
        return current;
    }
    next.append(normalized);
    return next;
}

// 按 id 删除一条。删空后返回空表——消费侧（loadProviders）会据此回落到旧三键，
// 这是刻意的：把「最后一个 provider」删掉应当退回改造前的形态，而不是留一张空表
// 让下拉彻底没项。
inline QList<Provider> removeProvider(const QList<Provider> &current, const QString &id)
{
    QList<Provider> next;
    for (const Provider &p : current)
    {
        if (p.id != id)
            next.append(p);
    }
    return next;
}

// 往某 provider 追加一条模型（走 upsert 的撞名判据，避免 UI 各自实现一套去重）。
inline QList<Provider> addModelToProvider(const QList<Provider> &current,
                                          const QString &providerId,
                                          const QString &model,
                                          QString &error)
{
    error.clear();
    const int at = indexOfId(current, providerId);
    if (at < 0)
    {
        error = QStringLiteral("unknown-provider");
        return current;
    }
    const QString name = normalizeModelName(model);
    if (name.isEmpty())
    {
        error = QStringLiteral("empty-model-name");
        return current;
    }
    Provider target = current.at(at);
    if (target.models.contains(name))
        return current; // 已存在：幂等，不算错也不算撞
    if (target.models.size() >= kModelsPerProviderMax)
    {
        error = QStringLiteral("models-cap");
        return current;
    }
    target.models.append(name);
    return upsertProvider(current, target, error);
}

// 从某 provider 摘掉一条模型（不碰别的 provider）。
inline QList<Provider> removeModelFromProvider(const QList<Provider> &current,
                                               const QString &providerId,
                                               const QString &model)
{
    QList<Provider> next = current;
    const int at = indexOfId(next, providerId);
    if (at < 0)
        return next;
    Provider target = next.at(at);
    target.models.removeAll(model);
    next[at] = target;
    return next;
}

// ============================================================================
// 读盘薄壳：本头唯一触配置的地方。测试不覆盖它（口径同 AgentConstants.h 的 *_value 族——
// 经 AppSettings::reader() 要求 QCoreApplication 实例且路径钉死在 exe 目录），
// 上面所有判据都是收参数的纯函数，本节的职责只是「把盘上的东西喂给它们」。
// ============================================================================

// 「一个列表」的两种盘上形态 → 逗号串。json 数组落 **QVariantList** 而非 QStringList
// （Qt 6.9 实测 typeId=9，`== QStringList` 判据为假），裸逗号串落 QString；两种的
// .toString() 都可能给出错的东西，故必须两种都判——只判一种就会被误判「未配置」而静默回退。
// 这段逻辑原先只住在 AgentConstants.h::normalizedText 里；现在单源搬到这里，
// 那边改成转发，避免两处各持一份而漂移（也解开 ProviderSpec ↔ AgentConstants 的循环 include）。
inline QString listToCsv(const QVariant &stored)
{
    if (stored.metaType() == QMetaType::fromType<QStringList>()
        || stored.metaType() == QMetaType::fromType<QVariantList>())
        return stored.toStringList().join(QLatin1Char(','));
    return stored.toString();
}

// 取生效 provider 表：新键 app/providers 优先，缺失**或空数组**一律回落旧三键合成的单条。
// 走 AppSettings::value()（缓存解析产物、按文件身份惰性重载），故手改 config.json 下一次取值即生效。
inline QList<Provider> loadProviders()
{
    const QList<Provider> parsed = parseProviders(AppSettings::value(kProvidersKey));
    return effectiveProviders(parsed,
                              AppSettings::value(kLegacyBaseUrlKey).toString(),
                              AppSettings::value(kLegacyTokenKey).toString(),
                              listToCsv(AppSettings::value(kLegacyModelOptionsKey)));
}

// 落盘 provider 表。返回 false = 写失败（文件只读/被占用/磁盘满），调用方必须显式告知用户，
// 不能像早期那样丢弃返回值——那是「改了没反应且无任何说明」的成因。
inline bool saveProviders(const QList<Provider> &providers)
{
    return AppSettings::setValue(kProvidersKey, QVariant(toVariantList(providers)));
}

// 跨 provider 摊平后的全量可选模型。空表返回空清单，是否回退内置由调用方决定
// （AgentConst::modelOptions 负责那层回退）。
inline QStringList effectiveModelOptions()
{
    return allModels(loadProviders());
}

} // namespace ProviderSpec
