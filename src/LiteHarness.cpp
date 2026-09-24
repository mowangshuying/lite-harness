#include "LiteHarness.h"
#include <FramelessHelper/Core/framelessmanager.h>
#include <FramelessHelper/Widgets/framelesswidgetshelper.h>
#include <FramelessHelper/Widgets/standardsystembutton.h>
#include <FramelessHelper/Widgets/standardtitlebar.h>
#include <FluThemeButton.h>
#include <QIcon>
#include "NewChatPage.h"
#include "ChatSessionPage.h"
#include "MessageBubbleWidget.h"
#include "SettingsPage.h"
#include <FluVNavigationSettingsItem.h>
#include <FluVNavigationIconTextItem.h>
#include <FluRoundMenu.h>
#include <FluAction.h>
#include <FluMessageBox.h>
#include "NavItem.h"
#include <QUuid>
#include "QOpenAi.h"
#include "SessionStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QVector>
#include <QDateTime>
#include <QEvent>
#include <QMouseEvent>
#include "FluentInputDialog.h"
#include <QTimer>   // singleShot(0) 延一拍执行磁盘数据目录递归删除
#include <QDebug>   // qWarning：目录删除失败仅告警容忍（外部编辑器占用等）
#include <algorithm> // std::stable_sort（Qt6 已移除 qStableSort）

FRAMELESSHELPER_USE_NAMESPACE

LiteHarness::LiteHarness(QWidget *parent) : FluFrameLessWidget(parent)
{
    QOpenAi::initByEnv();
    __initUI();
    __initNavView();
    __connect();
    // 导航骨架就绪后、事件循环前恢复历史会话（不切换当前页，仍停留在 NewChatPage）
    __restoreSessions();
}

void LiteHarness::__initUI()
{
    setWindowTitle("lite-harness");
    setWindowIcon(QIcon(":/res/LiteHarness.ico"));

    m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::black);
    m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::black);
    m_titleBar->setFixedHeight(32);

    auto hLayout = (QHBoxLayout *)m_titleBar->layout();
    auto vLayout = (QVBoxLayout *)hLayout->itemAt(1)->layout();
    auto hButtonLayout = (QHBoxLayout *)vLayout->itemAt(0)->layout();
    auto themeButton = new FluThemeButton;
    hButtonLayout->insertWidget(0, themeButton);

    m_navView = new FluVNavigationView;
    // m_navView->setViewWidth(200);
    m_sLayout = new FluStackedLayout;
    m_contentLayout->addWidget(m_navView);
    m_contentLayout->addLayout(m_sLayout);

    // __initNavView();
    FramelessWidgetsHelper::get(this)->setHitTestVisible(themeButton);
}

void LiteHarness::__initNavView()
{
    m_navView->hideSearchItem();

    auto newChatItem =  m_navView->insertIconTextItem(FluAwesomeType::Pencil, "New Chat", "NewChatPage");
    m_newChatPage = new NewChatPage;
    m_sLayout->addWidget("NewChatPage", m_newChatPage);

    // Sessions 分组改用 NavItem（FluVNavigationIconTextItem 最小派生）：额外具备 removeChildItem，
    // 供会话删除时摘除对应导航子项。构造参数与 insertIconTextItem(3 参) 内部所建者一致（itemType=IconText），
    // 仍经 addItemToMidLayout 注册进导航（setParentView + 布局成员），getItemByKey("SessionsGroup") 照常命中。
    auto sessionsItem = new NavItem(FluAwesomeType::List, "Sessions", "SessionsGroup");
    m_navView->addItemToMidLayout(sessionsItem);

    auto settingsItem = new FluVNavigationSettingsItem(FluAwesomeType::Settings, tr("Setting"), this);
    settingsItem->setKey("SettingsPage");
    m_navView->addItemToBottomLayout(settingsItem);

    auto settingsPage = new SettingsPage;
    m_sLayout->addWidget("SettingsPage", settingsPage);

    // setViewWidth 仅对已存在的 item 生效，须在全部 item 插入后调用，否则新增项停留在构造默认宽度 180
    m_navView->setViewWidth(256);

    /// clicked
    // emit m_navView->keyChanged("NewChatPage");
    newChatItem->itemClicked();
}

