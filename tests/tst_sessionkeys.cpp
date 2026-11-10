// tst_sessionkeys.cpp —— 会话键算术纯头直测（src/SessionKeys.h，header-only 零 CMake 改动即入
// ctest，仿 tst_memoryrules / tst_toolschema / tst_heatmapgrid 先例）。
//
// 抽头动机：键分配此前长在 `SessionRegistry.cpp`，该 TU include `<QWidget>` 与
// `ChatSessionPage.h`（硬绑 Qt6::Widgets + FluentUI），按「仅 QtCore-only 引擎进测试清单」的
// 硬纪律进不了 ctest，于是**键分配零直测**。而键撞车的后果是不可逆的数据删除：`insert()` 用
// `QHash::operator[]` 就地覆盖，两条会话共用一个 key 后 `dataId(key)` 只指向存活那条，右键删除
// 兄弟会话时会连带把**另一个会话的 `sessions/<dataId>` 整目录**（历史/任务/记忆）递归抹掉。
//
// 本套件同时**证伪一条注释不变量**：`SessionRegistry.h` 与 `LiteHarness.cpp` 的注释原写
// 「hex 永不等于十进制自增，避免键冲突」——这个理由是错的。十进制数字串是十六进制的子集，
// `"12345678"` 既是合法 8 位 hex 也是合法十进制 seq，两者能在同一个键字符串上相遇。两条键空间
// 实际不相交靠的是**长度**：新建会话的 dataId 恒 8 位，而自增号要写到 8 位得先跑到 10^7。
// 也就是说这是数量级上的巧合，不是字母表上的保证——手改 config.json 或旧 index.json 迁移进来的
// 短 dataId（如 "1"）会与新建会话的第一个键直接撞车。下面把「真实保证」与「已证伪的理由」都钉住。
//
// 未覆盖面（诚实登记）：`insert` 覆盖、`unregister` 按值全表扫荡这两条**后果侧**逻辑仍不可直测
// ——它们是 QHash 行为、绑在 SessionRegistry 的成员上，而该 TU 链 Qt6::Widgets。本头只算键。
//
// 分组：全部零夹具恒跑（纯字符串算术，不碰磁盘、不建控件）。

#include "TestHarness.h"

#include "SessionKeys.h"

#include "NavKeys.h"

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

