#include "LiteHarness.h"
#include <FramelessHelper/Core/framelessmanager.h>
#include <FramelessHelper/Widgets/framelesswidgetshelper.h>
#include <FramelessHelper/Widgets/standardsystembutton.h>
#include <FramelessHelper/Widgets/standardtitlebar.h>
#include <FluThemeButton.h>
#include <QIcon>
#include "NewChatPage.h"
#include "ChatSessionPage.h"
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
#include <QColor>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QMessageBox>
#include "FluentInputDialog.h"
#include "ThemeAware.h"
#include <FluScrollBar.h>        // 导航列内浮动滚动条（底色对齐追加块的目标控件）
#include <FluStyleSheetUtils.h>  // getStyleSheetDir/getQssByFileName：按 dir/theme/文件名 读追加块原文
#include <QTimer>   // singleShot(0) 延一拍执行磁盘数据目录递归删除
#include <QDebug>   // qWarning：目录删除失败仅告警容忍（外部编辑器占用等）
#include <algorithm> // std::stable_sort（Qt6 已移除 qStableSort）

FRAMELESSHELPER_USE_NAMESPACE

namespace
{
// 幂等地把 QSS 片段追加进控件「自身样式表」尾部：先按标记截去上一次的同款片段再追加，
// 保证我方规则恒为全表最末（同表 + 同特异性 + 后序 ⇒ 对 FluentUI 原生规则必胜），
// 且主题切换重放不会让样式表无限增长。marker 必须出现在 chunk 首行。
void appendOwnSheetOverride(QWidget *w, const char *marker, const QString &chunk)
{
    QString cur = w->styleSheet();
    const int idx = cur.indexOf(QLatin1String(marker));
    if (idx >= 0)
        cur.truncate(idx);
    w->setStyleSheet(cur + chunk);
}
} // namespace

LiteHarness::LiteHarness(QWidget *parent) : FluFrameLessWidget(parent)
{
    QOpenAi::initFromSettings();
    initUi();
    initNavView();
    setupConnections();
    // 导航骨架就绪后、事件循环前恢复历史会话（不切换当前页，仍停留在 NewChatPage）
    restoreSessions();
}

void LiteHarness::initUi()
{
    setWindowTitle("lite-harness");
    setWindowIcon(QIcon(":/res/LiteHarness.ico"));

    m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(Qt::black);
    m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(Qt::black);
    m_titleBar->setFixedHeight(32);

    // 主题按钮挂载点：titlebar 固定三层布局 QHBoxLayout(顶) → 槽位1 QVBoxLayout(居中列) →
    // 槽位0 QHBoxLayout(系统按钮行)（见 standardtitlebar.cpp 构造）。原 C 风格强转+硬索引在
    // 上游结构变动时会空指针解引用/踩内存；改 qobject_cast 逐级判空，失配时 qWarning 跳过插入
    // （不崩不回归），结构正常时视觉结果与原代码一致
    auto *titleBarLayout = m_titleBar ? qobject_cast<QHBoxLayout *>(m_titleBar->layout()) : nullptr;
    auto *centerSlot = titleBarLayout ? titleBarLayout->itemAt(1) : nullptr;
    auto *centerLayout = centerSlot ? qobject_cast<QVBoxLayout *>(centerSlot->layout()) : nullptr;
    auto *buttonSlot = centerLayout ? centerLayout->itemAt(0) : nullptr;
    auto *buttonLayout = buttonSlot ? qobject_cast<QHBoxLayout *>(buttonSlot->layout()) : nullptr;
    FluThemeButton *themeButton = nullptr;
    if (buttonLayout)
    {
        themeButton = new FluThemeButton;
        buttonLayout->insertWidget(0, themeButton);
    }
    else
    {
        qWarning() << "LiteHarness: unexpected titlebar layout structure, theme button not inserted";
    }

    m_navView = new FluVNavigationView;
    m_sLayout = new FluStackedLayout;
    m_contentLayout->addWidget(m_navView);
    m_contentLayout->addLayout(m_sLayout);

    // initNavView();
    if (themeButton)
        FramelessWidgetsHelper::get(this)->setHitTestVisible(themeButton);
}

