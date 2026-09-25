#include "SessionRegistry.h"

#include <QWidget>       // QPointer<QWidget> 赋值/data() 需完整类型
#include <iterator>      // std::next（受控控件表按值摘除循环）
#include "ChatSessionPage.h" // QPointer<ChatSessionPage> 赋值需完整类型（T*→QObject* 转换）
#include "NavItem.h"         // NavKey::SessionKeyFmt / SessionPrefix 键常量单源，禁止在此另造字面量

QString SessionRegistry::nextSessionKey()
{
    // 自增语义与原 LiteHarness::createSession 的 ++m_sessionCount 逐句等价，
    // 仅把计数与键拼装从主窗口挪进来（键格式串复用 NavKey 单源常量）
    return QString(NavKey::SessionKeyFmt).arg(++m_sessionSeq);
}

QString SessionRegistry::keyForDataId(const QString &dataId)
{
    return QString(NavKey::SessionPrefix) + dataId;
}

void SessionRegistry::insert(const QString &key, ChatSessionPage *page, const QString &dataId)
{
    // 原实现分家于 m_sessions / m_keyToDataId 两表，登记时机恒为同处；
    // 归并为单条 Entry 后消除「只插一半」的漂移面
    Entry &e = m_entries[key];
    e.page = page;
    e.dataId = dataId;
}

bool SessionRegistry::contains(const QString &key) const
{
    return m_entries.contains(key);
}

ChatSessionPage *SessionRegistry::page(const QString &key) const
{
    return m_entries.value(key).page.data(); // 未登记或已析构 → nullptr
}

QString SessionRegistry::dataId(const QString &key) const
{
    return m_entries.value(key).dataId;
}

QList<ChatSessionPage *> SessionRegistry::alivePages() const
{
    QList<ChatSessionPage *> pages;
    pages.reserve(m_entries.size());
    for (const Entry &e : m_entries)
    {
        if (ChatSessionPage *p = e.page.data()) // 目标已销毁即空，等价原 closeEvent 的 page&& 守卫
            pages.append(p);
    }
    return pages;
}

void SessionRegistry::mapChildItem(const QString &key, QWidget *childItem)
{
    m_entries[key].childItem = childItem;
}

QWidget *SessionRegistry::childItem(const QString &key) const
{
    return m_entries.value(key).childItem.data();
}

QString SessionRegistry::keyForChildItem(const QWidget *childItem) const
{
    // 遍历量级 = 会话数（小），换取单源存储：不再另立 widget→key 反查表，
    // 避免「同一关系两份真相」。子项已销毁的观察指针 data() 为空，不会误命中
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
    {
        if (it->childItem.data() == childItem)
            return it.key();
    }
    return QString();
}

void SessionRegistry::watchWidget(const QString &key, QWidget *w)
{
    m_watchedToKey.insert(w, key);
}

bool SessionRegistry::isWatched(const QWidget *w) const
{
    // 表键为非 const QWidget*（登记侧与 Qt 事件签名一致）；contains 仅比较地址，
    // const_cast 不产生写行为
    return m_watchedToKey.contains(const_cast<QWidget *>(w));
}

void SessionRegistry::unregister(const QString &key)
{
    // 受控控件表按值（key）清理：语义与原 deleteSession 中的双 erase 循环保留一致——
    // 已 delete 控件的地址可能被新控件复用，若不清理会误命中右键过滤，故必须随删除同步摘除
    for (auto it = m_watchedToKey.begin(); it != m_watchedToKey.end();)
        it = (it.value() == key) ? m_watchedToKey.erase(it) : std::next(it);
    m_entries.remove(key); // 连带摘除 page/dataId/childItem 观察记录
}
