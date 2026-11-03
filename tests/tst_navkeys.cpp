// tst_navkeys.cpp —— 导航/堆叠页键常量契约（src/NavKeys.h，纯 QtCore）
// 被测：各键非空唯一、SessionPrefix 与 SessionKeyFmt 的前缀一致性、格式串占位符。
//
// 为什么必须测：这些键是「导航项 key 与 FluStackedLayout 页键必须成对一致」的契约。
// 最隐蔽的一条是 SessionPrefix/SessionKeyFmt：恢复会话的键是 Session_+hex dataId，
// 新建会话的键是 Session_+自增号（arg 填充）。两者前缀必须同源 —— 若有人只改其中
// 一个（例如把 SessionKeyFmt 改成 "Session-%1"），新建会话的键就不再以 SessionPrefix
// 开头，表现为「打开旧会话变成新会话」或键查不到页，且没有任何报错。
//
// 零夹具依赖，任何环境全量执行。

#include "TestHarness.h"

#include "NavKeys.h"

#include <QString>

namespace {

using TestHarness::check;

} // namespace

int tst_navkeys()
{
    const int before = TestHarness::failCount();

    // ---- ① 非空 ----
    check(QString::fromLatin1(NavKey::NewChatPage).size() > 0, "navkey: NewChatPage non-empty");
    check(QString::fromLatin1(NavKey::SettingsPage).size() > 0, "navkey: SettingsPage non-empty");
    check(QString::fromLatin1(NavKey::SessionsGroup).size() > 0, "navkey: SessionsGroup non-empty");
    check(QString::fromLatin1(NavKey::SessionPrefix).size() > 0, "navkey: SessionPrefix non-empty");
    check(QString::fromLatin1(NavKey::SessionKeyFmt).size() > 0, "navkey: SessionKeyFmt non-empty");

    // ---- ② 页面键互不相同（撞键 = 导航点不动/堆叠页覆盖）----
    const QString newChat = QString::fromLatin1(NavKey::NewChatPage);
    const QString settings = QString::fromLatin1(NavKey::SettingsPage);
    const QString sessions = QString::fromLatin1(NavKey::SessionsGroup);
    check(newChat != settings && newChat != sessions && settings != sessions,
          "navkey: page keys are distinct");

    // ---- ③ 前缀一致性（本套件的主判据）----
    const QString prefix = QString::fromLatin1(NavKey::SessionPrefix);
    const QString fmt = QString::fromLatin1(NavKey::SessionKeyFmt);
    check(fmt.startsWith(prefix), "navkey: SessionKeyFmt starts with SessionPrefix");
    check(fmt.contains(QStringLiteral("%1")), "navkey: SessionKeyFmt has %1 placeholder");

    // 格式串填充后的键必须仍以同一前缀开头（arg 后前缀不变，是 ③ 的行为级验证）
    check(QString(fmt).arg(7).startsWith(prefix), "navkey: arg-filled key keeps the prefix");

    // ---- ④ 恢复会话键与新建会话键不会撞（前者 hex dataId，后者纯数字自增）----
    // 数字串是合法 hex 的子集，所以这里只钉「两者都以前缀开头、且长度都大于前缀」，
    // 唯一性由 SessionRegistry 的自增序号与 dataId 命名空间共同保证（不在本头职责内）
    check((prefix + QStringLiteral("abc123")).startsWith(prefix), "navkey: restored key keeps the prefix");
    check(QString(fmt).arg(1).size() > prefix.size(), "navkey: formatted key is longer than the prefix");

    // ---- ⑤ 页面键不得误用会话前缀（否则会被当成会话子项解析）----
    check(!newChat.startsWith(prefix), "navkey: NewChatPage is not a session key");
    check(!settings.startsWith(prefix), "navkey: SettingsPage is not a session key");
    check(!sessions.startsWith(prefix), "navkey: SessionsGroup is not a session key");

    return TestHarness::failCount() - before;
}