#pragma once

// 消息 content 形态单源（header-only，仅 QtCore，零 IO）：一条消息的 content 在本仓有两种
// 合法形态——纯串（历史至今）与 OpenAI 兼容的多模态 parts 数组（带图片时才用）。
//
// 抽头动机：全仓有十余处读写 content，其中三处是 `content = content.toString() + text` 的
// **拼接**（AgentLoopBash / AgentLoopTeam / AgentLoopRequest 的 todo reminder）。对 parts 数组
// 求 toString() 得空串再赋值，等于把整段图片连带原文一起抹掉，而 UI 上看不出丢过东西。
// 判据散在调用点必然漂移，故收进本头；这三处改调 appendText。
//
// 形态选择的硬约束：**没有图片时一律保持纯串**。这样零图片会话的 payload 字节、history.json
// 字节、token 计量与压缩判据全与改造前逐字相同（system prompt 静态化与前缀缓存依赖的正是
// 「字节恒定」这条），本批改动对既有会话的影响面为零。
//
// 下游摊平（textOf）为什么必须带标记而不是空串：goal 评估器转录（GoalRules::plainContent）
// 与记忆语料（MemoryRules::messageText）都是只吃文本的消费者，图片本来就读不进去；
// 但读成空串会让「用户发了张图」这件事在评估器眼里彻底不存在，于是模型答非所问时
// 评估器判不出「他其实给了证据」。一条 [image] 是最低成本的诚实。

#include "ImageAttach.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility> // std::as_const