void LiteHarness::initNavView()
{
    m_navView->hideSearchItem();

    // i18n 第八轮：导航三项改为中文源 + tr()，存成员指针供 LanguageChange 重译
    m_newChatItem = m_navView->insertIconTextItem(FluAwesomeType::Pencil, tr("新建对话"), NavKey::NewChatPage);
    m_newChatPage = new NewChatPage;
    m_sLayout->addWidget(NavKey::NewChatPage, m_newChatPage);

    // Sessions 分组改用 NavItem（FluVNavigationIconTextItem 最小派生）：额外具备 removeChildItem，
    // 供会话删除时摘除对应导航子项。构造参数与 insertIconTextItem(3 参) 内部所建者一致（itemType=IconText），
    // 仍经 addItemToMidLayout 注册进导航（setParentView + 布局成员），getItemByKey(NavKey::SessionsGroup) 照常命中。
    m_sessionsItem = new NavItem(FluAwesomeType::List, tr("会话"), NavKey::SessionsGroup);
    m_navView->addItemToMidLayout(m_sessionsItem);

    m_settingsItem = new FluVNavigationSettingsItem(FluAwesomeType::Settings, tr("设置"), this);
    m_settingsItem->setKey(NavKey::SettingsPage);
    m_navView->addItemToBottomLayout(m_settingsItem);

    auto settingsPage = new SettingsPage;
    m_sLayout->addWidget(NavKey::SettingsPage, settingsPage);

    // setViewWidth 仅对已存在的 item 生效，须在全部 item 插入后调用，否则新增项停留在构造默认宽度 180
    m_navView->setViewWidth(256);

    /// clicked
    // emit m_navView->keyChanged(NavKey::NewChatPage);
    m_newChatItem->itemClicked();
}

void LiteHarness::setupConnections()
{
    /// navView;
    connect(m_navView, &FluVNavigationView::keyChanged, this, [=](QString key) {
        m_sLayout->setCurrentWidget(key);
    });

    /// new chat;
    connect(m_newChatPage, &NewChatPage::newChatRequested, this, &LiteHarness::createSession);

    /// theme;
    // FluFrameLessWidget 派生自 FramelessWidget（非 FluWidget），无主题自动联动：
    // 这里的显式 connect 是主题切换的唯一通路，必须保留；首刷 onThemeChanged() 保证
    // 启动即按当前主题着色标题栏（initUi 里的黑色前景是主题盲初值）。
    // 标题栏着色非 QSS 可表达（chromePalette API），保留在本槽；窗口 QSS 样板收敛到 ThemeAware::bind
    onThemeChanged();
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [this](FluTheme) { onThemeChanged(); });
    // extraRefresh = 导航底色对齐追加块（窗口级 QSS 压不过 FluentUI 控件自身表，见 applyNavAlignOverrides 注释）。
    // 时序双保险：
    //  - bind 首次 apply 时同步执行一次——此刻各控件构造期自加载的自身表已就位，直接追加，首帧即正确底色；
    //  - 每次 apply（含 themeChanged）再排一个 singleShot(0)——FluThemeUtils::setTheme 把「emit themeChanged +
    //    代理批处理 flush（用纯文件内容重写控件自身表）」打包在同一个 queued lambda 里，本回调在该 lambda
    //    执行中排队，必然落在 flush 之后重放追加，抵消代理重写、且晚于 nav/滚动条各自的连接（构造更早）。
    ThemeAware::bind("LiteHarness.qss", this, [this] {
        applyNavAlignOverrides();
        QTimer::singleShot(0, this, [this] { applyNavAlignOverrides(); });
    });
}

void LiteHarness::createSession(const QString &text)
{
    const QString key = m_registry.nextSessionKey();

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
    // 登记 key→页面/数据ID（数据ID 供右键删除/重命名定位 index.json 条目；子项控件→key 于建子项后再登记）
    m_registry.insert(key, sessionPage, sessionDataId);
    m_sLayout->addWidget(key, sessionPage);

    // 登记全局会话索引：dataId/标题/模型/工作目录 + 创建/活跃时间，供下次启动恢复定位。
    // 紧随 setModel 之后，故 currentModel() 已是本会话最终模型。
    // 索引「根」固定为进程当前目录下 .lite-harness（即 restoreSessions 启动读取处，
    // 路径经 SessionStore::rootDirFor 单源派生），只有条目「工作目录字段」记所选目录——
    // 若把根也改到所选目录，切换工作目录后旧会话将无法被发现。
    SessionStore::upsertEntry(
        SessionStore::rootDirFor(QDir::currentPath()),
        sessionDataId, title, m_newChatPage->currentModel(), newWorkDir);

    auto sessionsItem = static_cast<NavItem *>(m_navView->getItemByKey(NavKey::SessionsGroup));
    auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, NavKey::SessionsGroup);
    if (childItem == nullptr)
        return;
    // 登记子项本体→key（重命名/删除反查专用），并递归给子项及其全部后代装右键过滤器
    // （行区域被 m_wrapWidget1/图标/标签/箭头占满，Qt 只投递最深接收者，装主体收不到）
    hookContextMenu(childItem);
    m_registry.mapChildItem(key, childItem);

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

