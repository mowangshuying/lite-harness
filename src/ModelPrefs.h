#pragma once

// 每模型偏好单源（s18.5 模型管理弹窗）：思考开关 / 推理强度 / 显示状态 / 上下文窗口。
// 判据全在本头（QtCore-only，tst_modelprefs 直测）；弹窗 UI 与请求装配只消费，不另写判据。
//
// 落盘形态：config.json 宿主键 app/modelPrefs = [ {"model":"<模型名>",thinking,effort,visible,window}, … ]
// （QVariantList 原子值，避免 JSON 后端把对象套对象拍平成叶子键）；app/customModelNames
// = [ "<模型名>", … ] 记录自定义窗口清单；providers 表结构不动，添加/拉取/移除/清空时两账同清。
// 旧版对象形态仍可读，下一次保存自动迁移为列表。
//
// 缺省即现状：thinking=true / effort=xhigh / visible=true / window=0（未设=回落全局
// contextCharLimit）。未写偏好的模型与改造前逐字节同行为，红线同「零技能零记忆不产生注入块」。

#include "AppSettings.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <algorithm>

namespace ModelPrefs {

inline QString prefsKey()
{
    return QStringLiteral("modelPrefs");
}

inline QString customNamesKey()
{
    return QStringLiteral("customModelNames");
}

// ---- 推理强度四档：wire 值序即 UI 下拉序（低→极高） ----
// 默认 xhigh 承原 AgentConst::kReasoningEffort（对齐 opencode 长思考观感）：
// 未设偏好的模型请求体与改造前逐字节一致。
inline QStringList effortWireValues()
{
    return {QStringLiteral("low"), QStringLiteral("medium"),
            QStringLiteral("high"), QStringLiteral("xhigh")};
}

inline QString defaultEffort()
{
    return QStringLiteral("xhigh");
}

inline QString normalizeEffort(const QString &value)
{
    return effortWireValues().contains(value) ? value : defaultEffort();
}

inline int effortIndex(const QString &value)
{
    return effortWireValues().indexOf(normalizeEffort(value));
}

// ---- 上下文窗口（ FluLineEdit 直接录入 token 数，0~1M 连续钳位） ----
// 与全局键同语义：声明值即窗口（s17.9 口径，不换算）。0 = 未设，回落全局。
inline int windowMax()
{
    return 1000000;
}

inline int normalizeWindow(int value)
{
    if (value < 0)
        return 0;
    if (value > windowMax())
        return windowMax();
    return value;
}

// 输入框文本 → 窗口值：空/全空白 = 0（未设）；纯整数钳位到 [0, 1M]；
// 非法文本返回 -1，由调用方回退显示，绝不把垃圾值写进偏好。
inline int windowFromText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return 0;
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (!ok)
        return -1;
    return normalizeWindow(value);
}

// ---- 单模型偏好 ----

struct Prefs
{
    bool thinking = true;
    QString effort = defaultEffort();
    bool visible = true;
    int window = 0; // 0 = 未设，回落全局 contextCharLimit
};

// 容错解析：非对象一律默认；布尔只认严格 Bool（QVariant::toBool 对串/数字宽松，
// 手改 "false" 串会被读成 true，与 providers 严格布尔同纪律）；effort/window 归一。
inline Prefs parsePrefs(const QVariant &value)
{
    Prefs prefs;
    if (value.typeId() != QMetaType::QVariantMap)
        return prefs;
    const QVariantMap map = value.toMap();
    const QVariant thinking = map.value(QStringLiteral("thinking"));
    if (thinking.typeId() == QMetaType::Bool)
        prefs.thinking = thinking.toBool();
    const QVariant visible = map.value(QStringLiteral("visible"));
    if (visible.typeId() == QMetaType::Bool)
        prefs.visible = visible.toBool();
    prefs.effort = normalizeEffort(map.value(QStringLiteral("effort")).toString());
    prefs.window = normalizeWindow(map.value(QStringLiteral("window")).toInt());
    return prefs;
}

inline QVariantMap toVariantMap(const Prefs &prefs)
{
    QVariantMap map;
    map.insert(QStringLiteral("thinking"), prefs.thinking);
    map.insert(QStringLiteral("effort"), prefs.effort);
    map.insert(QStringLiteral("visible"), prefs.visible);
    map.insert(QStringLiteral("window"), prefs.window);
    return map;
}