void LiteHarness::__connect()
{
    /// navView;
    connect(m_navView, &FluVNavigationView::keyChanged, this, [=](QString key) {
        m_sLayout->setCurrentWidget(key);
    });

    /// new chat;
    connect(m_newChatPage, &NewChatPage::newChatRequested, this, &LiteHarness::__createSession);

    /// theme;
    onThemeChanged();
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [=](FluTheme theme) { onThemeChanged(); });
}

void LiteHarness::__createSession(const QString &text)
{
    const int sessionId = ++m_sessionCount;
    const QString key = QString("Session_%1").arg(sessionId);

    auto title = text.simplified();
    if (title.length() > 12)
        title = title.left(12) + "...";
    if (title.isEmpty())
        title = tr("新会话");

    // 会话数据目录短 ID：一次性 uuid8（不随重启复用，避免串写），与仓内 task_<hex8> 命名风格一致；
    // 与导航 UI key（Session_%1，仅进程内自增）解耦——后者不能直接当数据目录名。
    const QString sessionDataId = QString::fromLatin1(
        QUuid::createUuid().toRfc4122().toHex().left(8));

    // 新建会话页所选工作目录（默认取设置页默认目录或进程当前目录，可临时改选）
    const QString newWorkDir = m_newChatPage->currentWorkDir();

    auto sessionPage = new ChatSessionPage(sessionDataId, newWorkDir);
    // 新会话继承新建会话页选择的模型（须在 startConversation 前注入，使首轮请求即用该模型）
    sessionPage->setModel(m_newChatPage->currentModel());
    sessionPage->startConversation(text);
    m_sessions.insert(key, sessionPage);
    // 记录 key→dataId，供右键删除/重命名定位 index.json 条目（子项控件→key 于建子项后再登记）
    m_keyToDataId.insert(key, sessionDataId);
    m_sLayout->addWidget(key, sessionPage);

    // 登记全局会话索引：dataId/标题/模型/工作目录 + 创建/活跃时间，供下次启动恢复定位。
    // 紧随 setModel 之后，故 currentModel() 已是本会话最终模型。
    // 索引「根」固定为进程当前目录下 .lite-harness（即 __restoreSessions 启动读取处），
    // 只有条目「工作目录字段」记所选目录——若把根也改到所选目录，切换工作目录后旧会话将无法被发现。
    SessionStore::upsertEntry(
        QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness")),
        sessionDataId, title, m_newChatPage->currentModel(), newWorkDir);

    auto sessionsItem = (NavItem *)m_navView->getItemByKey("SessionsGroup");
    auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, "SessionsGroup");
    if (childItem == nullptr)
        return;
    // 登记子项本体→key（重命名/删除反查专用），并递归给子项及其全部后代装右键过滤器
    // （行区域被 m_wrapWidget1/图标/标签/箭头占满，Qt 只投递最深接收者，装主体收不到）
    __hookContextMenu(childItem);
    m_childWidgetToKey.insert(childItem, key);

    // 子项构造默认宽 180，addItem 不会继承父宽；借 setItemWidth 的递归语义将全部子项对齐到父项当前宽
    // （与 Gallery "先建 item 后 setViewWidth" 的启动期对齐语义一致）
    sessionsItem->setItemWidth(sessionsItem->width());

    if (m_navView->isLong())
    {
        if (sessionsItem->getItems().size() == 1)
            sessionsItem->onItemClicked();
        else
            sessionsItem->adjustItemHeight(sessionsItem);
    }
    childItem->onItemClicked();
}

