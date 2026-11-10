#pragma once

// 会话键算术单源（header-only，只依赖 QtCore + 纯字面量 NavKeys.h，纪律同 ToolSchema.h /
// MemoryRules.h / HeatmapGrid.h）：把「新建会话键」与「恢复会话键」的拼装、归属判定、
// 撞车条件从 SessionRegistry 里抽出来直测。
//
// 为什么单独成头：`SessionRegistry.cpp` include `<QWidget>` 与 `ChatSessionPage.h`（硬绑
// Qt6::Widgets + FluentUI），按「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不了 ctest，
// 于是键分配**零直测**。而键一旦撞车后果是不可逆的数据删除：`insert()` 用
// `QHash::operator[]` 就地覆盖，两条会话共用一个 key 后，`dataId(key)` 只指向存活那条，
// 右键删除兄弟会话时会连带把**另一个会话的 `sessions/<dataId>` 整目录**（历史/任务/记忆）
// 递归抹掉，`unregister()` 再按值把两个兄弟的全部受控控件登记一次抹净。
//
// 关于「两条键空间不相交」的真实理由（本头据此下断言，勿照旧注释理解）：
// `SessionRegistry.h` 与 `LiteHarness.cpp` 的注释原写「hex 永不等于十进制自增，避免键冲突」，
// **这个理由是错的**——十进制数字串是十六进制的子集，`"12345678"` 既是合法 8 位 hex 也是
// 合法十进制 seq。两条键空间真正不相交靠的是**长度**：新建会话的 dataId 恒为 8 位小写 hex
// （`QUuid::toRfc4122().toHex().left(8)`），而自增号要写到 8 位得先跑到 10^7，进程内不可能。
// 也就是说这条保证是**数量级上的巧合，不是字母表上的不变量**：手改 config.json 或旧
// index.json 迁移进来的短 dataId（如 "1"）会与新建会话的第一个键直接撞车，而 createSession
// 路径上没有任何 contains() 卫兵（只有 restoreSessions 有）。下面的撞车谓词就是把这个口子
// 显式化，供调用方按需加守卫；本头只算键、不改行为。

#include "NavKeys.h"

#include <QString>

namespace SessionKeys {

// 新建会话键：`Session_` + 进程内自增号（十进制、无前导零）。
// 自增号由调用方持有（SessionRegistry::m_sessionSeq，先 ++ 再取），本头不持状态。
inline QString keyForSeq(int seq)
{
    return QString(NavKey::SessionKeyFmt).arg(seq);
}

// 恢复会话键：`Session_` + dataId **原文**（不清洗、不改大小写、不截断——键必须与磁盘上的
// `sessions/<dataId>` 目录名逐字一致，否则删会话时会删到别的目录去）。
inline QString keyForDataId(const QString &dataId)
{
    return QString(NavKey::SessionPrefix) + dataId;
}

// 是否会话键（两族共用前缀，故只能这样判）。非会话页键（NewChatPage/SettingsPage/
// SessionsGroup）不得命中。
inline bool isSessionKey(const QString &key)
{
    return key.startsWith(QLatin1String(NavKey::SessionPrefix));
}

// 取会话键的自增号：仅当尾段是**纯十进制且无前导零**时才返回该数，否则返回 -1。
// 「无前导零」是 keyForSeq 的产物形态（QString::arg(int) 不产生前导零），据此把
// `Session_00000001` 这类 dataId 键判回 dataId 族，不让它冒充 seq 族。
inline int seqFromKey(const QString &key)
{
    const QLatin1String prefix(NavKey::SessionPrefix);
    if (!key.startsWith(prefix))
        return -1;
    const QString tail = key.mid(prefix.size());
    if (tail.isEmpty())
        return -1;
    for (qsizetype i = 0; i < tail.size(); ++i)
    {
        const QChar c = tail.at(i);
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return -1;
    }
    if (tail.size() > 1 && tail.at(0) == QLatin1Char('0'))
        return -1; // 前导零 → 不是 keyForSeq 的产物
    return tail.toInt();
}

// 是否「新建会话生成的 dataId」形态：恰好 8 位小写十六进制
// （`QUuid::createUuid().toRfc4122().toHex().left(8)` 的产物形状）。
inline bool isGeneratedDataId(const QString &dataId)
{
    if (dataId.size() != 8)
        return false;
    for (const QChar &c : dataId)
    {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                         (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        if (!hex)
            return false;
    }
    return true;
}

// 撞车谓词：某 dataId 的恢复键是否等于第 seq 号新建会话键。
// 真实生成的 dataId（8 位 hex）在 seq < 10^7 时恒为 false——靠的是长度而非字母表；
// 短 dataId（手改配置/旧迁移）则可能直接为 true。调用方据此决定要不要加卫兵。
inline bool collidesWithSeqKey(const QString &dataId, int seq)
{
    return keyForDataId(dataId) == keyForSeq(seq);
}

} // namespace SessionKeys
