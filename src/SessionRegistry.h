#pragma once

// 会话注册表：把主窗口中「纯映射」的会话生命周期数据（key↔会话页、key↔数据短ID、
// 导航子项↔key、右键受控控件↔key）从 LiteHarness 收敛出来，主窗口只保留窗口组装、
// 菜单 UI 与导航联动，通过本类查询/登记。
//
// ── 所有权约定（关键，勿破坏）──
// 会话页对象的生死不由本类决定：其现由 LiteHarness 创建链 + FluStackedLayout 共管
// （removeWidget 仅移出布局、不销毁对象）。本类只持观察指针（QPointer），不参与
// 构造/析构；unregister() 的调用点即原 deleteSession 内四表清理的位置，删除时序不变。
// 选用 QPointer 而非裸指针的原因：若页面/导航子项经本类之外的其它路径先行销毁
// （如窗口整体析构连带），查询自然返回空，杜绝旧裸指针表在地址复用后的悬挂误命中。
//
// ── 键生成 ──
// 导航/堆叠键统一复用 NavKey::SessionKeyFmt / SessionPrefix（NavItem.h 单源），
// 本类是进程内键生成与解析的唯一落点。

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

class ChatSessionPage;
class QWidget;

class SessionRegistry
{
public:
    // ── 键生成 ──────────────────────────────────────────────
    // 新会话键："Session_%1" + 进程内自增号（与原 LiteHarness::m_sessionCount 语义一致，
    // 仅进程内有效、不落盘，故重启后可与恢复会话的 hex 键共存不冲突）
    QString nextSessionKey();
    // 恢复会话键："Session_" + 十六进制 dataId（hex 永不等于十进制自增，避免键冲突）
    static QString keyForDataId(const QString &dataId);

    // ── 会话页登记与正查 ────────────────────────────────────
    // 登记一条会话：key →（观察指针）页面 + 数据短 ID。页面所有权仍在外部
    void insert(const QString &key, ChatSessionPage *page, const QString &dataId);
    bool contains(const QString &key) const;
    // 取页面观察指针目标；未登记或已析构返回 nullptr（调用方判空，与原 page&& 检查等价）
    ChatSessionPage *page(const QString &key) const;
    QString dataId(const QString &key) const;
    // 全部仍存活的会话页（自动跳过悬挂），供 closeEvent 运行中守卫遍历
    QList<ChatSessionPage *> alivePages() const;

    // ── 导航子项映射（key ↔ 子项本体）──────────────────────
    void mapChildItem(const QString &key, QWidget *childItem);
    // key → 子项本体；未登记或子项已销毁返回 nullptr
    QWidget *childItem(const QString &key) const;
    // 子项本体 → key（右键菜单反查归属会话）；未命中返回空串
    QString keyForChildItem(const QWidget *childItem) const;

    // ── 右键过滤器受控控件登记 ──────────────────────────────
    // 会话子项本体及其全部后代逐一登记，eventFilter 以 isWatched 命中判定。
    // 表键为裸 QWidget*：仅做地址比较不解引用，悬空误命中风险由 unregister
    // （子项删除路径同步清理）兜底——沿用原实现「删除时清理防地址复用」的纪律
    void watchWidget(const QString &key, QWidget *w);
    bool isWatched(const QWidget *w) const;

    // ── 注销 ────────────────────────────────────────────────
    // 一次清空该 key 的页面/数据ID/子项/受控控件登记。
    // 注意：只做映射移除，不触碰任何 Qt 对象生命周期（deleteLater 等由调用方按原时序执行）
    void unregister(const QString &key);

private:
    // 单条会话的映射记录：page/childItem 均为观察指针，生命周期归 LiteHarness/布局共管
    struct Entry
    {
        QPointer<ChatSessionPage> page;
        QString dataId;
        QPointer<QWidget> childItem;
    };

    QHash<QString, Entry> m_entries;             // key → 会话映射记录
    int m_sessionSeq = 0;                        // 进程内自增序号（原 m_sessionCount）
    QHash<QWidget *, QString> m_watchedToKey;    // 受控控件 → key（eventFilter O(1) 命中判定）
};
