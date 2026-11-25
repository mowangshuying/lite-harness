#pragma once

// ============================================================================
// SSE tool_calls 增量累加器（header-only，纯 QtCore：零 Widgets / 零 FluentUI）
//
// 【本头为什么存在】
// 旧接线在 QOpenAi::ChatStream::processFrame 里逐帧合并：每个含 arguments 增量的帧都
// 执行 mergedFn["arguments"] = 旧串 + 增量——operator+ 每帧新分配并整体拷贝已累积的
// 参数串，再写回 QJsonObject（触发 detach）。50KB 的 write_file 参数按 ~20B/帧 ≈ 2500 帧，
// 累计拷贝 O(L²/帧长) ≈ 60MB，全在 GUI 线程，且此期间无 textDelta、UI 本应空闲。
// 本头把 arguments 改为 QString 增量缓冲（append 摊销 O(1)），终局 finalize 一次写回
// JSON；id/type/name 是低频覆写，保持即写。
//
// 【首帧双写修复（登记偏差，非逐字搬移）】
// 旧接线首帧 merged = tc 已含该帧的 function.arguments，同帧的拼接又追加一次——首帧
// 携带非空 arguments 时终局串把首段双写（典型 OpenAI 流式首帧 arguments 为空串故未
// 显形；首帧即发非空增量的网关会拿到坏 JSON 致工具解析失败）。本头首帧只计一次：
// arguments 移入缓冲并从 base 剥除，finalize 一次写回。tst_toolcallacc 内嵌「旧接线
// 参照实现」钉住两种形态：首帧空/缺席族逐字节等价，首帧非空族断言修复。
//
// 【语义保留清单（除上述双写修复外逐字不变）】
// - index < 0 静默跳过；index 空洞补占位，finalize 过滤（旧 finishStream 判据）；
// - 首帧额外字段（含 index 本身）原样保留——history.json 盘上格式不变；
// - id/type/name 仅非空覆写；arguments 键出现即写回（空串也写，旧 contains 判据）；
// - 后续帧 function 非空即确保 function 键存在（即便无 name/arguments，旧写回口径）；
// - arguments 非字符串形态 toString() 降级为空串（旧口径）；
// - reset() 全清（scheduleRetry 重试从零重收语义）。
// ============================================================================

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

namespace ToolCallAcc {

class Accumulator
{
public:
    // 喂入 delta.tool_calls 的一个元素（须带 index）。
    void feed(const QJsonObject &tc);

    // 终局组装：过滤占位、arguments 一次写回；输出顺序 = index 升序。
    QJsonArray finalize() const;

    // 重试复位：清空全部累积态（旧接线 d->toolCalls = QJsonArray() 同口径）。
    void reset();

private:
    struct Slot
    {
        QJsonObject base;      // 首帧原对象（保留 index 等额外字段）+ 后续 id/type/name 覆写
        QString args;          // arguments 增量缓冲（append 摊销 O(1)，替代逐帧全量拼接）
        bool seen = false;     // 是否已有真实帧（占位过滤判据，同旧「空对象被滤」）
        bool argsSeen = false; // arguments 键是否出现过（空串也写回，同旧 contains 判据）
    };
    QVector<Slot> m_slots;     // 按 index 对齐，空洞为占位（seen=false）
};

inline void Accumulator::feed(const QJsonObject &tc)
{
    const int index = tc.value(QStringLiteral("index")).toInt(-1);
    if (index < 0)
        return;
    while (m_slots.size() <= index)
        m_slots.append(Slot());
    Slot &slot = m_slots[index];

    const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();

    if (!slot.seen)
    {
        slot.base = tc;
        slot.seen = true;
        if (fn.contains(QStringLiteral("arguments")))
        {
            // 首帧 arguments 移入缓冲并从 base 剥除：finalize 一次写回。
            // 旧接线在此把首段双写（见头注释「首帧双写修复」）。
            slot.args = fn.value(QStringLiteral("arguments")).toString();
            slot.argsSeen = true;
            QJsonObject stripped = fn;
            stripped.remove(QStringLiteral("arguments"));
            slot.base[QStringLiteral("function")] = stripped;
        }
        return;
    }

    if (!tc.value(QStringLiteral("id")).toString().isEmpty())
        slot.base[QStringLiteral("id")] = tc.value(QStringLiteral("id"));
    if (!tc.value(QStringLiteral("type")).toString().isEmpty())
        slot.base[QStringLiteral("type")] = tc.value(QStringLiteral("type"));
    if (!fn.isEmpty())
    {
        // 与旧接线同口径：function 非空即写回（即便无 name/arguments 也确保键存在）；
        // arguments 走缓冲、不在此写。
        QJsonObject mergedFn = slot.base.value(QStringLiteral("function")).toObject();
        if (!fn.value(QStringLiteral("name")).toString().isEmpty())
            mergedFn[QStringLiteral("name")] = fn.value(QStringLiteral("name"));
        slot.base[QStringLiteral("function")] = mergedFn;
        if (fn.contains(QStringLiteral("arguments")))
        {
            slot.args += fn.value(QStringLiteral("arguments")).toString();
            slot.argsSeen = true;
        }
    }
}

inline QJsonArray Accumulator::finalize() const
{
    QJsonArray out;
    for (const Slot &slot : m_slots)
    {
        if (!slot.seen)
            continue; // 占位：同旧 finishStream「空对象被滤」判据
        QJsonObject obj = slot.base;
        if (slot.argsSeen)
        {
            QJsonObject fnObj = obj.value(QStringLiteral("function")).toObject();
            fnObj[QStringLiteral("arguments")] = slot.args;
            obj[QStringLiteral("function")] = fnObj;
        }
        out.append(obj);
    }
    return out;
}

inline void Accumulator::reset()
{
    m_slots.clear();
}

} // namespace ToolCallAcc