void LiteHarness::restoreSessions()
{
    const QString root = SessionStore::rootDirFor(QDir::currentPath());
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

    auto sessionsItem = static_cast<NavItem *>(m_navView->getItemByKey(NavKey::SessionsGroup));
    for (const QJsonObject &e : std::as_const(entries))
    {
        const QString dataId = e.value(QStringLiteral("dataId")).toString();
        if (dataId.isEmpty())
            continue;
        // 导航 key 用 "Session_" + 十六进制 dataId：hex 永不等于新会话的十进制自增，避免键冲突
        const QString key = SessionRegistry::keyForDataId(dataId);
        if (m_registry.contains(key))
            continue;

        // 恢复时沿用条目记录的工作目录；目录已不存在则静默回退进程当前目录，
        // 避免因外部删/移目录导致会话无法打开（数据仍在其原 sessions/<dataId> 下按所选根解析）
        const QString entryWork = e.value(QStringLiteral("workDir")).toString();
        const QString finalWork = (!entryWork.isEmpty() && QFileInfo(entryWork).isDir())
                                      ? entryWork
                                      : QDir::currentPath();

        auto page = new ChatSessionPage(dataId, finalWork);
        page->restoreFromDisk(); // 无 history.json 则为空会话页
        // 恢复会话同样登记页面与 key→dataId，使其可被右键删除/重命名
        m_registry.insert(key, page, dataId);
        m_sLayout->addWidget(key, page);

        const QString title = e.value(QStringLiteral("title")).toString();
        // 用索引标题播种会话页：否则重启后右侧面板恒显占位「新会话」与导航不一致；
        // 播种后 maybeCaptureSessionTitle 也不会再用首条消息覆盖历史改名结果
        page->setSessionTitle(title);
        auto childItem = m_navView->insertIconTextItem(FluAwesomeType::Message, title, key, NavKey::SessionsGroup);
        if (childItem == nullptr)
            continue;
        // 恢复的子项同样登记本体 + 递归装右键过滤器（同 createSession）
        hookContextMenu(childItem);
        m_registry.mapChildItem(key, childItem);
        // 同 createSession：子项默认宽 180 不继承父宽，恢复后统一对齐到父项当前宽
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

void LiteHarness::hookContextMenu(QWidget *childItem)
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
        m_registry.watchWidget(item->getKey(), w);
    }
}

FluVNavigationIconTextItem *LiteHarness::resolveSessionItem(QWidget *w) const
{
    // 沿父子链向上找首个登记在册的会话子项本体（后代命中 → 归位到本体）
    for (QWidget *p = w; p; p = p->parentWidget())
    {
        if (m_registry.isWatched(p))
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
    if (!widget || !m_registry.isWatched(widget))
        return FluFrameLessWidget::eventFilter(watched, event);

    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return FluFrameLessWidget::eventFilter(watched, event);

    auto *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::RightButton && !(me->buttons() & Qt::RightButton))
        return FluFrameLessWidget::eventFilter(watched, event);

    auto *item = resolveSessionItem(widget);
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
            if (m_registry.isWatched(item))
                showSessionMenu(item, gp);
        });
    }
    return true; // 释放同样吞掉，规避基类 mouseReleaseEvent 触发 itemClicked
}