namespace MessageContent {

// parts 里的类型名（协议字段，逐字取 OpenAI 兼容形态，本仓不自造名字）
inline QString textPartType()
{
    return QStringLiteral("text");
}

inline QString imagePartType()
{
    return QStringLiteral("image_url");
}

// 摊平标记：ASCII、与 [tool_use ...] / [tool_result ...] 同族的结构性标记，属**禁翻区**
// （发往 LLM 的 C 类串，见 AGENTS「禁翻区」口径），严禁包 tr()。
inline QString imageMarker()
{
    return QStringLiteral("[image]");
}

// 附件读不到时的降级标记（同上，禁 tr()）。有了它，缺图才是一次「可见的降级」
// 而不是静默少一块内容——与 CompactTranscript「读失败一律保留摘要消息降级」同族取舍。
inline QString imageUnavailableMarker()
{
    return QStringLiteral("[image unavailable]");
}

// 是否已是 parts 数组形态。非数组（含未设值）一律按纯串口径处理。
inline bool isParts(const QJsonValue &content)
{
    return content.isArray();
}

inline QJsonArray partsOf(const QJsonValue &content)
{
    return content.isArray() ? content.toArray() : QJsonArray();
}

inline QString partType(const QJsonValue &part)
{
    return part.toObject().value(QStringLiteral("type")).toString();
}

// 造一个文本 part（组装与降级两路共用，字段形态只在这里写一次）
inline QJsonObject textPart(const QString &text)
{
    return QJsonObject{{QStringLiteral("type"), textPartType()},
                       {QStringLiteral("text"), text}};
}

inline void appendTextPart(QJsonArray &parts, const QString &text)
{
    parts.append(textPart(text));
}

// 计费张数：本地 token 估算的对象（消费点 CompactTokens::messageTokens）。
// **与展开后真正上线的张数同口径**——同名引用在 expandForWire 里只在首现处展开，
// 这里也按名去重；data: 内联的每张各计（无法廉价判重，且内联本就不是本仓产出的形态）。
// 定义顺序：本函数必须先于其使用者 hasImages（inline 自由函数无两阶段查找，同 UpdateRules.h
// 登记过的那条坑）。
inline int imageCount(const QJsonValue &content)
{
    if (!content.isArray())
        return 0;
    QSet<QString> names;
    int inlineCount = 0;
    for (const QJsonValue &part : content.toArray()) {
        if (partType(part) != imagePartType())
            continue;
        const QString url =
            part.toObject().value(QStringLiteral("image_url")).toObject()
                .value(QStringLiteral("url")).toString();
        const QString name = ImageAttach::nameFromUrl(url);
        if (!name.isEmpty())
            names.insert(name);
        else if (ImageAttach::isInlineUrl(url))
            ++inlineCount;
    }
    return static_cast<int>(names.size()) + inlineCount;
}

// 带图判定（各消费口的分流开关）
inline bool hasImages(const QJsonValue &content)
{
    return imageCount(content) > 0;
}

// 摊平为文本：纯串原样返回（**逐字不变**，这是既有全链行为的地基）；数组则按序取文本段，
// 图片段落成标记。段间以单换行相接——只在本仓自造的数组形态里出现，纯串路径不受影响。
inline QString textOf(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    if (!content.isArray())
        return QString();

    QStringList lines;
    for (const QJsonValue &part : content.toArray()) {
        const QString type = partType(part);
        if (type == textPartType()) {
            const QString text = part.toObject().value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                lines << text;
        } else if (type == imagePartType()) {
            lines << imageMarker();
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// 附件裸名清单（按出现序、去重）。用于三处：UI 回放取图、删会话前的目录核对、
// 以及展开时判「这条消息里哪些图必须读得回来」。
inline QStringList imageNames(const QJsonValue &content)
{
    QStringList names;
    for (const QJsonValue &part : partsOf(content)) {
        if (partType(part) != imagePartType())
            continue;
        const QString url =
            part.toObject().value(QStringLiteral("image_url")).toObject()
                .value(QStringLiteral("url")).toString();
        const QString name = ImageAttach::nameFromUrl(url);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    return names;
}

// 组装一条用户消息的 content。**无附件时返回纯串**——落盘形态与改造前逐字节相同。
// 非法附件名（不合形态）直接不进数组：宁可少一张图，也不把 `..` 这类值写进历史，
// 否则下一轮的读取点就成了它的执行器。
inline QJsonValue fromTextAndImages(const QString &text, const QStringList &names)
{
    QJsonArray parts;
    for (const QString &name : names) {
        const QString url = ImageAttach::urlFor(name);
        if (url.isEmpty())
            continue;
        parts.append(QJsonObject{
            {QStringLiteral("type"), imagePartType()},
            {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}}});
    }
    if (parts.isEmpty())
        return QJsonValue(text);

    QJsonArray withText;
    if (!text.trimmed().isEmpty())
        appendTextPart(withText, text);
    for (const QJsonValue &part : parts)
        withText.append(part);
    return QJsonValue(withText);
}

// 只取文本段、**不插图片标记**：UI 呈现专用。
// 与 textOf 的分工是硬的：`[image]` 是喂给 goal 评估器与记忆抽取器的结构性标记（禁翻区
// ASCII），把它画进气泡、或让它进会话标题，等于把一个内部协议串端给了用户。
// UI 这边图片由真缩略图承载，所以标记不仅是错的，还是多余的。
inline QString displayTextOf(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    if (!content.isArray())
        return QString();

    QStringList lines;
    for (const QJsonValue &part : content.toArray()) {
        if (partType(part) != textPartType())
            continue;
        const QString text = part.toObject().value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            lines << text;
    }
    return lines.join(QLatin1Char('\n'));
}

// 追加一段文本（bg 通知、团队事件、todo reminder 三处共用）。
// 纯串保持 `原文 + 入参` 的既有语义（**分隔符由调用方给**，本函数不加戏，否则今日
// 落盘的字节形态会变）；parts 形态追加一个 text part，图片 part 原样留在原位。
inline QJsonValue appendText(const QJsonValue &content, const QString &extra)
{
    if (content.isString())
        return QJsonValue(content.toString() + extra);
    if (!content.isArray())
        return QJsonValue(extra);

    QJsonArray parts = content.toArray();
    appendTextPart(parts, extra);
    return QJsonValue(parts);
}

// 发送末段展开：把 lh-attach:// 引用换成 data URL，其余原样透传。
// 只在真正组请求体时调用（doStartChatRequest 里对 payload 副本做），**绝不写回 m_messages**：
// 一旦把 base64 落回历史，token 计量、压缩闸门、history.json 全被那份假账打穿
// （见 ImageAttach.h 头注释）。
//
// readAttachment 由调用方注入（同 ProjectInstructions::selectExisting 的桩式判据），
// 本头零 IO。返回空字节 = 附件缺失/不可读。
//
// 两条降级：① 附件读不到 → 该 part 换成 image unavailable 标记的文本 part（模型据此知道
// 图丢了，不会答成「你还没给我图」）；② 同名重复的引用只在首现处展开，其余丢弃
// ——载荷按内容哈希命名，同名的两段本来就是同一张图，重复内联等于白付一遍图的钱。
// 无图的 content（含纯串）走快路径原样返回，不做一次无谓的数组拷贝。
inline QJsonValue expandForWire(
    const QJsonValue &content,
    const std::function<QByteArray(const QString &name)> &readAttachment)
{
    if (!content.isArray())
        return content;

    QJsonArray expanded;
    QSet<QString> emitted;
    bool sawImage = false;

    for (const QJsonValue &part : content.toArray()) {
        if (partType(part) != imagePartType()) {
            expanded.append(part);
            continue;
        }
        sawImage = true;
        const QString url =
            part.toObject().value(QStringLiteral("image_url")).toObject()
                .value(QStringLiteral("url")).toString();
        const QString name = ImageAttach::nameFromUrl(url);

        // 非本 scheme 的 url（data: 内联、公网 https）原样交还端点：裁决别人的 url 不归本头。
        if (name.isEmpty() && !url.startsWith(ImageAttach::attachmentScheme())) {
            expanded.append(part);
            continue;
        }

        // 本仓引用：形态被破坏（手改历史）或文件读不回来，一律降级为可见标记，
        // 绝不让 lh-attach:// 漏上线（端点只会回一次坏 url）。
        if (name.isEmpty()) {
            appendTextPart(expanded, imageUnavailableMarker());
            continue;
        }
        if (emitted.contains(name))
            continue; // 同名即同图（载荷按内容哈希命名），首现处已展开
        emitted.insert(name);

        const QByteArray payload = readAttachment(name);
        const QString dataUrl =
            ImageAttach::dataUrlFor(ImageAttach::mimeFor(payload), payload);
        if (dataUrl.isEmpty()) {
            appendTextPart(expanded, imageUnavailableMarker());
            continue;
        }
        expanded.append(QJsonObject{
            {QStringLiteral("type"), imagePartType()},
            {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), dataUrl}}}});
    }

    return sawImage ? QJsonValue(expanded) : content;
}

} // namespace MessageContent
