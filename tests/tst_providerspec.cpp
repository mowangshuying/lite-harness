// tst_providerspec.cpp —— 多 Provider 数据层纯头（src/ProviderSpec.h，纯 QtCore）
// 被测：基址归一与合法性、id 生成的确定性与防撞、provider 解析的容错与上限、
//       新旧键回落、跨 provider 摊平与按模型名路由、/models 响应三形态解析、
//       拉取结果合并时的撞名拦截。
//
// 为什么必须测：本头是「按模型名反查凭据」这条路由的唯一判据来源。route() 判错
// 不是报错，而是**静默拿错 key 打错端点**——并发会话下表现为随机 401，界面复现不出来。
// 同理 effectiveProviders() 的回落判据若写错（比如只认「新键不存在」而不认「新键是空
// 数组」），用户删空 providers 就会把旧三键的凭据一起弄丢。这两类都是无日志的错，
// 只能靠断言钉。
//
// 注意：本套件**不调用** loadProviders 一类触盘取值（经 AppSettings::reader() 要求
// QCoreApplication 实例且路径钉死在测试 exe 目录，接线层不在本套件职责内，口径同
// tst_agentconst）。所有被测函数一律收参数、返结果，零夹具恒跑。

#include "TestHarness.h"

#include "ProviderSpec.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace {

using TestHarness::check;

QVariantMap makeProviderMap(const QString &id,
                            const QString &name,
                            const QString &baseUrl,
                            const QString &token,
                            const QStringList &models)
{
    QVariantMap map;
    if (!id.isEmpty())
        map.insert(QStringLiteral("id"), id);
    map.insert(QStringLiteral("name"), name);
    map.insert(QStringLiteral("baseUrl"), baseUrl);
    map.insert(QStringLiteral("token"), token);
    map.insert(QStringLiteral("models"), QVariant::fromValue(models));
    return map;
}

ProviderSpec::Provider makeProvider(const QString &id, const QString &name, const QStringList &models)
{
    ProviderSpec::Provider p;
    p.id = id;
    p.name = name;
    p.baseUrl = QStringLiteral("https://example.invalid/v1");
    p.token = QStringLiteral("sk-test");
    p.models = models;
    return p;
}

} // namespace

