#pragma once

// 记忆子系统的纯逻辑单源（header-only、纯 QtCore，纪律同 CompactTokens.h / ActivityLedger.h /
// HeatmapGrid.h）：frontmatter 解析、JSON 数组抽取、记录校验、去重门槛、slug 归一、YAML 标量。
//
// 为什么单独成头：这些判据此前全在 `MemoryManager.cpp`，而该 TU include QOpenAi.h
// （Qt6::Network），按 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest，
// 于是**整套判据零直测**。它们恰是「算得出但算错」的高危面：`extractJsonArray` 是手写括号
// 配对状态机，`escaped`/`depth` 错一格就返回空数组，LLM 已正确返回的记忆条目被静默丢弃，
// 用户只看到「这次没沉淀出新记忆」，无报错无日志；`memorySlug` 错则两个不同 name 撞同一
// 文件名、后写静默覆盖前一条；`normalizedMemoryText` 错则去重键失效、重复记忆无限增长。
//
// `MemoryManager` 侧保留同名 static 成员做**一行转发**，调用点与头文件签名均不变。
// helper 一律放具名 namespace 且 `inline`（不放匿名 ns）：`memoryTypes()` 与
// `collapseWhitespace()` 还被本头之外的 MemoryManager 代码调用，匿名 ns 会让它们够不着；
// 而 header-only 的匿名 ns 会让每个 TU 各持一份、函数内 static 身份分裂，都是坑。

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace MemoryRules {

// lcc MEMORY_TYPES（memory_manager.py :9）
inline const QStringList &memoryTypes()
{
    static const QStringList list = {
        QStringLiteral("user"),
        QStringLiteral("feedback"),
        QStringLiteral("project"),
        QStringLiteral("reference"),
    };
    return list;
}

// lcc TEMPORARY_MEMORY_MARKERS（:10-27）——16 条，逐字移植（状态文件写 18 为笔误，以 lcc 源码为准）
inline const QStringList &temporaryMemoryMarkers()
{
    static const QStringList list = {
        QStringLiteral("this session"),
        QStringLiteral("current session"),
        QStringLiteral("this turn"),
        QStringLiteral("current turn"),
        QStringLiteral("this task"),
        QStringLiteral("current task"),
        QStringLiteral("for now"),
        QStringLiteral("just this time"),
        QStringLiteral("today only"),
        QStringLiteral("本次会话"),
        QStringLiteral("当前会话"),
        QStringLiteral("这一轮"),
        QStringLiteral("当前轮次"),
        QStringLiteral("本次任务"),
        QStringLiteral("当前任务"),
        QStringLiteral("暂时"),
    };
    return list;
}

// Python str.split() 的任意空白切分近似（UCP 使 \s 覆盖 unicode 空白；
// 字符计数为 UTF-16 单元 vs Python 码点——天文字符微差，登记偏差，与 s08 同源）
inline const QRegularExpression &whitespaceRun()
{
    static const QRegularExpression re(QStringLiteral("\\s+"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

inline QString collapseWhitespace(const QString &value)
{
    return QString(value).split(whitespaceRun(), Qt::SkipEmptyParts).join(QLatin1Char(' '));
}

// memory_slug 的 [^\w]+ → "-" 折叠（Python \w 为 unicode 语义 → UseUnicodePropertiesOption）
inline QString slugCollapse(QString lowered)
{
    static const QRegularExpression nonWord(
        QStringLiteral("[^\\w]+"), QRegularExpression::UseUnicodePropertiesOption);
    lowered.replace(nonWord, QLatin1String("-"));
    return lowered;
}

inline QString trimDashes(const QString &value)
{
    qsizetype begin = 0;
    qsizetype end = value.size();
    while (begin < end && (value.at(begin) == QLatin1Char('-') || value.at(begin) == QLatin1Char('_')))
        ++begin;
    while (end > begin && (value.at(end - 1) == QLatin1Char('-') || value.at(end - 1) == QLatin1Char('_')))
        --end;
    return value.mid(begin, end - begin);
}

// 极简 YAML 标量序列化（与 PyYAML 的偏差：拿不准时统一输出 JSON 双引号风格——合法的 YAML
// 双引号标量，读回等价；PyYAML 默认可能用 plain/literal 风格，仅磁盘格式差异）
inline QString yamlScalar(const QString &value)
{
    static const QString specials = QStringLiteral(":#{[]},&*!|>'\"%@`");
    bool needQuote = value.isEmpty() || value != value.trimmed();
    if (!needQuote)
    {
        for (const QChar &c : value)
        {
            if (specials.contains(c) || c == QLatin1Char('\n'))
            {
                needQuote = true;
                break;
            }
        }
    }
    if (!needQuote &&
        (value.startsWith(QLatin1Char('-')) || value.startsWith(QLatin1Char('?')) ||
         value.startsWith(QLatin1Char(':'))))
        needQuote = true; // 序列/映射指示符开头（PyYAML 仅在跟随空白时加引号，从严无害）
    if (!needQuote)
    {
        const QString lower = value.toLower();
        if (lower == QLatin1String("true") || lower == QLatin1String("false") ||
            lower == QLatin1String("null") || lower == QLatin1String("~") ||
            lower == QLatin1String("yes") || lower == QLatin1String("no") ||
            lower == QLatin1String("on") || lower == QLatin1String("off"))
            needQuote = true; // 会被 YAML 解析成布尔/空值的保留词
    }
    if (!needQuote)
        return value;
    // JSON 字符串字面量即合法 YAML 双引号标量：借 QJsonArray 包裹技巧取得带转义的引号形式
    const QByteArray json =
        QJsonDocument(QJsonArray::fromStringList({ value })).toJson(QJsonDocument::Compact).trimmed();
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

// 去重比对所需的最小字段视图。刻意不依赖 `MemoryManager::MemoryRecord`——那是嵌套在
// MemoryManager.h 里的类型，而该头链 QOpenAi（Qt6::Network），纯头反向依赖它就把整个
// GUI/网络依赖拖回来了，抽头也就失去意义。映射在 MemoryManager 转发处一次完成。
struct ExistingMemory {
    QString name;
    QString description;
    QString body;
};

inline bool parseFrontmatter(const QString &text, QJsonObject *metadata, QString *body)
{
    // lcc parse_frontmatter（:35-47）：无 "---\n" 开头 / 缺闭合 "---" / YAML 解析失败 /
    // 非映射 → 一律回落 ({}, 原文)。lite 无 PyYAML：仅支持单行 "key: value" 平铺标量
    // （极简解析，登记偏差）——缩进行、缺冒号行等 PyYAML 会报错的输入，此处同样整体回落。
    // 失败路径 body 预置为全文（≈ lcc text.strip()，Gate4 MINOR-1），成功路径下方覆盖；
    // metadata 出参由调用方默认空对象承载 ({}) 回落语义。
    *body = text.trimmed();
    if (!text.startsWith(QLatin1String("---\n")))
        return false;
    const qsizetype closing = text.indexOf(QLatin1String("---"), 3);
    if (closing < 0)
        return false;

    QJsonObject meta;
    const QStringList lines = text.mid(3, closing - 3).split(QLatin1Char('\n'));
    for (const QString &rawLine : lines)
    {
        const QString line = rawLine.endsWith(QLatin1Char('\r')) ? rawLine.left(rawLine.size() - 1) : rawLine;
        if (line.trimmed().isEmpty())
            continue;
        if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t')))
            return false; // 缩进（嵌套映射/续行）不支持 → ≈ PyYAML 报错回落
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            return false; // 缺冒号或空键
        const QString key = line.left(colon).trimmed();
        if (key.isEmpty())
            return false;
        QString value = line.mid(colon + 1).trimmed();
        if (value.size() >= 2 && value.at(0) == value.at(value.size() - 1) &&
            (value.at(0) == QLatin1Char('"') || value.at(0) == QLatin1Char('\'')))
        {
            if (value.at(0) == QLatin1Char('"'))
            {
                // 双引号标量按 JSON 转义规则还原（PyYAML 双引号风格对常用标量与 JSON 兼容）
                const QJsonDocument unescaped =
                    QJsonDocument::fromJson(QByteArray("[") + value.toUtf8() + "]");
                if (!unescaped.isArray() || !unescaped.array().at(0).isString())
                    return false; // 整体不可解析才回落；注意 Qt 对非法转义宽容（见下条实测偏差）
                // 实测偏差（tst_memoryrules 钉死）：Qt 不像 PyYAML 那样对非法转义报错——
                // `["a\q"]` 照样解析成功、反斜杠被静默吞掉得到 `aq`。故「非法转义 ≈ PyYAML 报错
                // 回落」这句此前是**不成立**的（写侧 yamlScalar 只产出合法 JSON 转义，正常往返不受
                // 影响；受影响的是手改的记忆文件：读回的是被改形的文本，不是整体回落）。
                value = unescaped.array().at(0).toString();
            }
            else
            {
                // 单引号标量：'' 为引号转义，反斜杠保持字面
                value = value.mid(1, value.size() - 2);
                value.replace(QLatin1String("''"), QLatin1String("'"));
            }
        }
        // 平铺标量一律按字符串保留（PyYAML 会还原数字/布尔类型；消费点均做 str()/文本比较，
        // 效果等价——类型转换族差异登记于文件头偏差）
        meta.insert(key, value);
    }

    QString remainder = text.mid(closing + 3);
    qsizetype begin = 0;
    while (begin < remainder.size() && remainder.at(begin).isSpace())
        ++begin;
    remainder.remove(0, begin); // ≈ parts[2].lstrip()
    *metadata = meta;
    *body = remainder;
    return true;
}

inline QString memorySlug(const QString &name)
{
    const QString slug = trimDashes(slugCollapse(name.toLower()));
    return slug.isEmpty() ? QStringLiteral("memory") : slug;
}

inline QString normalizedMemoryText(const QString &value)
{
    // lcc " ".join(value.lower().split())
    return collapseWhitespace(value.toLower());
}

inline QString messageText(const QJsonObject &message)
{
    // OpenAI 形态 content 为平铺字符串。lcc（Anthropic 形态）只提取 text 块——tool_result/
    // tool_use 块均贡献空文本而被过滤；我们的工具结果是独立 role=="tool" 消息，
    // 等价跳过（登记偏差：使 dialogue/recent 文本与 lcc 对齐）
    if (message.value(QStringLiteral("role")) == QStringLiteral("tool"))
        return QString();
    return message.value(QStringLiteral("content")).toString();
}

inline QJsonArray extractJsonArray(const QString &text)
{
    // lcc extract_json_array（:233-246）：对每个 '[' 尝试 raw_decode，第一个整体可解析的
    // JSON 数组即返回。Qt 无 raw_decode：以括号配对（含字符串/转义识别）截出候选子串等价近似
    for (qsizetype pos = 0; pos < text.size(); ++pos)
    {
        if (text.at(pos) != QLatin1Char('['))
            continue;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        qsizetype end = -1;
        for (qsizetype i = pos; i < text.size(); ++i)
        {
            const QChar c = text.at(i);
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (c == QLatin1Char('\\'))
                    escaped = true;
                else if (c == QLatin1Char('"'))
                    inString = false;
                continue;
            }
            if (c == QLatin1Char('"'))
                inString = true;
            else if (c == QLatin1Char('['))
                ++depth;
            else if (c == QLatin1Char(']'))
            {
                --depth;
                if (depth == 0)
                {
                    end = i;
                    break;
                }
            }
        }
        if (end < 0)
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(text.mid(pos, end - pos + 1).toUtf8());
        if (doc.isArray())
            return doc.array();
    }
    return QJsonArray();
}

inline bool validateMemoryRecord(const QJsonObject &record, bool requireScope,
                                         QJsonObject *out)
{
    // lcc validate_memory_record（:369-395）；lcc 的 dict 判型由调用方 isObject 过滤等价承担
    const QString name = record.value(QStringLiteral("name")).toVariant().toString().trimmed();
    const QString type = record.value(QStringLiteral("type")).toVariant().toString().trimmed();
    const QString description =
        record.value(QStringLiteral("description")).toVariant().toString().trimmed();
    const QString body = record.value(QStringLiteral("body")).toVariant().toString().trimmed();
    const QString scope = record.value(QStringLiteral("scope")).toVariant().toString().trimmed();

    if (name.isEmpty() || !memoryTypes().contains(type) || description.isEmpty() || body.isEmpty())
        return false;
    if (requireScope && scope != QLatin1String("persistent") && scope != QLatin1String("current_task"))
        return false;

    QJsonObject validated;
    validated[QStringLiteral("name")] = name;
    validated[QStringLiteral("type")] = type;
    validated[QStringLiteral("description")] = description;
    validated[QStringLiteral("body")] = body;
    if (!scope.isEmpty())
        validated[QStringLiteral("scope")] = scope;
    *out = validated;
    return true;
}

inline bool shouldStoreMemory(const QJsonObject &candidate,
                                      const QVector<ExistingMemory> &existing)
{
    // lcc should_store_memory（:78-108）：持久门槛 + 16 条临时标记 + slug/描述/正文三重去重
    if (candidate.value(QStringLiteral("scope")).toString() != QLatin1String("persistent"))
        return false;
    if (!memoryTypes().contains(candidate.value(QStringLiteral("type")).toString()))
        return false;

    const QString name = candidate.value(QStringLiteral("name")).toString().trimmed();
    const QString description = candidate.value(QStringLiteral("description")).toString().trimmed();
    const QString body = candidate.value(QStringLiteral("body")).toString().trimmed();
    if (name.isEmpty() || description.isEmpty() || body.isEmpty())
        return false;

    const QString candidateText =
        normalizedMemoryText(name + QLatin1Char('\n') + description + QLatin1Char('\n') + body);
    for (const QString &marker : temporaryMemoryMarkers())
    {
        if (candidateText.contains(marker))
            return false;
    }

    const QString slug = memorySlug(name);
    const QString normalizedDescription = normalizedMemoryText(description);
    const QString normalizedBody = normalizedMemoryText(body);
    for (const ExistingMemory &memory : existing)
    {
        if (memorySlug(memory.name) == slug)
            return false;
        if (normalizedMemoryText(memory.description) == normalizedDescription)
            return false;
        if (normalizedMemoryText(memory.body) == normalizedBody)
            return false;
    }
    return true;
}

inline QString memoryDocument(const QString &name, const QString &type,
                                      const QString &description, const QString &body)
{
    // lcc memory_document（:110-116）：键序固定 name/description/type（safe_dump sort_keys=False）
    const QString metadata = QStringLiteral("name: %1\ndescription: %2\ntype: %3")
                                 .arg(yamlScalar(name), yamlScalar(description), yamlScalar(type));
    return QStringLiteral("---\n%1\n---\n\n%2\n").arg(metadata, body.trimmed());
}

} // namespace MemoryRules