namespace {

// ---- 组 1：新建会话键的形态（前缀单源 + 十进制自增 + 无前导零）----

void testSeqKeyShape()
{
    const QString k1 = SessionKeys::keyForSeq(1);
    TestHarness::check(k1 == QStringLiteral("Session_1"), "seqkey: 第 1 号键逐字为 Session_1");
    TestHarness::check(k1.startsWith(QLatin1String(NavKey::SessionPrefix)),
                       "seqkey: 键保持 NavKey::SessionPrefix 前缀");
    TestHarness::check(SessionKeys::keyForSeq(42) == QStringLiteral("Session_42"),
                       "seqkey: 42 号键逐字为 Session_42");
    TestHarness::check(SessionKeys::keyForSeq(1000000) == QStringLiteral("Session_1000000"),
                       "seqkey: 七位自增号不截断不分隔");

    // arg(int) 不产生前导零、不分组：一旦 Qt 行为变了，seqFromKey 的「无前导零」判据会
    // 与产物形态分叉，恢复侧的归属判定整体错位
    TestHarness::check(SessionKeys::keyForSeq(7) == QStringLiteral("Session_7"),
                       "seqkey: 个位数键不补前导零");
    TestHarness::check(SessionKeys::seqFromKey(SessionKeys::keyForSeq(7)) == 7,
                       "seqkey: 无前导零产物可被 seqFromKey 认回本族");
    // 单调且互不相同（同进程内自增号不复用，故新建会话永不撞自己）
    QSet<QString> keys;
    for (int i = 1; i <= 500; ++i)
        keys.insert(SessionKeys::keyForSeq(i));
    TestHarness::check(keys.size() == 500, "seqkey: 1..500 号键两两不同");
}

// ---- 组 2：恢复会话键必须逐字拼接（键与磁盘目录名同源，清洗一次就删错目录）----

void testDataIdKeyIsVerbatim()
{
    TestHarness::check(SessionKeys::keyForDataId(QStringLiteral("deadbeef")) ==
                           QStringLiteral("Session_deadbeef"),
                       "dataidkey: 8 位 hex 逐字拼接");
    // 不改大小写：同一 dataId 的大写形态必须产出不同键（文件系统大小写不敏感是另一层，
    // 但键本身不得静默归一，否则两条不同条目会合并成一条）
    TestHarness::check(SessionKeys::keyForDataId(QStringLiteral("DEADBEEF")) !=
                           SessionKeys::keyForDataId(QStringLiteral("deadbeef")),
                       "dataidkey: 大小写不同则键不同（不静默归一）");
    // 不清洗空白/分隔符：手改配置里的脏 dataId 必须原样反映在键上，好让上层守卫看得见
    TestHarness::check(SessionKeys::keyForDataId(QStringLiteral(" ab ")) ==
                           QStringLiteral("Session_ ab "),
                       "dataidkey: 前后空白不被吞掉");
    // 空 dataId 的退化形态：键恰好等于前缀本身。restoreSessions 上游已判空，
    // 这里钉住形态，避免将来有人以为 keyForDataId("") 会得到一个安全值
    TestHarness::check(SessionKeys::keyForDataId(QString()) ==
                           QString(QLatin1String(NavKey::SessionPrefix)),
                       "dataidkey: 空 dataId 退化为裸前缀（上游必须自己判空）");
}

// ---- 组 3：两族键共用前缀，非会话页键不得命中 ----

void testIsSessionKey()
{
    TestHarness::check(SessionKeys::isSessionKey(SessionKeys::keyForSeq(3)),
                       "classify: 新建会话键被识别为会话键");
    TestHarness::check(SessionKeys::isSessionKey(SessionKeys::keyForDataId(QStringLiteral("a1b2c3d4"))),
                       "classify: 恢复会话键被识别为会话键");
    // 三个非会话页键若被误判为会话键，导航联动会把它们当会话去查注册表
    TestHarness::check(!SessionKeys::isSessionKey(QLatin1String(NavKey::NewChatPage)),
                       "classify: NewChatPage 不是会话键");
    TestHarness::check(!SessionKeys::isSessionKey(QLatin1String(NavKey::SettingsPage)),
                       "classify: SettingsPage 不是会话键");
    TestHarness::check(!SessionKeys::isSessionKey(QLatin1String(NavKey::SessionsGroup)),
                       "classify: SessionsGroup 不是会话键");
    TestHarness::check(!SessionKeys::isSessionKey(QString()), "classify: 空串不是会话键");
}

// ---- 组 4：归属反解（seqFromKey 只认自己产出的形态）----

void testSeqRoundtrip()
{
    for (int seq : {1, 2, 9, 10, 42, 999, 1000, 123456})
    {
        const QString key = SessionKeys::keyForSeq(seq);
        TestHarness::check(SessionKeys::seqFromKey(key) == seq,
                           QString("roundtrip: %1 反解回 %2").arg(key).arg(seq).toUtf8().constData());
    }
    // hex dataId 键不得被反解成 seq（含纯数字 8 位的情形，见组 5 的撞车条件）
    TestHarness::check(SessionKeys::seqFromKey(SessionKeys::keyForDataId(QStringLiteral("deadbeef"))) == -1,
                       "roundtrip: 含字母的 hex 键反解为 -1");
    // 前导零 dataId 键必须判回 dataId 族：Session_00000001 不是 keyForSeq 的产物
    TestHarness::check(SessionKeys::seqFromKey(SessionKeys::keyForDataId(QStringLiteral("00000001"))) == -1,
                       "roundtrip: 前导零键不被冒充成 seq 族");
    TestHarness::check(SessionKeys::seqFromKey(QString(QLatin1String(NavKey::SessionPrefix))) == -1,
                       "roundtrip: 裸前缀（空尾段）反解为 -1");
    TestHarness::check(SessionKeys::seqFromKey(QLatin1String(NavKey::NewChatPage)) == -1,
                       "roundtrip: 非会话键反解为 -1");
}

// ---- 组 5：真实生成 dataId 的形态，以及两族键不相交的**真实**理由 ----

void testGeneratedDataIdShape()
{
    // QUuid::createUuid().toRfc4122().toHex().left(8) 的产物形状：恰好 8 位小写 hex
    TestHarness::check(SessionKeys::isGeneratedDataId(QStringLiteral("deadbeef")),
                       "shape: 8 位小写 hex 判为生成形态");
    TestHarness::check(SessionKeys::isGeneratedDataId(QStringLiteral("00000001")),
                       "shape: 全数字 8 位仍是合法 hex（这正是旧注释理由失效的根源）");
    TestHarness::check(!SessionKeys::isGeneratedDataId(QStringLiteral("DEADBEEF")),
                       "shape: 大写不算生成形态（toHex 出小写）");
    TestHarness::check(!SessionKeys::isGeneratedDataId(QStringLiteral("deadbee")),
                       "shape: 7 位不算");
    TestHarness::check(!SessionKeys::isGeneratedDataId(QStringLiteral("deadbeef0")),
                       "shape: 9 位不算");
    TestHarness::check(!SessionKeys::isGeneratedDataId(QStringLiteral("deadbeeg")),
                       "shape: 非 hex 字符不算");
    TestHarness::check(!SessionKeys::isGeneratedDataId(QString()), "shape: 空串不算");

    // 真实保证：seq 在现实量级内（< 10^7，即尾段不足 8 位）时，任何生成形态的 dataId
    // 都不会与新建会话键相撞。钉的是「长度」这条理由，不是「字母表」。
    int collisions = 0;
    for (int seq = 1; seq < 10000000; seq *= 10)
    {
        const QString tail = SessionKeys::keyForSeq(seq).mid(QByteArray(NavKey::SessionPrefix).size());
        if (tail.size() == 8)
            ++collisions; // 尾段已达 8 位 → 长度屏障失效，进入可与 hex 相遇的区间
    }
    TestHarness::check(collisions == 0,
                       "disjoint: seq < 10^7 时尾段不足 8 位，长度屏障成立");

    // 屏障失效的确切位置：seq 首次写到 8 位（10^7）时，纯数字 hex dataId 就能撞上
    TestHarness::check(SessionKeys::collidesWithSeqKey(QStringLiteral("10000000"), 10000000),
                       "disjoint: 屏障恰在 seq=10^7 处失效（长度而非字母表）");
}

// ---- 组 6：撞车谓词本身（旧注释所称「永不」的两个反例）----

void testCollisionPredicate()
{
    // 反例一：短 dataId（手改 config.json / 旧 index.json 迁移的产物）与新建会话第一个键直接相等
    TestHarness::check(SessionKeys::collidesWithSeqKey(QStringLiteral("1"), 1),
                       "collide: dataId \"1\" 与 Session_1 撞车（旧注释的「永不」不成立）");
    TestHarness::check(SessionKeys::collidesWithSeqKey(QStringLiteral("42"), 42),
                       "collide: dataId \"42\" 与第 42 号新建键撞车");
    // 反例二：8 位纯数字 hex 与同值 8 位 seq 撞车（证明 hex ⊇ 十进制数字串）
    TestHarness::check(SessionKeys::collidesWithSeqKey(QStringLiteral("12345678"), 12345678),
                       "collide: 8 位纯数字 hex 与同值 seq 撞车");
    // 非撞车侧：带字母的 hex 永不与 seq 相等；前导零形态也不等
    TestHarness::check(!SessionKeys::collidesWithSeqKey(QStringLiteral("deadbeef"), 1),
                       "collide: 含字母 hex 不与 seq 撞");
    TestHarness::check(!SessionKeys::collidesWithSeqKey(QStringLiteral("00000001"), 1),
                       "collide: 前导零 dataId 不与 Session_1 撞（键串不同）");
    TestHarness::check(!SessionKeys::collidesWithSeqKey(QStringLiteral("1"), 2),
                       "collide: 同 dataId 只与同号 seq 撞，不与他号撞");

    // 撞车必须是对称的键相等，而不是前缀相似
    TestHarness::check(SessionKeys::keyForDataId(QStringLiteral("1")) ==
                           SessionKeys::keyForSeq(1),
                       "collide: 两族键逐字相等才算撞车");
}

} // namespace

int tst_sessionkeys()
{
    const int before = TestHarness::failCount();

    testSeqKeyShape();
    testDataIdKeyIsVerbatim();
    testIsSessionKey();
    testSeqRoundtrip();
    testGeneratedDataIdShape();
    testCollisionPredicate();

    return TestHarness::failCount() - before;
}