void LiteHarness::__restoreSessions()
{
    const QString root = QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness"));
    const QJsonArray index = SessionStore::loadIndex(root);
    if (index.isEmpty())
        return;

    // 收集并按创建时间升序稳定排序，使恢复后导航顺序与历史创建顺序一致
    QVector<QJsonObject> entries;
    entries.reserve(index.size());
    for (const QJsonValue &v : index)
        entries.append(v.toObject());
    std::stable_sort(entries.begin(), entries.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("createdMs")).toDouble()
             < b.value(QStringLiteral("createdMs")).toDouble();
    });

    auto sessionsItem = (NavItem *)m_navView->getItemByKey("SessionsGroup");
    for (const QJsonObject &e : std::as_const(entries))
    {
        const QString dataId = e.value(QStringLiteral("dataId")).toString();
        if (dataId.isEmpty())
            continue;
        // 导航 key 用 "Session_" + 十六进制 dataId：hex 永不等于新会话的十进制自增，避免键冲突
        const QString key = QStringLiteral("Session_") + dataId;
        if (m_sessions.contains(key))
            continue;

        // 恢复时沿用条目记录的工作目录；目录已不存在则静默回退进程当前目录，
        // 避免因外部删/移目录导致会话无法打开（数据仍在其原 sessions/<dataId> 下按所选根解析）
        const QString entryWork = e.value(QStringLiteral("workDir")).toString();
        const QString finalWork = (!entryWork.isEmpty() && QFileInfo(entryWork).isDir())
                                      ? entryWork
                                      : QDir::currentPath();

        auto page = new ChatSessionPage(dataId, finalWork);
        page->restoreFromDisk(); // 无 history.json 则为空会话页
        m_sessions.insert(key, page);
        m_keyToDataId.insert(key, dataId); // 恢复会话同样登记 key→dataId，使其可被右键删除/重命名
        m_sLayout->addWidget(key, page);

        const QString title = e.value(QStringLiteral("title")).toString();
        auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, "SessionsGroup");
        if (childItem == nullptr)
            continue;
        // 恢复的子项同样登记本体 + 递归装右键过滤器（同 __createSession）
        __hookContextMenu(childItem);
        m_childWidgetToKey.insert(childItem, key);
        // 同 __createSession：子项默认宽 180 不继承父宽，恢复后统一对齐到父项当前宽
        sessionsItem->setItemWidth(sessionsItem->width());
        // 恢复不切换当前页（不调 childItem->onItemClicked），仅按需展开/调高保持导航视觉一致
        if (m_navView->isLong())
        {
            if (sessionsItem->getItems().size() == 1)
                sessionsItem->onItemClicked();
            else
                sessionsItem->adjustItemHeight(sessionsItem);
        }
    }
}

void LiteHarness::__hookContextMenu(QWidget *childItem)
{
    // 会话子项行区域被后代控件占满（m_wrapWidget1/indicator/iconButton/label/arrow），
    // Qt 鼠标事件只投递最深接收者 → 必须给本体与全部后代逐一装过滤器并登记，
    // 否则 item 级过滤器永远收不到 press/release（右键菜单不弹 + 释放漏给基类误翻页）。
    auto *item = qobject_cast<FluVNavigationIconTextItem *>(childItem);
    if (!item)
        return;
    QList<QWidget *> targets;
    targets.append(item);
    const QList<QWidget *> descendants = item->findChildren<QWidget *>();
    targets.append(descendants);
    for (QWidget *w : std::as_const(targets))
    {
        w->installEventFilter(this);
        m_ctxWatchedToKey.insert(w, item->getKey());
    }
}

FluVNavigationIconTextItem *LiteHarness::__resolveSessionItem(QWidget *w) const
{
    // 沿父子链向上找首个登记在册的会话子项本体（后代命中 → 归位到本体）
    for (QWidget *p = w; p; p = p->parentWidget())
    {
        if (m_ctxWatchedToKey.contains(p))
            return qobject_cast<FluVNavigationIconTextItem *>(p);
    }
    return nullptr;
}