void LiteHarness::showSessionMenu(QWidget *childItem, const QPoint &globalPos)
{
    const QString key = m_registry.keyForChildItem(childItem);
    if (key.isEmpty())
        return;
    auto menu = new FluRoundMenu(this);
    auto renameAction = new FluAction(FluAwesomeType::Edit, tr("重命名"), menu);
    auto deleteAction = new FluAction(FluAwesomeType::Delete, tr("删除会话"), menu);
    connect(renameAction, &QAction::triggered, this, [this, key]() { renameSession(key); });
    connect(deleteAction, &QAction::triggered, this, [this, key]() { deleteSession(key); });
    menu->addAction(renameAction);
    menu->addAction(deleteAction);
    // 注意：FluRoundMenu::exec 并非 QMenu 的阻塞式 exec，它只是「动画定位 + show()」立即返回。
    // 若在调用后 deleteLater，菜单会在本轮事件循环刚回到空闲时即被销毁——用户看到的就是
    // 「右键没反应」。正确用法参照 FluentUI Gallery（FluMenuBarPage）：生命周期交给
    // Qt::WA_DeleteOnClose，菜单关闭（closeEvent 内已 emit closed()）时自行回收。
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->exec(globalPos);
}

void LiteHarness::renameSession(const QString &key)
{
    if (!m_registry.contains(key))
        return;
    const QString dataId = m_registry.dataId(key);
    if (dataId.isEmpty())
        return;

    // 据 key 找回导航子项（注册表持观察指针，子项已销毁时自然为空）
    auto *childItem = qobject_cast<FluVNavigationIconTextItem *>(m_registry.childItem(key));
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
        if (auto *grp = static_cast<NavItem *>(m_navView->getItemByKey(NavKey::SessionsGroup)))
            grp->adjustItemHeight(childItem);
    }
    // 仅更新索引标题（model/workDir 传空即不覆盖），刷新 lastActiveMs；
    // 根路径经 SessionStore::rootDirFor 单源派生（与原手写 ".lite-harness" 拼接等价）
    SessionStore::upsertEntry(
        SessionStore::rootDirFor(QDir::currentPath()),
        dataId, text, QString(), QString());

    // 回填会话页：右侧信息面板的标题单源在 ChatSessionPage，不推这一步即表现为「导航已改、面板仍旧」。
    // 顺序上先落 index 再推页面：页面标题为纯内存态，index 才是重启后的权威来源。
    if (auto *page = m_registry.page(key))
        page->setSessionTitle(text);
}

void LiteHarness::deleteSession(const QString &key)
{
    if (!m_registry.contains(key))
        return;
    ChatSessionPage *page = m_registry.page(key);
    const QString dataId = m_registry.dataId(key);
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
        if (auto *newChat = static_cast<FluVNavigationIconTextItem *>(m_navView->getItemByKey(NavKey::NewChatPage)))
            newChat->onItemClicked(); // 触发 itemClicked→onItemClicked→keyChanged，堆叠切至 NewChatPage
    }

    // 趁 page 仍存活捕获会话数据根（deleteLater 析构后 AgentLoop 不复存在，无法再取）
    const QString dataRoot = page->sessionDataRoot();

    // 从分组摘除导航子项（deleteLater 在 removeChildItem 内完成）
    if (auto *grp = static_cast<NavItem *>(m_navView->getItemByKey(NavKey::SessionsGroup)))
        grp->removeChildItem(key);
    m_sLayout->removeWidget(key, page); // 仅移出堆叠，不销毁（page 的注销登记在下一步统一执行）
    page->deleteLater();                // 当前页已切走，安全回收会话页及其子控件

    // 注销登记：页面/数据ID/子项本体→key/受控控件表→key 一并摘除（与原四表清理同位置同时序）。
    // （过滤器表若不清理，已 delete 控件的悬空地址可能被新控件复用导致误命中）
    m_registry.unregister(key);

    // 移出 index.json 条目（根路径经 SessionStore::rootDirFor 单源派生）
    SessionStore::removeEntry(
        SessionStore::rootDirFor(QDir::currentPath()), dataId);

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
    // 原 if/else 两分支仅前景色一处差异（light=black / dark=white），背景恒透明；
    // 变化参数提取为变量后单套调用收口，消除重复 4+3 行（行为与原分支逐句等价）
    const QColor foreground = FluThemeUtils::isLightTheme() ? QColor(Qt::black) : QColor(Qt::white);
    m_titleBar->chromePalette()->setTitleBarActiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarInactiveBackgroundColor(Qt::transparent);
    m_titleBar->chromePalette()->setTitleBarActiveForegroundColor(foreground);
    m_titleBar->chromePalette()->setTitleBarInactiveForegroundColor(foreground);
    m_titleBar->minimizeButton()->setActiveForegroundColor(foreground);
    m_titleBar->closeButton()->setActiveForegroundColor(foreground);
    m_titleBar->maximizeButton()->setActiveForegroundColor(foreground);
    m_titleBar->show();
}