// 整表解析：当前列表形态与旧对象形态都认；空模型键跳过，脏条目按默认偏好保留。
inline QHash<QString, Prefs> parseAll(const QVariant &value)
{
    QHash<QString, Prefs> out;
    if (value.typeId() == QMetaType::QVariantList) {
        const QVariantList list = value.toList();
        for (const QVariant &item : list) {
            if (item.typeId() != QMetaType::QVariantMap)
                continue;
            const QVariantMap map = item.toMap();
            const QString model = map.value(QStringLiteral("model")).toString();
            if (!model.isEmpty())
                out.insert(model, parsePrefs(map));
        }
        return out;
    }
    if (value.typeId() != QMetaType::QVariantMap)
        return out;
    const QVariantMap map = value.toMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().isEmpty())
            continue;
        out.insert(it.key(), parsePrefs(it.value()));
    }
    return out;
}

inline QVariantMap allToVariantMap(const QHash<QString, Prefs> &all)
{
    QVariantMap map;
    for (auto it = all.cbegin(); it != all.cend(); ++it)
        map.insert(it.key(), toVariantMap(it.value()));
    return map;
}

inline QVariantList allToVariantList(const QHash<QString, Prefs> &all)
{
    QStringList models = all.keys();
    std::sort(models.begin(), models.end());
    QVariantList list;
    for (const QString &model : models) {
        QVariantMap entry = toVariantMap(all.value(model));
        entry.insert(QStringLiteral("model"), model);
        list.append(entry);
    }
    return list;
}

// 旧 JSON 对象被后端拍平成 "<model>/<field>" 叶子；最后一段是字段，其余原样拼回模型名，
// 因而 OpenRouter 这类含 "/" 的模型名也不会在迁移时被截断。
inline QHash<QString, Prefs> parseLeafTable(const QHash<QString, QVariant> &leaves)
{
    QVariantMap legacy;
    for (auto it = leaves.cbegin(); it != leaves.cend(); ++it) {
        const int slash = it.key().lastIndexOf(QLatin1Char('/'));
        if (slash <= 0)
            continue;
        QVariantMap entry = legacy.value(it.key().left(slash)).toMap();
        entry.insert(it.key().mid(slash + 1), it.value());
        legacy.insert(it.key().left(slash), entry);
    }
    return parseAll(legacy);
}

// ---- 派生判据（纯入参，零配置依赖） ----

// 显示状态过滤：保序；无偏好 = 显示。隐藏只影响下拉候选，不影响已选会话的请求模型。
inline QStringList visibleModels(const QStringList &all, const QHash<QString, Prefs> &prefs)
{
    QStringList out;
    out.reserve(all.size());
    for (const QString &model : all) {
        const auto it = prefs.constFind(model);
        if (it == prefs.constEnd() || it->visible)
            out.append(model);
    }
    return out;
}

inline int effectiveWindow(const QHash<QString, Prefs> &prefs, const QString &model,
                           int globalFallback)
{
    const int window = prefs.value(model).window;
    return window > 0 ? window : globalFallback;
}

// 请求体思考参数单源：开 = enable_thinking + reasoning_effort；关 = 仅 enable_thinking:false
// （关时不下发 effort——部分端点对「关思考+强度」组合直接 400）。
inline QJsonObject thinkingParams(const Prefs &prefs)
{
    QJsonObject object;
    object[QStringLiteral("enable_thinking")] = prefs.thinking;
    if (prefs.thinking)
        object[QStringLiteral("reasoning_effort")] = prefs.effort;
    return object;
}

// ---- 自定义窗口清单（customModelNames） ----
// 清单键两形态都认（ini 裸逗号串 / json 数组落 QVariantList，同 modelOptions 老坑）；
// trim、跳空、保序去重。
inline QStringList parseCustomNames(const QVariant &value)
{
    QStringList raw;
    if (value.typeId() == QMetaType::QString)
        raw = value.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    else if (value.typeId() == QMetaType::QStringList || value.typeId() == QMetaType::QVariantList)
        raw = value.toStringList();
    QStringList out;
    for (const QString &name : raw) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !out.contains(trimmed))
            out.append(trimmed);
    }
    return out;
}

// ---- 配置现取便捷口（与 AgentConst 同纪律：要求 QCoreApplication 实例，测试只测纯入参口） ----

inline QHash<QString, Prefs> allPrefs()
{
    const QVariant stored = AppSettings::value(prefsKey());
    if (stored.isValid())
        return parseAll(stored);
    return parseLeafTable(AppSettings::valuesWithPrefix(prefsKey()));
}

inline Prefs prefsFor(const QString &model)
{
    return allPrefs().value(model);
}

inline QJsonObject thinkingParamsFor(const QString &model)
{
    return thinkingParams(prefsFor(model));
}

inline QStringList customNames()
{
    return parseCustomNames(AppSettings::value(customNamesKey()));
}

inline bool isCustom(const QString &model)
{
    return customNames().contains(model);
}

} // namespace ModelPrefs