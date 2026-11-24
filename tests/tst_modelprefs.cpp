// tst_modelprefs —— 每模型偏好纯头 ModelPrefs.h 直测（s18.5 模型管理弹窗）。
// 零夹具恒跑：全部断言只调纯入参口，不触 AppSettings（便捷口要求 QCoreApplication，
// 与 tst_agentconst 同纪律）。钉的是「错了不报错、只静默改行为」的面：
// 布尔宽松读会把 "false" 串读成 true；effort/window 不归一会把垃圾值发进请求体；
// 显示过滤丢序会改下拉候选顺序；thinkingParams 关思考时多带 effort 会撞端点 400。

#include "ModelPrefs.h"

#include "TestHarness.h"

using TestHarness::check;

int tst_modelprefs()
{
    const int failBefore = TestHarness::failCount();

    // ---- ① 强度四档：wire 值与序 ----
    check(ModelPrefs::effortWireValues() == QStringList({"low", "medium", "high", "xhigh"}),
          "effortWireValues: four wire values in UI order");
    check(ModelPrefs::defaultEffort() == QStringLiteral("xhigh"),
          "defaultEffort: xhigh keeps pre-feature request bytes");
    check(ModelPrefs::normalizeEffort(QStringLiteral("low")) == QStringLiteral("low"),
          "normalizeEffort: valid value passes through");
    check(ModelPrefs::normalizeEffort(QStringLiteral("medium")) == QStringLiteral("medium"),
          "normalizeEffort: medium passes through");
    check(ModelPrefs::normalizeEffort(QStringLiteral("high")) == QStringLiteral("high"),
          "normalizeEffort: high passes through");
    check(ModelPrefs::normalizeEffort(QStringLiteral("xhigh")) == QStringLiteral("xhigh"),
          "normalizeEffort: xhigh passes through");
    check(ModelPrefs::normalizeEffort(QStringLiteral("XHIGH")) == QStringLiteral("xhigh"),
          "normalizeEffort: case variant rejected to default (wire values are case sensitive)");
    check(ModelPrefs::normalizeEffort(QString()) == QStringLiteral("xhigh"),
          "normalizeEffort: empty falls back to default");
    check(ModelPrefs::normalizeEffort(QStringLiteral("none")) == QStringLiteral("xhigh"),
          "normalizeEffort: unknown level falls back to default");
    check(ModelPrefs::effortIndex(QStringLiteral("low")) == 0, "effortIndex: low -> 0");
    check(ModelPrefs::effortIndex(QStringLiteral("medium")) == 1, "effortIndex: medium -> 1");
    check(ModelPrefs::effortIndex(QStringLiteral("high")) == 2, "effortIndex: high -> 2");
    check(ModelPrefs::effortIndex(QStringLiteral("xhigh")) == 3, "effortIndex: xhigh -> 3");
    check(ModelPrefs::effortIndex(QStringLiteral("garbage")) == 3,
          "effortIndex: invalid normalizes before indexing (never -1 into a combo)");

    // ---- ② 窗口 0~1M 连续钳位与输入框文本判据 ----
    check(ModelPrefs::windowMax() == 1000000, "windowMax: exactly 1M tokens");
    check(ModelPrefs::normalizeWindow(0) == 0, "normalizeWindow: zero stays unset");
    check(ModelPrefs::normalizeWindow(500000) == 500000, "normalizeWindow: mid value kept");
    check(ModelPrefs::normalizeWindow(1000000) == 1000000, "normalizeWindow: max kept");
    check(ModelPrefs::normalizeWindow(12345) == 12345,
          "normalizeWindow: continuous values are kept, not snapped to stops");
    check(ModelPrefs::normalizeWindow(-5) == 0, "normalizeWindow: negative clamps to unset");
    check(ModelPrefs::normalizeWindow(5000000) == 1000000,
          "normalizeWindow: above max clamps to 1M");
    check(ModelPrefs::windowFromText(QString()) == 0, "windowFromText: empty means unset");
    check(ModelPrefs::windowFromText(QStringLiteral("   ")) == 0,
          "windowFromText: whitespace means unset");
    check(ModelPrefs::windowFromText(QStringLiteral("200000")) == 200000,
          "windowFromText: plain integer kept");
    check(ModelPrefs::windowFromText(QStringLiteral(" 400000 ")) == 400000,
          "windowFromText: surrounding whitespace trimmed");
    check(ModelPrefs::windowFromText(QStringLiteral("1000000")) == 1000000,
          "windowFromText: max kept");
    check(ModelPrefs::windowFromText(QStringLiteral("12345")) == 12345,
          "windowFromText: continuous value kept");
    check(ModelPrefs::windowFromText(QStringLiteral("-5")) == 0,
          "windowFromText: negative clamps to unset");
    check(ModelPrefs::windowFromText(QStringLiteral("5000000")) == 1000000,
          "windowFromText: above max clamps to 1M");
    check(ModelPrefs::windowFromText(QStringLiteral("abc")) == -1,
          "windowFromText: non-numeric rejected, never written");
    check(ModelPrefs::windowFromText(QStringLiteral("20x")) == -1,
          "windowFromText: mixed text rejected");
    check(ModelPrefs::windowFromText(QStringLiteral("1.5")) == -1,
          "windowFromText: fractional rejected");
    check(ModelPrefs::windowFromText(QStringLiteral("0")) == 0,
          "windowFromText: explicit zero stays unset");
    check(ModelPrefs::windowFromText(QStringLiteral("+5")) == 5,
          "windowFromText: leading plus accepted as integer");
    check(ModelPrefs::windowFromText(QStringLiteral("999999999999")) == -1,
          "windowFromText: overflow rejected, never wraps");

    // ---- ③ parsePrefs 容错与严格布尔 ----
    const ModelPrefs::Prefs defaults;
    check(defaults.thinking && defaults.visible, "Prefs defaults: thinking and visible on");
    check(defaults.effort == QStringLiteral("xhigh") && defaults.window == 0,
          "Prefs defaults: xhigh and unset window");
    check(ModelPrefs::parsePrefs(QVariant(QStringLiteral("garbage"))).thinking,
          "parsePrefs: non-map yields defaults");
    check(ModelPrefs::parsePrefs(QVariant(QVariantList{1, 2})).window == 0,
          "parsePrefs: list yields defaults");
    check(ModelPrefs::parsePrefs(QVariant()).effort == QStringLiteral("xhigh"),
          "parsePrefs: invalid variant yields defaults");
    QVariantMap loose;
    loose.insert(QStringLiteral("thinking"), QStringLiteral("false"));
    loose.insert(QStringLiteral("visible"), QStringLiteral("false"));
    const ModelPrefs::Prefs looseParsed = ModelPrefs::parsePrefs(loose);
    check(looseParsed.thinking,
          "parsePrefs: string \"false\" is NOT bool true->false (strict Bool only)");
    check(looseParsed.visible, "parsePrefs: visible likewise strict");
    QVariantMap strict;
    strict.insert(QStringLiteral("thinking"), false);
    strict.insert(QStringLiteral("visible"), false);
    strict.insert(QStringLiteral("effort"), QStringLiteral("medium"));
    strict.insert(QStringLiteral("window"), 400000);
    const ModelPrefs::Prefs strictParsed = ModelPrefs::parsePrefs(strict);
    check(!strictParsed.thinking && !strictParsed.visible,
          "parsePrefs: real bools honored");
    check(strictParsed.effort == QStringLiteral("medium"), "parsePrefs: effort read");
    check(strictParsed.window == 400000, "parsePrefs: window read");
    QVariantMap dirty;
    dirty.insert(QStringLiteral("effort"), QStringLiteral("ULTRA"));
    dirty.insert(QStringLiteral("window"), 12345);
    const ModelPrefs::Prefs dirtyParsed = ModelPrefs::parsePrefs(dirty);
    check(dirtyParsed.effort == QStringLiteral("xhigh"), "parsePrefs: dirty effort normalized");
    check(dirtyParsed.window == 12345, "parsePrefs: continuous window value kept");

    // ---- ④ 写读往返 ----
    const ModelPrefs::Prefs round = strictParsed;
    const ModelPrefs::Prefs back = ModelPrefs::parsePrefs(ModelPrefs::toVariantMap(round));
    check(back.thinking == round.thinking && back.visible == round.visible,
          "roundtrip: bools survive");
    check(back.effort == round.effort && back.window == round.window,
          "roundtrip: effort and window survive");

    // ---- ⑤ parseAll / allToVariantMap ----
    check(ModelPrefs::parseAll(QVariant(QStringLiteral("x"))).isEmpty(),
          "parseAll: non-map degrades to empty table");
    QVariantMap table;
    table.insert(QStringLiteral("m1"), ModelPrefs::toVariantMap(strictParsed));
    table.insert(QString(), ModelPrefs::toVariantMap(defaults));
    table.insert(QStringLiteral("m2"), QStringLiteral("junk"));
    const QHash<QString, ModelPrefs::Prefs> all = ModelPrefs::parseAll(table);
    check(all.size() == 2, "parseAll: empty key skipped, junk value kept as defaults");
    check(!all.value(QStringLiteral("m1")).thinking, "parseAll: per-model parse applied");
    check(all.value(QStringLiteral("m2")).thinking, "parseAll: junk value -> defaults");
    const QHash<QString, ModelPrefs::Prefs> allBack =
        ModelPrefs::parseAll(ModelPrefs::allToVariantMap(all));
    check(allBack.size() == all.size()
              && allBack.value(QStringLiteral("m1")).window == 400000,
          "allToVariantMap: table roundtrip");

    QVariantMap slashEntry = ModelPrefs::toVariantMap(strictParsed);
    slashEntry.insert(QStringLiteral("model"), QStringLiteral("org/model-a"));
    const QVariantList storedList{QVariant(slashEntry), QVariant(QStringLiteral("junk")),
                                  QVariant(ModelPrefs::toVariantMap(defaults))};
    const QHash<QString, ModelPrefs::Prefs> fromList = ModelPrefs::parseAll(storedList);
    check(fromList.size() == 1 && !fromList.value(QStringLiteral("org/model-a")).thinking,
          "parseAll: list form preserves slash model names and skips junk entries");
    const QVariantList encodedList = ModelPrefs::allToVariantList(fromList);
    check(encodedList.size() == 1
              && encodedList.first().typeId() == QMetaType::QVariantMap,
          "allToVariantList: persists one atomic list entry per model");
    const QHash<QString, ModelPrefs::Prefs> listBack =
        ModelPrefs::parseAll(encodedList);
    check(listBack.value(QStringLiteral("org/model-a")).window == 400000
              && !listBack.value(QStringLiteral("org/model-a")).visible,
          "allToVariantList: list roundtrip keeps window and visibility");

    QHash<QString, QVariant> leaves;
    leaves.insert(QStringLiteral("org/model-a/thinking"), false);
    leaves.insert(QStringLiteral("org/model-a/effort"), QStringLiteral("medium"));
    leaves.insert(QStringLiteral("org/model-a/visible"), false);
    leaves.insert(QStringLiteral("org/model-a/window"), 1000000);
    leaves.insert(QStringLiteral("broken"), 1);
    const QHash<QString, ModelPrefs::Prefs> migrated = ModelPrefs::parseLeafTable(leaves);
    check(migrated.size() == 1
              && !migrated.value(QStringLiteral("org/model-a")).thinking
              && migrated.value(QStringLiteral("org/model-a")).effort == QStringLiteral("medium")
              && migrated.value(QStringLiteral("org/model-a")).window == 1000000,
          "parseLeafTable: legacy flattened object migrates slash model names without truncation");

    // ---- ⑥ visibleModels 保序过滤 ----
    const QStringList models = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    QHash<QString, ModelPrefs::Prefs> vis;
    vis.insert(QStringLiteral("b"), strictParsed); // visible=false
    check(ModelPrefs::visibleModels(models, vis) == QStringList({"a", "c"}),
          "visibleModels: hidden filtered, order preserved");
    check(ModelPrefs::visibleModels(models, QHash<QString, ModelPrefs::Prefs>()) == models,
          "visibleModels: no prefs = all shown");
    QHash<QString, ModelPrefs::Prefs> allHidden;
    allHidden.insert(QStringLiteral("a"), strictParsed);
    allHidden.insert(QStringLiteral("b"), strictParsed);
    allHidden.insert(QStringLiteral("c"), strictParsed);
    check(ModelPrefs::visibleModels(models, allHidden).isEmpty(),
          "visibleModels: all hidden yields empty picker list");

    // ---- ⑦ effectiveWindow 回落 ----
    check(ModelPrefs::effectiveWindow(vis, QStringLiteral("b"), 200000) == 400000,
          "effectiveWindow: explicit stop overrides global");
    check(ModelPrefs::effectiveWindow(vis, QStringLiteral("a"), 200000) == 200000,
          "effectiveWindow: missing prefs fall back to global");
    QHash<QString, ModelPrefs::Prefs> unset;
    unset.insert(QStringLiteral("a"), defaults);
    check(ModelPrefs::effectiveWindow(unset, QStringLiteral("a"), 777000) == 777000,
          "effectiveWindow: window=0 falls back even with a prefs entry");

    // ---- ⑧ thinkingParams 请求体形态 ----
    const QJsonObject on = ModelPrefs::thinkingParams(defaults);
    check(on.value(QStringLiteral("enable_thinking")).toBool() == true,
          "thinkingParams: on sends enable_thinking true");
    check(on.value(QStringLiteral("reasoning_effort")).toString() == QStringLiteral("xhigh"),
          "thinkingParams: on carries the effort");
    const QJsonObject off = ModelPrefs::thinkingParams(strictParsed);
    check(off.value(QStringLiteral("enable_thinking")).toBool() == false,
          "thinkingParams: off sends enable_thinking false");
    check(!off.contains(QStringLiteral("reasoning_effort")),
          "thinkingParams: off omits reasoning_effort (some endpoints 400 on the combo)");

    // ---- ⑨ customModelNames 两形态与去重 ----
    check(ModelPrefs::parseCustomNames(QVariant(QStringLiteral(" a , b ,, a ")))
              == QStringList({"a", "b"}),
          "parseCustomNames: CSV trims, skips empties, dedups in order");
    check(ModelPrefs::parseCustomNames(QVariant(QStringList{"x", "y"}))
              == QStringList({"x", "y"}),
          "parseCustomNames: QStringList form");
    check(ModelPrefs::parseCustomNames(QVariant(QVariantList{"p", "q"}))
              == QStringList({"p", "q"}),
          "parseCustomNames: QVariantList form (json backend)");
    check(ModelPrefs::parseCustomNames(QVariant(42)).isEmpty(),
          "parseCustomNames: scalar number degrades to empty");
    check(ModelPrefs::parseCustomNames(QVariant()).isEmpty(),
          "parseCustomNames: invalid degrades to empty");

    return TestHarness::failCount() - failBefore;
}