// 把导航列/滚动条底色覆盖块追加到 FluentUI 控件「自身样式表」尾部。
// 为何窗口级 QSS 不行（9e7fcf2 像素实证）：FluentUI 各控件把主题 QSS 经
// setStyleSheet 挂在自身，Qt 级联中越靠近控件的表越优先，祖先（窗口）表无论
// 特异性多高都压不过——导航带实测恒为原生 243/32/33,37,43 而非基准色。
// 自身表内追加则同表同特异性、后序必胜；qproperty 规则（滚动条 trunk）亦住自身表，同理。
void LiteHarness::applyNavAlignOverrides()
{
    if (!m_navView)
        return;

    // 路径与 FluentUI 控件自加载完全同源：dir(=:/stylesheet/) + 小写主题名 + 文件名
    const QString dir = FluStyleSheetUtils::getUtils()->getStyleSheetDir();
    const QString theme = FluThemeUtils::getThemeName();
    constexpr const char *kMarker = "/*lh-nav-align*/";

    // 1) 导航本体与 widget1/2/3（追加块见 stylesheet/<theme>/LiteHarnessNavAlign.qss）
    const QString navChunk =
        FluStyleSheetUtils::getQssByFileName(dir + theme + "/LiteHarnessNavAlign.qss");
    if (!navChunk.isEmpty())
        appendOwnSheetOverride(m_navView, kMarker, navChunk);

    // 2) 导航列内浮动滚动条 trunk（构造即存在、随导航常驻，findChildren 一次全覆盖；
    //    仅 nav 子树，聊天页等其他滚动条不受影响）
    const QString barChunk =
        FluStyleSheetUtils::getQssByFileName(dir + theme + "/LiteHarnessScrollBarAlign.qss");
    if (!barChunk.isEmpty())
    {
        const auto bars = m_navView->findChildren<FluScrollBar *>();
        for (FluScrollBar *bar : bars)
            appendOwnSheetOverride(bar, kMarker, barChunk);
    }
}

// 退出守卫：AgentLoop 回合含流式请求与工具子进程，直接关窗会丢失未完成回合
// （历史仅持久化已落盘部分）。有运行中会话时先确认；确认退出则逐个 stop()
// （各自收敛网络请求/子进程）后照常走基类关闭链；取消则 ignore 事件。
// 与 deleteSession 的“删前停”逻辑相互独立，此处不改其行为。
void LiteHarness::closeEvent(QCloseEvent *event)
{
    // alivePages 已滤除悬挂观察指针（等价原 page&& 守卫），守卫逻辑本身不动
    QList<ChatSessionPage *> running;
    for (ChatSessionPage *page : m_registry.alivePages())
    {
        if (page->isRunning())
            running.append(page);
    }

    if (!running.isEmpty())
    {
        const auto ret = QMessageBox::question(
            this, tr("退出确认"),
            tr("任务仍在运行，退出将丢失未完成回合。确定退出吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
        for (ChatSessionPage *page : running)
            page->stop();
    }

    FluFrameLessWidget::closeEvent(event);
}

// i18n 第八轮：语言切换走「重启生效」链路（主窗口构造前 translator 已装好，本钩子
// 正常不命中）；保留为「运行中广播」的兜底通道——Qt 在 QTranslator 安装/移除时向
// 全部 widget 发送 QEvent::LanguageChange，若未来演进为免重启切换，导航文本即时重译
void LiteHarness::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    FluFrameLessWidget::changeEvent(event);
}

void LiteHarness::retranslateUi()
{
    // 导航三项 label 文本重取 tr()（item 常驻，仅文本需重译；key 不变）
    if (m_newChatItem && m_newChatItem->getLabel())
        m_newChatItem->getLabel()->setText(tr("新建对话"));
    if (m_sessionsItem && m_sessionsItem->getLabel())
        m_sessionsItem->getLabel()->setText(tr("会话"));
    if (m_settingsItem && m_settingsItem->getLabel())
        m_settingsItem->getLabel()->setText(tr("设置"));
}