int tst_providerspec()
{
    const int before = TestHarness::failCount();

    // ---- ① 基址归一：尾部斜杠全去、首尾空白去净 ----
    check(ProviderSpec::normalizeBaseUrl(QStringLiteral("https://a.example/v1///"))
              == QStringLiteral("https://a.example/v1"),
          "normalizeBaseUrl: strips all trailing slashes");
    check(ProviderSpec::normalizeBaseUrl(QStringLiteral("  https://a.example/v1  "))
              == QStringLiteral("https://a.example/v1"),
          "normalizeBaseUrl: trims surrounding whitespace");
    check(ProviderSpec::normalizeBaseUrl(QStringLiteral("https://a.example")).isEmpty() == false,
          "normalizeBaseUrl: bare base kept");
    // 只有一个斜杠的根不能整串吃掉成空——那是「https://host/」的合法写法
    check(ProviderSpec::normalizeBaseUrl(QStringLiteral("https://a.example/"))
              == QStringLiteral("https://a.example"),
          "normalizeBaseUrl: single trailing slash removed");

    // ---- ② 基址合法性：scheme 必须 http/https，裸主机名天然拒 ----
    check(ProviderSpec::isValidBaseUrl(QStringLiteral("https://a.example/v1")),
          "isValidBaseUrl: https accepted");
    check(ProviderSpec::isValidBaseUrl(QStringLiteral("http://127.0.0.1:11434/v1")),
          "isValidBaseUrl: http accepted (local gateway)");
    check(!ProviderSpec::isValidBaseUrl(QStringLiteral("a.example/v1")),
          "isValidBaseUrl: bare host rejected");
    check(!ProviderSpec::isValidBaseUrl(QStringLiteral("ftp://a.example/v1")),
          "isValidBaseUrl: non-http scheme rejected");
    check(!ProviderSpec::isValidBaseUrl(QString()),
          "isValidBaseUrl: empty rejected");

    // ---- ③ /models 端点拼接与 chat/completions 同构（都基于去尾斜杠的基址）----
    check(ProviderSpec::modelsEndpointFor(QStringLiteral("https://a.example/v1/"))
              == QStringLiteral("https://a.example/v1/models"),
          "modelsEndpointFor: appends /models to normalized base");

    // ---- ④ id 生成：同种子必同值（可测性），且防撞不返回已占用的 id ----
    const QString first = ProviderSpec::makeProviderId(QStringLiteral("阿里云"));
    check(first == ProviderSpec::makeProviderId(QStringLiteral("阿里云")),
          "makeProviderId: deterministic for same seed");
    check(first != ProviderSpec::makeProviderId(QStringLiteral("DeepSeek")),
          "makeProviderId: differs across seeds");
    check(first.startsWith(QStringLiteral("p-")) && first.size() == 10,
          "makeProviderId: shape is p-<8 hex>");
    {
        QStringList taken;
        taken << first;
        const QString second = ProviderSpec::uniqueProviderId(QStringLiteral("阿里云"), taken);
        check(second != first && !second.isEmpty(),
              "uniqueProviderId: avoids a taken id");
        check(ProviderSpec::uniqueProviderId(QStringLiteral("x"), QStringList{})
                  == ProviderSpec::makeProviderId(QStringLiteral("x")),
              "uniqueProviderId: free seed yields plain hash id");
    }

    // ---- ⑤ providerFromMap：字段缺失降级为空而非失败；模型名清洗去重 ----
    {
        const ProviderSpec::Provider p = ProviderSpec::providerFromMap(QVariantMap{});
        check(p.id.isEmpty() && p.baseUrl.isEmpty() && p.models.isEmpty(),
              "providerFromMap: empty map degrades to empty provider");
    }
    {
        const QVariantMap map = makeProviderMap(QStringLiteral("p-1"), QStringLiteral("甲"),
                                                QStringLiteral("https://a/v1//"), QStringLiteral("t"),
                                                {QStringLiteral(" m-a "), QStringLiteral("m-a"),
                                                 QString(), QStringLiteral("m-b")});
        const ProviderSpec::Provider p = ProviderSpec::providerFromMap(map);
        check(p.baseUrl == QStringLiteral("https://a/v1"), "providerFromMap: baseUrl normalized");
        check(p.models == QStringList({QStringLiteral("m-a"), QStringLiteral("m-b")}),
              "providerFromMap: models trimmed, deduped, empties dropped, order kept");
    }
    // 单条 provider 的模型数上限
    {
        QStringList many;
        for (int i = 0; i < ProviderSpec::kModelsPerProviderMax + 20; ++i)
            many << QStringLiteral("m%1").arg(i);
        const QVariantMap map =
            makeProviderMap(QStringLiteral("p-2"), QStringLiteral("乙"), QStringLiteral("https://b/v1"),
                            QStringLiteral("t"), many);
        const ProviderSpec::Provider p = ProviderSpec::providerFromMap(map);
        check(p.models.size() == ProviderSpec::kModelsPerProviderMax,
              "providerFromMap: per-provider model count capped");
    }
    // 超名模型名截断
    {
        const QString longName(ProviderSpec::kModelNameMax + 40, QLatin1Char('x'));
        check(ProviderSpec::normalizeModelName(longName).size() == ProviderSpec::kModelNameMax,
              "normalizeModelName: over-long name truncated to cap");
    }

    // ---- ⑥ models 的裸逗号串形态也要读得回（合流成 json 后只判数组会静默判成未配置）----
    {
        QVariantMap map;
        map.insert(QStringLiteral("id"), QStringLiteral("p-csv"));
        map.insert(QStringLiteral("name"), QStringLiteral("串形态"));
        map.insert(QStringLiteral("baseUrl"), QStringLiteral("https://c/v1"));
        map.insert(QStringLiteral("models"), QStringLiteral("m-a, m-b ,m-a,"));
        const ProviderSpec::Provider p = ProviderSpec::providerFromMap(map);
        check(p.models == QStringList({QStringLiteral("m-a"), QStringLiteral("m-b")}),
              "providerFromMap: bare comma string models form parsed and deduped");
    }

    // ---- ⑦ parseProviders：非 map 条目跳过、id 缺失补、id 重复消歧、条数截断 ----
    {
        QVariantList list;
        list.append(QStringLiteral("not-a-map"));
        list.append(makeProviderMap(QStringLiteral("p-a"), QStringLiteral("甲"), QStringLiteral("https://a/v1"),
                                    QStringLiteral("t"), {QStringLiteral("m-a")}));
        list.append(makeProviderMap(QString(), QStringLiteral("无id"), QStringLiteral("https://b/v1"),
                                    QStringLiteral("t"), {QStringLiteral("m-b")}));
        list.append(makeProviderMap(QStringLiteral("p-a"), QStringLiteral("甲重复"),
                                    QStringLiteral("https://c/v1"), QStringLiteral("t"),
                                    {QStringLiteral("m-c")}));
        const QList<ProviderSpec::Provider> parsed = ProviderSpec::parseProviders(QVariant(list));
        check(parsed.size() == 3, "parseProviders: non-map entry skipped, rest kept");
        check(parsed.at(1).id == ProviderSpec::makeProviderId(QStringLiteral("无id|https://b/v1")),
              "parseProviders: missing id derived from name+baseUrl");
        check(parsed.at(0).id == QStringLiteral("p-a") && parsed.at(2).id != QStringLiteral("p-a"),
              "parseProviders: duplicate id disambiguated, first keeps its own");
    }
    {
        QVariantList list;
        for (int i = 0; i < ProviderSpec::kProvidersMax + 9; ++i)
            list.append(makeProviderMap(QStringLiteral("p%1").arg(i), QStringLiteral("n%1").arg(i),
                                        QStringLiteral("https://x/v1"), QStringLiteral("t"),
                                        {QStringLiteral("m%1").arg(i)}));
        check(ProviderSpec::parseProviders(QVariant(list)).size() == ProviderSpec::kProvidersMax,
              "parseProviders: provider count capped");
    }

    // ---- ⑧ 落盘往返：toVariantList → parseProviders 必须逐字段复原 ----
    {
        QList<ProviderSpec::Provider> src;
        src.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {QStringLiteral("m-a"), QStringLiteral("m-b")}));
        ProviderSpec::Provider tail;
        tail.id = QStringLiteral("p-2");
        tail.name = QStringLiteral("乙");
        tail.baseUrl = QStringLiteral("https://b/v1");
        src.append(tail);
        const QList<ProviderSpec::Provider> back =
            ProviderSpec::parseProviders(ProviderSpec::toVariantList(src));
        check(back.size() == 2, "roundtrip: provider count preserved");
        check(back.at(0).id == src.at(0).id && back.at(0).name == src.at(0).name
                  && back.at(0).token == src.at(0).token
                  && back.at(0).models == src.at(0).models,
              "roundtrip: full provider fields survive");
        check(back.at(1).models.isEmpty() && back.at(1).id == QStringLiteral("p-2"),
              "roundtrip: empty models kept (not dropped as 'no provider')");
    }

    // ---- ⑨ legacyProvider：旧三键合成，id 固定可预测 ----
    {
        const ProviderSpec::Provider p = ProviderSpec::legacyProvider(
            QStringLiteral("https://old/v1/"), QStringLiteral("sk-old"),
            QStringLiteral("qwen-a, qwen-b ,,qwen-a"));
        check(p.id == QStringLiteral("legacy"), "legacyProvider: id is the fixed literal");
        check(p.baseUrl == QStringLiteral("https://old/v1"), "legacyProvider: baseUrl normalized");
        check(p.token == QStringLiteral("sk-old"), "legacyProvider: token passed through");
        check(p.models == QStringList({QStringLiteral("qwen-a"), QStringLiteral("qwen-b")}),
              "legacyProvider: csv split, trimmed, deduped");
    }

    // ---- ⑩ effectiveProviders：回落判据是「表空」而非「键缺失」 ----
    {
        QList<ProviderSpec::Provider> parsed;
        parsed.append(makeProvider(QStringLiteral("p-new"), QStringLiteral("新"), {QStringLiteral("m-new")}));
        const QList<ProviderSpec::Provider> eff =
            ProviderSpec::effectiveProviders(parsed, QStringLiteral("https://old/v1"),
                                             QStringLiteral("sk-old"), QStringLiteral("m-old"));
        check(eff.size() == 1 && eff.at(0).id == QStringLiteral("p-new"),
              "effectiveProviders: non-empty parsed wins, legacy ignored");
    }
    {
        // 用户把 providers 删成空数组：必须还能用旧三键，不能把凭据弄丢
        const QList<ProviderSpec::Provider> eff = ProviderSpec::effectiveProviders(
            QList<ProviderSpec::Provider>{}, QStringLiteral("https://old/v1"), QStringLiteral("sk-old"),
            QStringLiteral("m-old"));
        check(eff.size() == 1 && eff.at(0).id == QStringLiteral("legacy")
                  && eff.at(0).token == QStringLiteral("sk-old"),
              "effectiveProviders: empty array falls back to legacy keys (credentials survive)");
    }
    {
        // 新旧全空 = 什么都没配，返回空表让消费方判「未配置」
        check(ProviderSpec::effectiveProviders(QList<ProviderSpec::Provider>{}, QString(), QString(), QString())
                  .isEmpty(),
              "effectiveProviders: nothing configured yields empty table");
    }
    {
        // 只有 baseUrl 没有 key 也算配过（旧语义：启动只 warn 不拦，请求时才报）
        const QList<ProviderSpec::Provider> eff = ProviderSpec::effectiveProviders(
            QList<ProviderSpec::Provider>{}, QStringLiteral("https://old/v1"), QString(), QString());
        check(eff.size() == 1, "effectiveProviders: baseUrl-only legacy still surfaces");
    }

    // ---- ⑪ allModels：跨 provider 保序摊平 + 去重 + 总量上限 ----
    {
        QList<ProviderSpec::Provider> providers;
        providers.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"),
                                      {QStringLiteral("m-a"), QStringLiteral("m-b")}));
        providers.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"),
                                      {QStringLiteral("m-c"), QStringLiteral("m-a")}));
        check(ProviderSpec::allModels(providers)
                  == QStringList({QStringLiteral("m-a"), QStringLiteral("m-b"), QStringLiteral("m-c")}),
              "allModels: flattened in provider order, cross-provider dupe collapsed");
    }
    {
        QList<ProviderSpec::Provider> providers;
        QStringList many;
        for (int i = 0; i < ProviderSpec::kModelsTotalMax + 30; ++i)
            many << QStringLiteral("m%1").arg(i);
        providers.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), many));
        check(ProviderSpec::allModels(providers).size() == ProviderSpec::kModelsTotalMax,
              "allModels: total count capped");
    }

    // ---- ⑫ route：按模型名反查 provider 下标（本头的核心判据）----
    {
        QList<ProviderSpec::Provider> providers;
        providers.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"),
                                      {QStringLiteral("m-a"), QStringLiteral("m-b")}));
        providers.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"), {QStringLiteral("m-c")}));
        check(ProviderSpec::route(providers, QStringLiteral("m-a")) == 0, "route: first provider hit");
        check(ProviderSpec::route(providers, QStringLiteral("m-c")) == 1, "route: second provider hit");
        check(ProviderSpec::route(providers, QStringLiteral("  m-b  ")) == 0,
              "route: model name trimmed before lookup");
        check(ProviderSpec::route(providers, QStringLiteral("m-zz")) == -1,
              "route: unknown model returns -1 (caller falls back to global creds)");
        check(ProviderSpec::route(providers, QString()) == -1, "route: empty model returns -1");
        check(ProviderSpec::route(QList<ProviderSpec::Provider>{}, QStringLiteral("m-a")) == -1,
              "route: empty table returns -1");
        // 大小写敏感：模型名直接进请求体，改大小写可能 404，故不得折叠
        check(ProviderSpec::route(providers, QStringLiteral("M-A")) == -1,
              "route: model match is case-sensitive (names go verbatim into request body)");
    }

    // ---- ⑬ indexOfId ----
    {
        QList<ProviderSpec::Provider> providers;
        providers.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {}));
        providers.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"), {}));
        check(ProviderSpec::indexOfId(providers, QStringLiteral("p-2")) == 1, "indexOfId: hit");
        check(ProviderSpec::indexOfId(providers, QStringLiteral("p-9")) == -1, "indexOfId: miss");
        check(ProviderSpec::indexOfId(providers, QString()) == -1, "indexOfId: empty miss");
    }

    // ---- ⑭ duplicateModels：只报重名项，供写入期拦 ----
    {
        QList<ProviderSpec::Provider> providers;
        providers.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"),
                                      {QStringLiteral("m-a"), QStringLiteral("m-b")}));
        providers.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"),
                                      {QStringLiteral("m-b"), QStringLiteral("m-c")}));
        providers.append(makeProvider(QStringLiteral("p-3"), QStringLiteral("丙"),
                                      {QStringLiteral("m-a")}));
        const QStringList dupes = ProviderSpec::duplicateModels(providers);
        check(dupes.size() == 2 && dupes.contains(QStringLiteral("m-a"))
                  && dupes.contains(QStringLiteral("m-b")),
              "duplicateModels: reports each cross-provider dupe once");
        check(ProviderSpec::duplicateModels(QList<ProviderSpec::Provider>{}).isEmpty(),
              "duplicateModels: empty table has no dupes");
    }

    // ---- ⑮ displayLabel：仅设置页/日志用，下拉本体必须显示裸模型名 ----
    {
        const ProviderSpec::Provider named = makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {});
        check(ProviderSpec::displayLabel(named, QStringLiteral("m-a")) == QStringLiteral("甲/m-a"),
              "displayLabel: name/model");
        const ProviderSpec::Provider nameless = makeProvider(QStringLiteral("p-1"), QString(), {});
        check(ProviderSpec::displayLabel(nameless, QStringLiteral("m-a")) == QStringLiteral("p-1/m-a"),
              "displayLabel: falls back to id when name empty");
        const ProviderSpec::Provider bare;
        check(ProviderSpec::displayLabel(bare, QStringLiteral("m-a")) == QStringLiteral("m-a"),
              "displayLabel: bare model when no owner at all");
    }

    // ---- ⑯ parseModelsResponse：三种形态都要认，坏 JSON 给空表而非崩 ----
    const auto buildListPayload = [](const QStringList &ids) {
        QJsonArray arr;
        for (const QString &id : ids)
        {
            QJsonObject obj;
            obj.insert(QStringLiteral("id"), id);
            obj.insert(QStringLiteral("object"), QStringLiteral("model"));
            arr.append(obj);
        }
        QJsonObject root;
        root.insert(QStringLiteral("object"), QStringLiteral("list"));
        root.insert(QStringLiteral("data"), arr);
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    };
    {
        const QByteArray payload = buildListPayload({QStringLiteral("gpt-x"), QStringLiteral("gpt-y")});
        check(ProviderSpec::parseModelsResponse(payload)
                  == QStringList({QStringLiteral("gpt-x"), QStringLiteral("gpt-y")}),
              "parseModelsResponse: standard OpenAI data[].id form");
    }
    {
        QJsonArray arr;
        arr.append(QStringLiteral("m-1"));
        arr.append(QStringLiteral("m-2"));
        arr.append(QStringLiteral("m-1"));
        QJsonObject root;
        root.insert(QStringLiteral("models"), arr);
        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
        check(ProviderSpec::parseModelsResponse(payload)
                  == QStringList({QStringLiteral("m-1"), QStringLiteral("m-2")}),
              "parseModelsResponse: {models:[str]} form, deduped");
    }
    {
        QJsonArray arr;
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), QStringLiteral("m-by-name"));
        arr.append(obj);
        const QByteArray payload = QJsonDocument(arr).toJson(QJsonDocument::Compact);
        check(ProviderSpec::parseModelsResponse(payload) == QStringList({QStringLiteral("m-by-name")}),
              "parseModelsResponse: top-level array with name fallback");
    }
    {
        check(ProviderSpec::parseModelsResponse("not json at all").isEmpty(),
              "parseModelsResponse: garbage yields empty list, no crash");
        check(ProviderSpec::parseModelsResponse(QByteArray()).isEmpty(),
              "parseModelsResponse: empty payload yields empty list");
        QJsonObject root;
        root.insert(QStringLiteral("unexpected"), QStringLiteral("shape"));
        check(ProviderSpec::parseModelsResponse(
                  QJsonDocument(root).toJson(QJsonDocument::Compact))
                  .isEmpty(),
              "parseModelsResponse: unknown object shape yields empty list");
    }
    {
        QStringList many;
        for (int i = 0; i < ProviderSpec::kModelsResponseMax + 50; ++i)
            many << QStringLiteral("m%1").arg(i);
        check(ProviderSpec::parseModelsResponse(buildListPayload(many)).size()
                  == ProviderSpec::kModelsResponseMax,
              "parseModelsResponse: response count capped");
    }

    // ---- ⑰ mergeableModels：已存在的不重复加，与别的 provider 撞名的不加 ----
    {
        QList<ProviderSpec::Provider> all;
        ProviderSpec::Provider target = makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"),
                                                     {QStringLiteral("m-a")});
        all.append(target);
        all.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"), {QStringLiteral("m-taken")}));
        const QStringList fetched = {QStringLiteral("m-a"),      // 已在 target 里
                                     QStringLiteral("m-taken"),  // 被别的 provider 占了
                                     QStringLiteral("m-new"),    // 可加
                                     QStringLiteral("  ")};      // 空白丢弃
        const QStringList added = ProviderSpec::mergeableModels(target, fetched, all);
        check(added == QStringList({QStringLiteral("m-new")}),
              "mergeableModels: skips existing, skips cross-provider clash, skips blanks");
    }
    {
        // 上限：加到 kModelsPerProviderMax 就停
        QStringList existing;
        for (int i = 0; i < ProviderSpec::kModelsPerProviderMax - 2; ++i)
            existing << QStringLiteral("have%1").arg(i);
        const ProviderSpec::Provider target =
            makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), existing);
        QList<ProviderSpec::Provider> all;
        all.append(target);
        QStringList fetched;
        for (int i = 0; i < 30; ++i)
            fetched << QStringLiteral("new%1").arg(i);
        check(ProviderSpec::mergeableModels(target, fetched, all).size() == 2,
              "mergeableModels: stops at per-provider cap");
    }

    // ---- ⑰' 表级增删改：撞名必须拒绝而非静默去重（route 的唯一性前提）----
    {
        QList<ProviderSpec::Provider> base;
        base.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {QStringLiteral("m-a")}));
        QString err;

        // 新增一条不撞名的：应追加，且 id 自动分配非空
        ProviderSpec::Provider fresh;
        fresh.name = QStringLiteral("乙");
        fresh.baseUrl = QStringLiteral("https://b/v1/");
        fresh.token = QStringLiteral("t-b");
        fresh.models = {QStringLiteral("m-b")};
        const QList<ProviderSpec::Provider> added = ProviderSpec::upsertProvider(base, fresh, err);
        check(err.isEmpty() && added.size() == 2, "upsert: non-clashing provider appended");
        check(added.at(1).id.startsWith(QStringLiteral("p-")) && !added.at(1).id.isEmpty(),
              "upsert: new provider gets a generated id");
        check(added.at(1).baseUrl == QStringLiteral("https://b/v1"),
              "upsert: baseUrl normalized on write");

        // 同 id 再写 = 更新，条数不变且 id 沿用原值
        ProviderSpec::Provider upd = base.at(0);
        upd.name = QStringLiteral("甲改名");
        upd.token = QStringLiteral("t-new");
        const QList<ProviderSpec::Provider> edited = ProviderSpec::upsertProvider(base, upd, err);
        check(err.isEmpty() && edited.size() == 1 && edited.at(0).name == QStringLiteral("甲改名")
                  && edited.at(0).id == QStringLiteral("p-1"),
              "upsert: same id updates in place, keeps original id");

        // 与别的 provider 撞模型名：拒绝、原表不动、error 里带出撞的那个名字
        ProviderSpec::Provider clash;
        clash.name = QStringLiteral("丙");
        clash.baseUrl = QStringLiteral("https://c/v1");
        clash.models = {QStringLiteral("m-a")};
        const QList<ProviderSpec::Provider> rejected = ProviderSpec::upsertProvider(added, clash, err);
        check(!err.isEmpty() && rejected.size() == added.size(),
              "upsert: cross-provider duplicate model rejected, table unchanged");
        check(err.contains(QStringLiteral("m-a")), "upsert: error text names the clashing model");

        // 非法基址：拒绝且不落表
        ProviderSpec::Provider badUrl = fresh;
        badUrl.baseUrl = QStringLiteral("127.0.0.1:11434/v1");
        const QList<ProviderSpec::Provider> afterBad = ProviderSpec::upsertProvider(base, badUrl, err);
        check(!err.isEmpty() && afterBad.size() == base.size(),
              "upsert: invalid baseUrl rejected");

        // 空基址放行——provider 可以先建再填端点（与旧「空串=清除」口径一致，不得比旧口径更严）
        ProviderSpec::Provider emptyUrl = fresh;
        emptyUrl.baseUrl.clear();
        check(ProviderSpec::upsertProvider(base, emptyUrl, err).size() == 2 && err.isEmpty(),
              "upsert: empty baseUrl accepted (create-then-fill flow)");

        // provider 条数上限
        QList<ProviderSpec::Provider> many;
        for (int i = 0; i < ProviderSpec::kProvidersMax; ++i)
            many.append(makeProvider(QStringLiteral("p%1").arg(i), QStringLiteral("n%1").arg(i),
                                     {QStringLiteral("mm%1").arg(i)}));
        ProviderSpec::Provider overflow;
        overflow.name = QStringLiteral("溢出");
        overflow.baseUrl = QStringLiteral("https://o/v1");
        overflow.models = {QStringLiteral("mm-overflow")};
        const QList<ProviderSpec::Provider> capped = ProviderSpec::upsertProvider(many, overflow, err);
        check(capped.size() == ProviderSpec::kProvidersMax && !err.isEmpty(),
              "upsert: provider count cap rejects rather than silently truncating");
    }

    // ---- ⑰''' 空 id 的 incoming 必须走「追加」而非「覆盖」----
    // 为什么单独钉：设置页「新增服务商」传的就是 id 为空的 Provider，upsert 靠
    // indexOfId 判新增/更新。若哪天表里出现一条 id 为空的行（手改 config.json 完全写得出来），
    // indexOfId(表, "") 会命中它并把新服务商**覆盖上去**——用户点新增却改掉了原有那条，
    // 且毫无报错。parseProviders 会补空 id，但那是读侧兜底，不能替代写侧这条判据。
    {
        QList<ProviderSpec::Provider> base;
        base.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {QStringLiteral("m-a")}));
        QString err;
        ProviderSpec::Provider fresh; // id 刻意留空
        fresh.name = QStringLiteral("新来的");
        fresh.baseUrl = QStringLiteral("https://n/v1");
        fresh.models = {QStringLiteral("m-n")};
        const QList<ProviderSpec::Provider> out = ProviderSpec::upsertProvider(base, fresh, err);
        check(err.isEmpty() && out.size() == 2, "upsert: empty-id incoming appends rather than replaces");
        check(out.at(0).name == QStringLiteral("甲") && out.at(0).models == base.at(0).models,
              "upsert: existing row untouched by an empty-id append");
        check(!out.at(1).id.isEmpty(), "upsert: appended row gets a non-empty id");

        // 表里真有空 id 行（手改配置形态）时，追加仍不得吃掉它
        QList<ProviderSpec::Provider> withBlank;
        withBlank.append(makeProvider(QString(), QStringLiteral("空id行"), {QStringLiteral("m-blank")}));
        const QList<ProviderSpec::Provider> out2 = ProviderSpec::upsertProvider(withBlank, fresh, err);
        check(out2.size() == 2 && out2.at(0).models.contains(QStringLiteral("m-blank")),
              "upsert: blank-id row in table is not overwritten by a new provider");
    }

    // ---- ⑰'' removeProvider / addModel / removeModel ----
    {
        QList<ProviderSpec::Provider> base;
        base.append(makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"), {QStringLiteral("m-a")}));
        base.append(makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"), {QStringLiteral("m-b")}));

        const QList<ProviderSpec::Provider> removed = ProviderSpec::removeProvider(base, QStringLiteral("p-1"));
        check(removed.size() == 1 && removed.at(0).id == QStringLiteral("p-2"),
              "removeProvider: drops only the targeted id");
        check(ProviderSpec::removeProvider(base, QStringLiteral("p-none")).size() == 2,
              "removeProvider: unknown id is a no-op");
        check(ProviderSpec::removeProvider(base, base.at(0).id).size() == base.size() - 1,
              "removeProvider: deleting the last-but-one leaves one row");
        // 删到空表是合法结果：消费侧据此回落旧三键，不得在此处拦
        QList<ProviderSpec::Provider> single;
        single.append(base.at(0));
        check(ProviderSpec::removeProvider(single, base.at(0).id).isEmpty(),
              "removeProvider: last row may be removed (yields empty table)");

        QString err;
        const QList<ProviderSpec::Provider> withNew =
            ProviderSpec::addModelToProvider(base, QStringLiteral("p-1"), QStringLiteral("  m-new  "), err);
        check(err.isEmpty() && withNew.at(0).models.contains(QStringLiteral("m-new")),
              "addModelToProvider: appends trimmed name");
        check(withNew.at(1).models == base.at(1).models,
              "addModelToProvider: other providers untouched");

        // 幂等：重复加同一条不报错也不产生第二份
        const QList<ProviderSpec::Provider> twice =
            ProviderSpec::addModelToProvider(withNew, QStringLiteral("p-1"), QStringLiteral("m-new"), err);
        check(err.isEmpty() && twice.at(0).models.count(QStringLiteral("m-new")) == 1,
              "addModelToProvider: re-adding same name is idempotent");

        // 加到别的 provider 已占用的名字上：必须被 upsert 的撞名判据拦下
        ProviderSpec::addModelToProvider(base, QStringLiteral("p-1"), QStringLiteral("m-b"), err);
        check(!err.isEmpty(), "addModelToProvider: clashing name across providers rejected");

        // 空白名与未知 provider 各自报错，不改表
        err.clear();
        check(ProviderSpec::addModelToProvider(base, QStringLiteral("p-1"), QStringLiteral("   "), err).size()
                  == base.size()
              && !err.isEmpty(),
              "addModelToProvider: blank name rejected");
        err.clear();
        check(ProviderSpec::addModelToProvider(base, QStringLiteral("p-none"), QStringLiteral("m-x"), err).size()
                  == base.size()
              && !err.isEmpty(),
              "addModelToProvider: unknown provider rejected");

        const QList<ProviderSpec::Provider> shrunk =
            ProviderSpec::removeModelFromProvider(base, QStringLiteral("p-1"), QStringLiteral("m-a"));
        check(shrunk.at(0).models.isEmpty() && shrunk.at(1).models.size() == 1,
              "removeModelFromProvider: removes only from the targeted provider");

        // 摘掉某 provider 的最后一个模型后，该名字应可被别的 provider 使用（route 仍唯一）
        QString err2;
        const QList<ProviderSpec::Provider> moved =
            ProviderSpec::addModelToProvider(shrunk, QStringLiteral("p-2"), QStringLiteral("m-a"), err2);
        check(err2.isEmpty() && moved.at(1).models.contains(QStringLiteral("m-a")),
              "route uniqueness: freed model name becomes reusable by another provider");
    }

    // ---- ⑱ 键名是裸名（组前缀 app/ 由 AppSettings::scopedKey 统一拼）----
    check(!ProviderSpec::kProvidersKey.contains(QLatin1Char('/'))
              && !ProviderSpec::kProvidersKey.isEmpty(),
          "kProvidersKey: bare name, no group prefix");
    check(ProviderSpec::kProvidersKey != ProviderSpec::kLegacyBaseUrlKey
              && ProviderSpec::kProvidersKey != ProviderSpec::kLegacyTokenKey
              && ProviderSpec::kProvidersKey != ProviderSpec::kLegacyModelOptionsKey,
          "kProvidersKey: distinct from the three legacy keys");

    // ---- ⑲ 上限本身为正（写反成 0 会让所有配置静默变空表）----
    check(ProviderSpec::kProvidersMax > 0 && ProviderSpec::kModelsPerProviderMax > 0
              && ProviderSpec::kModelsTotalMax >= ProviderSpec::kModelsPerProviderMax
              && ProviderSpec::kModelNameMax > 0 && ProviderSpec::kModelsResponseMax > 0,
          "caps: all positive and mutually consistent");

    // ---- ⑳ 请求凭据装配：串钥修复回归（端点与 token 必须同源路由）----
    // 曾有的缺陷形态：QOpenAi::sendRequest 的端点按 model 路由去了乙家，Authorization
    // 却仍拿全局旧三键的 token——请求打到乙家端点却携带甲家的 key，轻则静默 401、
    // 重则把甲家密钥泄露给第三方。判据收进本头后，头部只能由路由出的 creds 构造。
    {
        QList<ProviderSpec::Provider> providers;
        ProviderSpec::Provider a = makeProvider(QStringLiteral("p-1"), QStringLiteral("甲"),
                                                {QStringLiteral("m-a")});
        a.baseUrl = QStringLiteral("https://a.example/v1");
        a.token = QStringLiteral("sk-A");
        ProviderSpec::Provider b = makeProvider(QStringLiteral("p-2"), QStringLiteral("乙"),
                                                {QStringLiteral("m-b")});
        b.baseUrl = QStringLiteral("https://b.example/v1");
        b.token = QStringLiteral("sk-B");
        providers.append(a);
        providers.append(b);

        // 命中乙家：基址与 token 都必须是乙家的（全局凭据刻意给不同值，串钥即现形）
        const ProviderSpec::RequestCreds hitB = ProviderSpec::credsForModel(
            providers, QStringLiteral("m-b"), QStringLiteral("https://global.example/v1"),
            QStringLiteral("sk-GLOBAL"));
        check(hitB.baseUrl == QStringLiteral("https://b.example/v1")
                  && hitB.token == QStringLiteral("sk-B"),
              "credsForModel: routed provider supplies BOTH baseUrl and token");
        check(ProviderSpec::authorizationHeader(hitB) == QByteArray("Bearer sk-B"),
              "authorizationHeader: built from routed creds, not the global token (cross-key regression)");

        // 命中甲家同理
        const ProviderSpec::RequestCreds hitA = ProviderSpec::credsForModel(
            providers, QStringLiteral("m-a"), QStringLiteral("https://global.example/v1"),
            QStringLiteral("sk-GLOBAL"));
        check(hitA.token == QStringLiteral("sk-A")
                  && ProviderSpec::authorizationHeader(hitA) == QByteArray("Bearer sk-A"),
              "credsForModel: first provider hit carries its own token");

        // 查无回落全局旧三键（旧通路行为逐字不变）
        const ProviderSpec::RequestCreds miss = ProviderSpec::credsForModel(
            providers, QStringLiteral("m-zz"), QStringLiteral("https://global.example/v1"),
            QStringLiteral("sk-GLOBAL"));
        check(miss.baseUrl == QStringLiteral("https://global.example/v1")
                  && miss.token == QStringLiteral("sk-GLOBAL")
                  && ProviderSpec::authorizationHeader(miss) == QByteArray("Bearer sk-GLOBAL"),
              "credsForModel: unknown model falls back to global legacy creds");

        // 空表 / 空模型名同样回落
        const ProviderSpec::RequestCreds emptyTable = ProviderSpec::credsForModel(
            QList<ProviderSpec::Provider>{}, QStringLiteral("m-a"),
            QStringLiteral("https://global.example/v1"), QStringLiteral("sk-GLOBAL"));
        check(emptyTable.token == QStringLiteral("sk-GLOBAL"),
              "credsForModel: empty provider table falls back to global");
        const ProviderSpec::RequestCreds emptyModel = ProviderSpec::credsForModel(
            providers, QString(), QStringLiteral("https://global.example/v1"),
            QStringLiteral("sk-GLOBAL"));
        check(emptyModel.token == QStringLiteral("sk-GLOBAL"),
              "credsForModel: empty model name falls back to global");

        // 模型名 trim 口径与 route 一致
        const ProviderSpec::RequestCreds trimmed = ProviderSpec::credsForModel(
            providers, QStringLiteral("  m-b  "), QStringLiteral("https://global.example/v1"),
            QStringLiteral("sk-GLOBAL"));
        check(trimmed.token == QStringLiteral("sk-B"),
              "credsForModel: model name trimmed before lookup (same as route)");

        // 全局凭据为空也不得拿别家的顶替（未配置判据在 sendRequest，这里只钉回落值原样）
        const ProviderSpec::RequestCreds noGlobal = ProviderSpec::credsForModel(
            providers, QStringLiteral("m-zz"), QString(), QString());
        check(noGlobal.baseUrl.isEmpty() && noGlobal.token.isEmpty(),
              "credsForModel: fallback passes global values through verbatim (empty stays empty)");
    }

    return TestHarness::failCount() - before;
}