bool LiteHarness::eventFilter(QObject *watched, QEvent *event)
{
    // 仅处理登记在册控件（会话子项本体或其后代）上的右键。基类 mouseReleaseEvent
    // 不辨按键即 emit itemClicked，会在右键时误翻页；故按下/释放均吞掉，itemClicked 永不触发。
    // 菜单弹出时机必须在「释放之后延一拍」：若在 press 内直接 exec，右键物理未抬起、
    // 平台随后派发的上下文菜单消息会使刚弹出的模态菜单瞬间被关闭（表现为右键无反应）。
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || !m_ctxWatchedToKey.contains(widget))
        return FluFrameLessWidget::eventFilter(watched, event);

    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return FluFrameLessWidget::eventFilter(watched, event);

    auto *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::RightButton && !(me->buttons() & Qt::RightButton))
        return FluFrameLessWidget::eventFilter(watched, event);

    auto *item = __resolveSessionItem(widget);
    if (!item)
        return false; // 已登记的右键一律吞掉，不放行（防漏给基类触发 itemClicked）

    if (type == QEvent::MouseButtonPress)
        return true; // 按下即吞（不弹菜单：press 内 exec 会被随后的 WM_CONTEXTMENU 关闭）

    // 释放：命中判定用「会话子项本体」的 rect（事件可能来自本体之外的后代控件，
    // 后代坐标无法与本体 rect 直接比较，故用全局坐标映射回本体系）
    const QPoint pInItem = item->mapFromGlobal(me->globalPosition().toPoint());
    if (item->rect().contains(pInItem))
    {
        const QPoint gp = me->globalPosition().toPoint();
        QTimer::singleShot(0, this, [this, item, gp]() {
            // 延拍期间子项可能已被删除（如切页/删会话），查表复核
            if (m_ctxWatchedToKey.contains(item))
                __showSessionMenu(item, gp);
        });
    }
    return true; // 释放同样吞掉，规避基类 mouseReleaseEvent 触发 itemClicked
}

void LiteHarness::__showSessionMenu(QWidget *childItem, const QPoint &globalPos)
{
    const QString key = m_childWidgetToKey.value(childItem);
    if (key.isEmpty())
        return;
    auto menu = new FluRoundMenu(this);
    auto renameAction = new FluAction(FluAwesomeType::Edit, tr("重命名"), menu);
    auto deleteAction = new FluAction(FluAwesomeType::Delete, tr("删除会话"), menu);
    connect(renameAction, &QAction::triggered, this, [this, key]() { __renameSession(key); });
    connect(deleteAction, &QAction::triggered, this, [this, key]() { __deleteSession(key); });
    menu->addAction(renameAction);
    menu->addAction(deleteAction);
    // 注意：FluRoundMenu::exec 并非 QMenu 的阻塞式 exec，它只是「动画定位 + show()」立即返回。
    // 若在调用后 deleteLater，菜单会在本轮事件循环刚回到空闲时即被销毁——用户看到的就是
    // 「右键没反应」。正确用法参照 FluentUI Gallery（FluMenuBarPage）：生命周期交给
    // Qt::WA_DeleteOnClose，菜单关闭（closeEvent 内已 emit closed()）时自行回收。
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->exec(globalPos);
}

void LiteHarness::__renameSession(const QString &key)
{
    if (!m_sessions.contains(key))
        return;
    const QString dataId = m_keyToDataId.value(key);
    if (dataId.isEmpty())
        return;

    // 据 key 找回导航子项（QPointer 自动置空已删项）
    FluVNavigationIconTextItem *childItem = nullptr;
    for (auto it = m_childWidgetToKey.begin(); it != m_childWidgetToKey.end(); ++it)
    {
        if (it.value() != key)
            continue;
        if (auto *w = qobject_cast<FluVNavigationIconTextItem *>(it.key()))
            childItem = w;
        break;
    }
    if (!childItem)
        return;

    const QString oldTitle = childItem->getLabel()->text();
    // FluentUI 风格输入框（与删除确认的 FluMessageBox 同款骨架），替代原生 QInputDialog
    const auto [input, ok] = FluentInputDialog::getInputText(this, tr("重命名会话"), tr("名称"), oldTitle);
    const QString text = input.simplified();
    if (!ok || text.isEmpty() || text == oldTitle)
        return;

    childItem->getLabel()->setText(text);
    // 标题变长可能撑破导航宽，按长导航重算该项高度保持换行显示正确
    if (m_navView->isLong())
    {
        if (auto *grp = (NavItem *)m_navView->getItemByKey("SessionsGroup"))
            grp->adjustItemHeight(childItem);
    }
    // 仅更新索引标题（model/workDir 传空即不覆盖），刷新 lastActiveMs
    SessionStore::upsertEntry(
        QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness")),
        dataId, text, QString(), QString());
}

void LiteHarness::__deleteSession(const QString &key)
{
    if (!m_sessions.contains(key))
        return;
    ChatSessionPage *page = m_sessions.value(key);
    const QString dataId = m_keyToDataId.value(key);
    if (!page)
        return;

    FluMessageBox box(tr("删除会话"),
                      tr("确定删除该会话及其全部数据（任务/记忆/历史）吗？此操作不可撤销。"), this);
    if (box.exec() != QDialog::Accepted)
        return;

    // 运行中的循环先停止（取消流/杀进程），避免删除后仍有回调触碰将亡页
    if (page->isRunning())
        page->stop();

    // 若正显示被删页，先切回新建会话页，避免堆叠布局 currentWidget 悬空
    if (m_sLayout->currentWidget() == page)
    {
        if (auto *newChat = (FluVNavigationIconTextItem *)m_navView->getItemByKey("NewChatPage"))
            newChat->onItemClicked(); // 触发 itemClicked→onItemClicked→keyChanged，堆叠切至 NewChatPage
    }

    // 趁 page 仍存活捕获会话数据根（deleteLater 析构后 AgentLoop 不复存在，无法再取）
    const QString dataRoot = page->sessionDataRoot();

    // 从分组摘除导航子项（deleteLater 在 removeChildItem 内完成）
    if (auto *grp = (NavItem *)m_navView->getItemByKey("SessionsGroup"))
        grp->removeChildItem(key);
    m_sLayout->removeWidget(key, page); // 仅移出堆叠，不销毁（page 仍由 m_sessions 持有）
    page->deleteLater();                // 当前页已切走，安全回收会话页及其子控件

    // 清理登记：子项本体→key、本体及后代过滤器表→key、page、key→dataId
    // （过滤器表若不清理，已 delete 控件的悬空地址可能被新控件复用导致误命中）
    for (auto it = m_childWidgetToKey.begin(); it != m_childWidgetToKey.end();)
        it = (it.value() == key) ? m_childWidgetToKey.erase(it) : std::next(it);
    for (auto it = m_ctxWatchedToKey.begin(); it != m_ctxWatchedToKey.end();)
        it = (it.value() == key) ? m_ctxWatchedToKey.erase(it) : std::next(it);
    m_sessions.remove(key);
    m_keyToDataId.remove(key);

    // 移出 index.json 条目
    SessionStore::removeEntry(
        QDir(QDir::currentPath()).filePath(QStringLiteral(".lite-harness")), dataId);

    // 连同磁盘数据目录一并删除。仅对已隔离会话（dataId 非空）执行——未注入 ID 时
    // sessionDataRoot() 回退全局 .lite-harness（含 index.json/skills），据此护栏避免误删全局数据。
    // singleShot(0) 延一拍：确保 page 的 deleteLater→~AgentLoop 完成（杀子进程/停 cron）后目录无占用，
    // removeRecursively 方可成功；返回 false 仅告警容忍（外部编辑器占用等），不阻断。
    if (!dataId.isEmpty() && !dataRoot.isEmpty())
    {
        QTimer::singleShot(0, this, [dataRoot] {
            if (!QDir(dataRoot).removeRecursively())
                qWarning() << "session data dir removal failed:" << dataRoot;
        });
    }
}

void LiteHarness::onThemeChanged()
{
    if (FluThemeUtils::isLightTheme())
    {
        m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::black);
        m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::black);
        m_titleBar->minimizeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->closeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->maximizeButton()->setActiveForegroundColor(Qt::black);
        m_titleBar->show();
    }
    else
    {
        m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
        m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::white);
        m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::white);

        m_titleBar->minimizeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->closeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->maximizeButton()->setActiveForegroundColor(Qt::white);
        m_titleBar->show();
    }
    FluStyleSheetUtils::setQssByFileName("LiteHarness.qss", this, FluThemeUtils::getUtils()->getTheme());   
}
