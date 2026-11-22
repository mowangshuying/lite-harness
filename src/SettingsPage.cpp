#include "SettingsPage.h"
#include "AgentConstants.h"
#include "LayoutConstants.h" // P2-D2：页面留白与其余页同源（kSideMargin/kPageVMargin）
#include "UiScale.h"         // P0 字号缩放：设置卡动作钮尺寸按启动倍率放大
#include "InputDialog.h"
#include "I18n.h"
#include "QOpenAi.h" // 写入 ini 后即时覆盖全局配置（setUrl / setToken）
#include "ModelListFetcher.h" // s18.3 B 项：从服务端拉取 /models
#include "ThemeAware.h"
#include <FluUtils.h>
#include <FluMessageBox.h>
#include <FluPMenu.h>  // s18.3：模型清单的「管理」菜单（已随三主题 QSS 着色，无需新增 qss）
#include <FluAction.h>
#include <QAction>     // menu.exec() 的返回类型与逐项 action（不依赖 QMenu 的传递包含）
#include <QCursor>     // QCursor::pos()：菜单在鼠标处弹出
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QShowEvent> // s17.12 showEvent：自动校准的触发点（BasePage 未覆写，定义须自取）
#include <QFontMetrics> // WorkDirSettingCard 说明位的长路径中间省略（同 WorkDirPathBar 口径）
#include <QLocale>
#include "AppSettings.h"
#include <QFileInfo>
#include <QFileDialog>
#include <QUrl>
#include <FluVScrollView.h>
#include <FluLabel.h>
#include <FluSettingsVersionBox.h>
#include <FluPushButton.h>
#include <FluProgressRing.h> // s17.12：下载进度环（vendor 控件，取色见每主题 SettingsPage.qss）

namespace {

/// 设置卡右侧「修改/清除」钮尺寸（P0 字号缩放）：基值在 LayoutConstants（单源），
/// 这里按启动倍率放大 —— 写死 64×30 时英文文案（Change/Clear）+ 大字号会把按钮文字
/// 截掉，且截断处没有省略号可提示。
QSize cardActionButtonSize()
{
    return QSize(UiScale::scaled(LayoutConst::kCardActionWidth),
                 UiScale::scaled(LayoutConst::kCardActionHeight));
}

/// 版本更新卡「检查更新 / 下载并更新」两枚钮的尺寸：高与其余六卡同值（行高不能变），
/// 宽走 kCardUpdateActionWidth —— 那两枚的文案是 4-5 字，64 档会裁字。
QSize cardUpdateActionButtonSize()
{
    return QSize(UiScale::scaled(LayoutConst::kCardUpdateActionWidth),
                 UiScale::scaled(LayoutConst::kCardActionHeight));
}

/// 写配置并就地提示失败（P1）：AppSettings::setValue 返回是否成功落盘，此前 7 个
/// 调用点全部丢弃返回值 —— config.json 只读/被占用/磁盘满时，卡片值区会回读成旧值，
/// 用户只看到「改了没反应」，没有任何错误说明（语言项还会接着弹「重启生效」的假承诺）。
/// anchor 必须非空：FluMessageBox 的约定是 parent 不可为 null（各调用点传 window()）。
bool applySetting(const QString &key, const QVariant &value, QWidget *anchor)
{
    if (AppSettings::setValue(key, value))
        return true;
    FluMessageBox(QObject::tr("保存失败"),
                  QObject::tr("配置无法写入 config.json，本次修改不会保留。"
                              "请检查文件是否只读或被其它程序占用。"),
                  anchor)
        .exec();
    return false;
}

// 默认工作目录：config.json 键 app/defaultWorkDir（AppSettings 单源，存储根 <exe 目录>/../config）；
// 与主题/语言配置互不影响（语言权威存储见 I18n.cpp，主题走 FluentUI themeChanged）。
const QString kDefaultWorkDirKey = QStringLiteral("defaultWorkDir");

QString readDefaultWorkDir()
{
    return AppSettings::value(kDefaultWorkDirKey).toString();
}

// 未设置时的占位文案（现占用卡片标题下方的说明位 m_infoLabel；
// 与已存路径同源同格，二者互斥显示，长文本按当前宽度中间省略、ToolTip 兜底全量）
QString workDirDisplayText(const QString &stored)
{
    return stored.isEmpty() ? QObject::tr("未设置（使用进程当前目录）") : stored;
}

// 模型服务：s18.3 起权威存储是 config.json 键 app/providers（整表），下面三个旧键降级为
// **兼容镜像 + 回落值**（见 mirrorPrimaryToLegacyKeys）：QOpenAi::initFromSettings 仍读同源，
// 未配 providers 的老配置照旧可用。token 明文存配置文件属用户裁决：存储根下的本机文件，
// 属主可见可改（原环境变量方案已废弃）。
//
// 此处原有 readApiBaseUrl / readApiToken / readModelOptionsRaw / normalizeModelOptions /
// isHttpBaseUrl 五个助手，已随本批改造删除：读写改走 ModelServiceTarget（按 provider），
// 基址合法性改走 ProviderSpec::isValidBaseUrl（与拉取端点拼接同一把尺子）。
// 留着它们不会报错但会触发 /W4 的 C4505（匿名命名空间函数零调用者），故一并清掉。
const QString kApiBaseUrlKey = QStringLiteral("apiBaseUrl");
const QString kApiTokenKey = QStringLiteral("apiToken");
const QString kModelOptionsKey = QStringLiteral("modelOptions");

// API Key 值区脱敏摘要：长值取前 4 + 星 + 后 4；短值（<=8）一律固定四星——
// 否则前后各 4 会覆盖整个 Key，等于把明文摆上界面
QString maskedApiToken(const QString &stored)
{
    if (stored.length() <= 8)
        return QStringLiteral("****");
    return stored.left(4) + QStringLiteral("****") + stored.right(4);
}

/// s18.3 多 Provider：把整表落盘后**镜像首条回旧三键**。
///
/// 为什么必须镜像而不是只写 app/providers：旧三键是 loadProviders() 的回落路径
/// （providers 缺失或删空时合成那条 id="legacy" 的 provider）。只写新键而把旧键留在
/// 盘上，它们就成了陈旧副本——用户日后删空 providers 表，回落路径会把**改造前的老端点和
/// 老 key** 悄悄复活，表现是「明明删干净了却又连上老服务」。镜像之后回落值恒等于当前
/// 首条，两条路径给同一个答案。
///
/// 权威关系是单向的：providers 为准，旧三键只是兼容镜像与回落值，读侧永远先看新键。
/// 写三条各一次而非整表覆盖：AppSettings 无 remove()（见 AppSettings.h 顶部对
/// 「传 invalid QVariant 删键」的警告），逐键 setValue 是唯一安全写法。
void mirrorPrimaryToLegacyKeys(const QList<ProviderSpec::Provider> &providers, QWidget *anchor)
{
    const ProviderSpec::Provider primary =
        providers.isEmpty() ? ProviderSpec::Provider{} : providers.first();
    applySetting(kApiBaseUrlKey, primary.baseUrl, anchor);
    applySetting(kApiTokenKey, primary.token, anchor);
    applySetting(kModelOptionsKey, primary.models.join(QLatin1Char(',')), anchor);
}

} // namespace

// ============================================================================
// ModelServiceTarget（s18.3）：模型服务组四张卡共享的「当前编辑哪家 provider」
// ============================================================================

ModelServiceTarget::ModelServiceTarget(QObject *parent) : QObject(parent)
{
}

void ModelServiceTarget::setProviderId(const QString &id)
{
    if (m_providerId == id)
        return; // 同值早退：不发信号，避免四张卡无谓重画（下拉重填还会抢焦点）
    m_providerId = id;
    emit targetChanged();
}

QList<ProviderSpec::Provider> ModelServiceTarget::providers() const
{
    return ProviderSpec::loadProviders();
}

int ModelServiceTarget::currentIndex() const
{
    const QList<ProviderSpec::Provider> list = providers();
    const int at = ProviderSpec::indexOfId(list, m_providerId);
    // 未选中（含 id 指向已被删掉的 provider）一律落到首条：四张卡必须恒有编辑目标，
    // 否则「当前 provider 为空」会让地址/密钥卡显示空值、用户以为配置被清了。
    return at >= 0 ? at : (list.isEmpty() ? -1 : 0);
}

ProviderSpec::Provider ModelServiceTarget::current() const
{
    // providers() 只取一次：原写法先调 currentIndex()（其内部已 providers() → loadProviders()
    // 重建整张表），再 providers() 又建一遍——一次 current() = 两次整表重建（含全部模型名拷贝
    // + normalizeModelName + 逐条 contains 查重）。SettingsPage 构造期本函数被调约 10-16 次。
    // 下标判据与 currentIndex() 逐值一致：未选中/指向已删 provider → 落首条，空表 → -1。
    const QList<ProviderSpec::Provider> list = providers();
    const int at = ProviderSpec::indexOfId(list, m_providerId);
    const int index = at >= 0 ? at : (list.isEmpty() ? -1 : 0);
    if (index < 0)
        return ProviderSpec::Provider{};
    return list.at(index);
}

bool ModelServiceTarget::commit(const QList<ProviderSpec::Provider> &next, QWidget *anchor)
{
    if (!ProviderSpec::saveProviders(next))
    {
        FluMessageBox(tr("保存失败"),
                      tr("配置无法写入 config.json，本次修改不会保留。"
                         "请检查文件是否只读或被其它程序占用。"),
                      anchor)
            .exec();
        return false;
    }
    mirrorPrimaryToLegacyKeys(next, anchor);
    QOpenAi::reloadProviders(); // 热生效：下一回合请求即按新表路由凭据，无需重启
    emit targetChanged();
    return true;
}

namespace {

/// 以「改当前 provider 的一个字段」的形态提交整表。四张卡共用，避免各写一套
/// load→改→upsert→commit 而漏掉撞名校验或镜像回落。返回 false = 已弹错，调用方只需放弃刷新。
/// 把 upsertProvider/addModelToProvider 回抛的机器码翻成用户可读文案。
/// 判据串与 ProviderSpec.h 的返回值逐字对应，两处必须同步改——故集中在这一个自由函数里，
/// 四张卡不再各自拼一套（各自拼就会有人漏掉 providers-cap 这种少见分支）。
/// 用 QObject::tr 而非裸 tr：本函数是匿名命名空间里的自由函数，没有 Q_OBJECT 上下文，
/// 裸 tr() 编译不过。
QString providerErrorText(const QString &error)
{
    if (error == QLatin1String("invalid-base-url"))
        return QObject::tr("服务地址须以 http:// 或 https:// 开头。");
    if (error.startsWith(QLatin1String("duplicate-model:")))
        return QObject::tr("以下模型名已被其它服务商占用：%1")
            .arg(error.mid(QLatin1String("duplicate-model:").size())
                     .replace(QLatin1Char(','), QStringLiteral("、")));
    if (error == QLatin1String("providers-cap"))
        return QObject::tr("服务商数量已达上限。");
    if (error == QLatin1String("models-cap"))
        return QObject::tr("本服务商模型数已达上限。");
    if (error == QLatin1String("empty-model-name"))
        return QObject::tr("模型名不能为空。");
    if (error == QLatin1String("unknown-provider"))
        return QObject::tr("目标服务商已不存在，请重新选择。");
    return QObject::tr("配置被拒绝写入。");
}

bool commitEditedCurrent(ModelServiceTarget *target, const ProviderSpec::Provider &edited,
                         QWidget *anchor)
{
    QString error;
    const QList<ProviderSpec::Provider> next =
        ProviderSpec::upsertProvider(target->providers(), edited, error);
    if (!error.isEmpty())
    {
        // 文案必须把冲突项念出来——只说「保存失败」，用户无从知道是哪个模型名被别家占了。
        FluMessageBox(QObject::tr("无法保存"), providerErrorText(error), anchor).exec();
        return false;
    }
    return target->commit(next, anchor);
}

} // namespace

// ============================================================================
// ProviderSettingCard（s18.3）：服务商选择 + 新增 + 删除
// ============================================================================

ProviderSettingCard::ProviderSettingCard(ModelServiceTarget *target, QWidget *parent)
    : InfoSlotSettingCard(parent), m_target(target)
{
    setTitleInfo(tr("服务商"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::ReadingMode);  // 多家服务并列语义（Link/Lock/ReadingList 已用于同组三卡）

    // 本卡是四张卡里唯一**用基类自带下拉**的：其余三卡都 getComboBox()->hide() 换自定义
    // 操作行，而这里要的正是「选一家」的交互，复用 vendor 控件即天然随主题着色。
    m_addButton = new FluPushButton(tr("新增"), this);
    m_addButton->setFixedSize(cardActionButtonSize());
    m_removeButton = new FluPushButton(tr("删除"), this);
    m_removeButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_addButton);
    row->addWidget(m_removeButton);
    m_mainLayout->addLayout(row, 0);

    refreshCombo();

    connect(getComboBox(), &FluComboBox::currentTextChanged, this, [this](const QString &) {
        if (m_repopulating)
            return; // 重填下拉触发的 currentTextChanged 不是用户意图，回环会把选中态打回首条
        const int at = getComboBox()->currentIndex();
        const QList<ProviderSpec::Provider> list = m_target->providers();
        if (at >= 0 && at < list.size())
            m_target->setProviderId(list.at(at).id);
    });
    connect(m_addButton, &QPushButton::clicked, this, [this]() { promptAdd(); });
    connect(m_removeButton, &QPushButton::clicked, this, [this]() { promptRemove(); });
    // 别处改了表（新增/删除/改名）也要反映到本卡下拉
    connect(m_target, &ModelServiceTarget::targetChanged, this, [this]() { refreshCombo(); });
}

void ProviderSettingCard::retranslate()
{
    setTitleInfo(tr("服务商"), QString());
    m_addButton->setText(tr("新增"));
    m_removeButton->setText(tr("删除"));
    refreshCombo();
}

void ProviderSettingCard::refreshCombo()
{
    const QList<ProviderSpec::Provider> list = m_target->providers();
    m_repopulating = true;
    getComboBox()->blockSignals(true);
    getComboBox()->clear();
    QStringList names;
    for (const ProviderSpec::Provider &p : list)
        names << (p.name.isEmpty() ? p.id : p.name);
    getComboBox()->addItems(names);
    const int at = m_target->currentIndex();
    if (at >= 0)
        getComboBox()->setCurrentIndex(at);
    getComboBox()->blockSignals(false);
    m_repopulating = false;

    // 只剩一条时禁删：删空会让 loadProviders() 回落到旧三键镜像，而镜像正是这条本身——
    // 表现为「删了又自己长回来」。与其让用户困惑，不如在入口就拦住并说明原因。
    m_removeButton->setEnabled(list.size() > 1);
    m_removeButton->setToolTip(list.size() > 1 ? QString()
                                               : tr("至少需保留一个服务商。"));

    const ProviderSpec::Provider cur = m_target->current();
    setInfoValue(tr("当前编辑"),
                 list.isEmpty() ? tr("无") : (cur.name.isEmpty() ? cur.id : cur.name),
                 tr("共 %1 个服务商").arg(list.size()));
}

void ProviderSettingCard::promptAdd()
{
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("新增服务商"), tr("给这个服务商起个名字，如 DeepSeek。"), QString());
    if (!accepted)
        return;
    ProviderSpec::Provider fresh;
    fresh.name = ProviderSpec::normalizeProviderName(input);
    // 新条目先建空壳（无端点无模型）：允许「先建档、再填地址」，与旧三键「空串=未配置」
    // 口径一致。真正的合法性在填地址那一刻由 commitEditedCurrent 校验。
    if (!commitEditedCurrent(m_target, fresh, window()))
        return;
    // 建完即切到新条目，否则用户接着填地址时改的还是老那条——那是最容易写错地方的一步
    const QList<ProviderSpec::Provider> after = m_target->providers();
    if (!after.isEmpty())
        m_target->setProviderId(after.last().id);
}

void ProviderSettingCard::promptRemove()
{
    const ProviderSpec::Provider cur = m_target->current();
    if (m_target->providers().size() <= 1)
        return; // 按钮已禁，双保险
    const QString label = cur.name.isEmpty() ? cur.id : cur.name;
    if (FluMessageBox(tr("删除服务商"),
                      tr("确定删除「%1」？其端点、密钥与模型清单都会移除。").arg(label),
                      window())
            .exec()
        != QDialog::Accepted)
        return;
    const QList<ProviderSpec::Provider> next = ProviderSpec::removeProvider(m_target->providers(), cur.id);
    if (!m_target->commit(next, window()))
        return;
    m_target->setProviderId(next.isEmpty() ? QString() : next.first().id);
}

// 注：上面 commitEditedCurrent 所在的匿名 namespace 已闭合。闭合并**不会**让它对后续代码
// 失去可见性（同一 TU 内匿名命名空间成员在此后仍然可用），故此处不再重开一个
// `namespace {`——重开而没有配对的 `}` 会让整个文件命名空间不闭合。

// 说明位值卡基类：把「简短解释：值」渲染进 FluSettingsSelectBox 的 m_infoLabel
// （标题下方那一格），右侧操作行只留按钮。六张设置卡共用，避免各写一份省略逻辑。
// 压缩口径沿用 WorkDirPathBar / 原 WorkDirSettingCard：minimumWidth(0) + Expanding +
// ElideMiddle + ToolTip 全量 + Resize 重算。QLabel 不会自动省略，不设 minimumWidth(0)
// 时长值 sizeHint 会把右侧操作按钮挤出卡片（默认 Preferred 虽含 ShrinkFlag，仍受
// minimumSizeHint 顶住，窄宽下顶不住）。
InfoSlotSettingCard::InfoSlotSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    m_infoLabel->setTextFormat(Qt::PlainText); // 值按纯文本处理，避免被当作富文本解析
    m_infoLabel->setMinimumWidth(0);
    m_infoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_infoLabel->installEventFilter(this); // 说明位宽度变化时重算中间省略
}

void InfoSlotSettingCard::setInfoValue(const QString &prefix, const QString &value,
                                       const QString &toolTip)
{
    m_prefix = prefix;
    m_value = value;
    m_toolTip = toolTip;
    refreshInfoSlot();
}

// **只对值部分省略，前缀恒完整**：若对「前缀：值」整串做 ElideMiddle，省略点会落在前缀
// 与值的交界上，等于同时毁掉说明和值。宽度未知（首帧前 w==0）时先放全文，Resize 到达即重算。
void InfoSlotSettingCard::refreshInfoSlot()
{
    // 分隔符走 tr()：中文全角「：」，英文回落半角 ": "，避免英文界面出现 CJK 宽标点
    const QString head = m_prefix.isEmpty() ? QString() : m_prefix + tr("：");
    m_infoLabel->setToolTip(m_toolTip.isEmpty() ? head + m_value : m_toolTip);

    const int w = m_infoLabel->width();
    if (w <= 0)
    {
        m_infoLabel->setText(head + m_value);
        return;
    }
    const QFontMetrics fm(m_infoLabel->font());
    const int avail = w - fm.horizontalAdvance(head);
    // avail<=0 表示窄到连前缀都放不下：退化为整串省略，至少不丢 ToolTip 全量
    m_infoLabel->setText(avail > 0
                             ? head + fm.elidedText(m_value, Qt::ElideMiddle, avail)
                             : fm.elidedText(head + m_value, Qt::ElideMiddle, w));
}

bool InfoSlotSettingCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_infoLabel && event->type() == QEvent::Resize)
        refreshInfoSlot(); // 卡片/页面变宽变窄时重算省略，避免长值把操作按钮挤出可视区
    return FluSettingsSelectBox::eventFilter(watched, event);
}

// 默认工作目录设置卡：说明位展示当前路径，**无解释前缀**（原「新建会话将继承该工作目录。」
// 提示文案按需求移除）。右侧操作行只留「修改 + 清除」。
// 该格样式本就是 12px 次要灰（FluSettingsSelectBox.qss #infoLabel），与 WorkDirPathBar 的
// 路径字号一致，视觉重量天然匹配、无需另立样式。
// 类声明在 SettingsPage.h（Q_OBJECT 上下文对齐译词条，注释见彼处）。
WorkDirSettingCard::WorkDirSettingCard(QWidget *parent)
    : InfoSlotSettingCard(parent)
{
    setTitleInfo(tr("默认工作目录"), QString()); // 说明位留空，值由 updateValue 接管
    setIcon(FluAwesomeType::Folder);
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(cardActionButtonSize());
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() {
        const QString current = readDefaultWorkDir();
        const QString startDir = (!current.isEmpty() && QFileInfo(current).isDir())
                                     ? current
                                     : QDir::currentPath();
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择默认工作目录"), startDir);
        if (dir.isEmpty())
            return; // 取消：保持原值
        applySetting(kDefaultWorkDirKey, dir, window());
        updateValue();
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        applySetting(kDefaultWorkDirKey, QString(), window());
        updateValue();
    });
}

void WorkDirSettingCard::retranslate()
{
    // 说明位已让给路径，不再回填提示文案；标题重取 tr()，占位文案随 updateValue 重算
    setTitleInfo(tr("默认工作目录"), QString());
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void WorkDirSettingCard::updateValue()
{
    const QString stored = readDefaultWorkDir();
    // 无前缀：可见文本是路径或「未设置」占位文案；ToolTip 用原始全量路径（未设置=空串，
    // 此时基类回落为占位文案本身，无泄密面）
    setInfoValue(QString(), workDirDisplayText(stored), stored);
}

// 上下文上限设置卡（第九轮；s17.9 起单位定为 token）：说明位「超限自动压缩：200,000 token」，
// 右侧只留「修改」。展示/回写均经 AgentConst::contextCharLimitValue() 单点取值
// （未设置/非法自动回退默认 200000），与 CompactManager 消费侧同源不分叉；
// 写 config.json 后 CompactManager 下一回合管线现取即生效，无需重启。
// 单位是 token 且**声明值即生效窗口**（不再有 ÷4 的二次换算，也没有「≈N token」派生括号），
// 所以这里显示的数 == 侧栏占用分母 == 压缩门槛基准，三处同一个数。
ContextLimitSettingCard::ContextLimitSettingCard(QWidget *parent)
    : InfoSlotSettingCard(parent)
{
    setTitleInfo(tr("上下文上限（token）"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::Trim); // 裁减语义
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_modifyButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
}

void ContextLimitSettingCard::retranslate()
{
    setTitleInfo(tr("上下文上限（token）"), QString());
    m_modifyButton->setText(tr("修改"));
    updateValue();
}

void ContextLimitSettingCard::updateValue()
{
    // 千分位展示（c-locale 固定逗号分组，不随界面语言变）；编辑预填用裸数字防解析歧义。
    // s17.9（丁-3）：单位是 token，且**声明值即生效窗口**——不再有 ÷4 的二次换算，也删掉了
    // 原先「≈N token」派生括号（那个括号就是 ÷4 的化身，留着它等于在文案里给一个已经不
    // 存在的换算背书）。这里显示的数 == 侧栏占用分母 == 压缩门槛基准，三处同一个数。
    const QLocale loc(QLocale::c());
    setInfoValue(tr("超限自动压缩"),
                 tr("%1 token")
                     .arg(loc.toString(AgentConst::contextCharLimitValue())));
}

void ContextLimitSettingCard::promptEdit()
{
    const QString rangeHint =
        tr("范围 %1 ~ %2（token）。")
            .arg(AgentConst::kContextCharLimitMin)
            .arg(AgentConst::kContextCharLimitMax);
    // InputDialog 约定 parent 传主窗口（遮罩铺满）；预填当前生效值（裸数字）
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("设置上下文上限"), rangeHint,
        QString::number(AgentConst::contextCharLimitValue()));
    if (!accepted)
        return; // 取消：保持原值
    // 容忍千分位输入（与展示格式对称）；toLongLong 对残留非数字判 ok=false
    QString cleaned = input.trimmed();
    cleaned.remove(QLatin1Char(','));
    bool ok = false;
    const qlonglong parsed = cleaned.toLongLong(&ok);
    if (!ok || parsed < AgentConst::kContextCharLimitMin ||
        parsed > AgentConst::kContextCharLimitMax)
    {
        // 非法值拒绝并提示，不落盘（CompactManager 侧兜底回退默认，但设置页不写脏值）
        FluMessageBox(tr("无效数值"),
                      tr("请输入 %1 ~ %2 之间的整数。")
                          .arg(AgentConst::kContextCharLimitMin)
                          .arg(AgentConst::kContextCharLimitMax),
                      window())
            .exec();
        return;
    }
    applySetting(AgentConst::kContextCharLimitKey, parsed, window());
    updateValue();
}

// 单轮最大调用次数设置卡（第十二轮）：与上下文上限卡同款结构——说明位
// 「超限即终止循环：500」+「修改」。展示/回写均经 AgentConst::maxToolIterationsValue()
// 单点取值（未设置/非法自动回退默认 500），与 AgentLoop 回合入口快照同源不分叉；
// 写 config.json 后下一回合生效。数值域 [10,1000] 无需千分位，直接裸整数展示。
// 前缀「超限即终止循环」非拟测：撞上限走 AgentLoopRequest.cpp 的
// error(「工具调用轮次超过上限（N 轮），终止循环。」) 分支。
MaxRoundsSettingCard::MaxRoundsSettingCard(QWidget *parent)
    : InfoSlotSettingCard(parent)
{
    setTitleInfo(tr("单轮最大调用次数"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::Calculator); // 计数语义（todo 卡同款图标族，轮次即计数）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_modifyButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
}

void MaxRoundsSettingCard::retranslate()
{
    setTitleInfo(tr("单轮最大调用次数"), QString());
    m_modifyButton->setText(tr("修改"));
    updateValue();
}

void MaxRoundsSettingCard::updateValue()
{
    setInfoValue(tr("超限即终止循环"),
                 QString::number(AgentConst::maxToolIterationsValue()));
}

void MaxRoundsSettingCard::promptEdit()
{
    const QString rangeHint =
        tr("范围 %1 ~ %2（轮）。")
            .arg(AgentConst::kMaxToolIterationsMin)
            .arg(AgentConst::kMaxToolIterationsMax);
    // InputDialog 约定 parent 传主窗口（遮罩铺满）；预填当前生效值
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("设置最大轮次"), rangeHint,
        QString::number(AgentConst::maxToolIterationsValue()));
    if (!accepted)
        return; // 取消：保持原值
    // toLongLong 对非数字判 ok=false；越界拒写（AgentLoop 侧兜底回退默认，
    // 但设置页不写脏值）
    const QString cleaned = input.trimmed();
    bool ok = false;
    const qlonglong parsed = cleaned.toLongLong(&ok);
    if (!ok || parsed < AgentConst::kMaxToolIterationsMin ||
        parsed > AgentConst::kMaxToolIterationsMax)
    {
        FluMessageBox(tr("无效数值"),
                      tr("请输入 %1 ~ %2 之间的整数。")
                          .arg(AgentConst::kMaxToolIterationsMin)
                          .arg(AgentConst::kMaxToolIterationsMax),
                      window())
            .exec();
        return;
    }
    applySetting(AgentConst::kMaxToolIterationsKey, parsed, window());
    updateValue();
}

// 服务地址设置卡：说明位「OpenAI 兼容基址：<url>」+「修改 + 清除」操作行。
// 编辑走 InputDialog 单行输入（parent 传主窗口保遮罩铺满），预填存量值；
// 校验不过弹 FluMessageBox 拒写，空串按清除处理。URL 非机密，明文展示、ToolTip 回落全量。
// s18.3：本卡读写的是 **target 当前选中 provider** 的 baseUrl，不再是全局单键；
// 合法性判据也统一走 ProviderSpec::isValidBaseUrl（与拉取端点拼接同一把尺子）。
ApiUrlSettingCard::ApiUrlSettingCard(ModelServiceTarget *target, QWidget *parent)
    : InfoSlotSettingCard(parent), m_target(target)
{
    setTitleInfo(tr("服务地址"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::Link); // 端点链接语义（Globe 已被语言卡占用）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(cardActionButtonSize());
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        ProviderSpec::Provider edited = m_target->current();
        edited.baseUrl.clear();
        commitEditedCurrent(m_target, edited, window());
    });
    // 切了 provider 必须整体重画：否则显示的是甲的地址、写进去落在乙身上
    connect(m_target, &ModelServiceTarget::targetChanged, this, [this]() { updateValue(); });
}

void ApiUrlSettingCard::retranslate()
{
    setTitleInfo(tr("服务地址"), QString());
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ApiUrlSettingCard::updateValue()
{
    const QString stored = m_target->current().baseUrl;
    // ToolTip 传原始全量 URL（长 URL 省略后仍可悬停读全）；未设置时空串，基类回落占位文案
    setInfoValue(tr("OpenAI 兼容基址"), stored.isEmpty() ? tr("未设置") : stored, stored);
}

void ApiUrlSettingCard::promptEdit()
{
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("设置服务地址"), tr("以 /v1 等版本路径结尾，不含补全端点。"),
        m_target->current().baseUrl);
    if (!accepted)
        return; // 取消：保持原值
    const QString cleaned = input.trimmed();
    if (!cleaned.isEmpty() && !ProviderSpec::isValidBaseUrl(cleaned))
    {
        // 非法 URL 拒绝并提示，不落盘（原值继续生效）
        FluMessageBox(tr("无效地址"),
                      tr("请输入以 http:// 或 https:// 开头的完整地址。"),
                      window())
            .exec();
        return;
    }
    ProviderSpec::Provider edited = m_target->current();
    edited.baseUrl = cleaned;
    commitEditedCurrent(m_target, edited, window()); // 内部已 reloadProviders，热生效无需重启
}

// API Key 设置卡：结构与 URL 卡同款，差异全在「机密值」处理——说明位只显脱敏摘要，
// **明文绝不上屏也不进 ToolTip**（基类会把 value 原样写进 ToolTip，故进说明位的必须
// 已是 maskedApiToken 摘要，绝不能把 stored 明文交给基类）。
// 编辑框不预填存量明文 + Password 回显；清除另有「清除」按钮，职责不重叠。
// s18.3：读写 target 当前 provider 的 token。掩码纪律一字未动，只是换了存储位置。
ApiTokenSettingCard::ApiTokenSettingCard(ModelServiceTarget *target, QWidget *parent)
    : InfoSlotSettingCard(parent), m_target(target)
{
    setTitleInfo(tr("API Key"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::Lock); // 凭据语义（枚举表无 Key 项）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(cardActionButtonSize());
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        ProviderSpec::Provider edited = m_target->current();
        edited.token.clear();
        commitEditedCurrent(m_target, edited, window());
    });
    connect(m_target, &ModelServiceTarget::targetChanged, this, [this]() { updateValue(); });
}

void ApiTokenSettingCard::retranslate()
{
    setTitleInfo(tr("API Key"), QString());
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ApiTokenSettingCard::updateValue()
{
    const QString stored = m_target->current().token;
    // 安全收口：value 传脱敏摘要（非 stored），toolTip 显式传状态串而非全量值——
    // 否则基类的「回落 prefix+value」会把摘要写进悬停提示，虽仍非明文，但「已保存」
    // 的既有语义更贴合凭据卡（不重复罗列星号串）。
    setInfoValue(tr("Bearer 凭据"), stored.isEmpty() ? tr("未设置") : maskedApiToken(stored),
                 stored.isEmpty() ? QString() : tr("已保存"));
}

void ApiTokenSettingCard::promptEdit()
{
    // 安全收口：此前把已存 token **明文预填**进编辑框，而全仓无一处 setEchoMode——与同文件
    // updateValue() 里「明文绝不上屏」的注释自相矛盾（值区脱敏了，点「修改」反而把完整 key
    // 摊在截屏/肩窥里）。现改为不预填 + Password 回显。
    // 语义随之调整：空输入＝不修改。否则「不预填」会让「打开就直接点确定」变成误删 token；
    // 清除另有卡上的「清除」按钮（见构造函数 m_clearButton 的连接），职责不重叠。
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("设置 API Key"),
        tr("输入新的 Key 并保存；留空表示不修改（清除请用「清除」按钮）。"),
        QString(), QLineEdit::EchoMode::Password);
    if (!accepted)
        return; // 取消：保持原值
    const QString cleaned = input.trimmed();
    if (cleaned.isEmpty())
        return; // 留空＝不修改
    ProviderSpec::Provider edited = m_target->current();
    edited.token = cleaned;
    commitEditedCurrent(m_target, edited, window());
}

// 可选模型清单设置卡：说明位「下拉候选：<清单>」+「管理 + 清除」操作行。
// s18.3 改造要点（本次「添加模型不方便」的主修对象）：
//   旧形态是「修改」弹一个输入框、把整份清单当**一条逗号文本**让用户整体重打。三个后果：
//     ① 加一个模型要重打全部已有模型；② 预填取的是原始存储值，而值区显示的是清洗后的
//        生效清单——未配置时弹窗是**空的**，用户补一个新名字就把内置两项静默抹掉；
//     ③ 模型名必须去服务商控制台手抄，拼错要到发消息时才发现。
//   现改为「管理」弹菜单：添加一个 / 从服务端拉取 / 逐项移除 / 清空，逐项操作、不碰整串。
// 说明位仍展示**生效清单**（所见即下拉所得）。清单来源改为 target 当前 provider。
// 生效路径不变：本卡只写 config.json，不持有也不引用 ChatMsgEdit（两控件解耦）；输入框
// 下拉在每次弹层展开前重读清单并原地刷新，故改完配置无需重启即可选到新模型。
ModelListSettingCard::ModelListSettingCard(ModelServiceTarget *target, QWidget *parent)
    : InfoSlotSettingCard(parent), m_target(target)
{
    setTitleInfo(tr("可选模型"), QString()); // 说明位归值管
    setIcon(FluAwesomeType::ReadingList); // 清单语义（Link / Lock 已用于同组两卡）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_manageButton = new FluPushButton(tr("管理"), this);
    m_manageButton->setFixedSize(cardActionButtonSize());
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(cardActionButtonSize());

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_manageButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

    updateValue();

    connect(m_manageButton, &QPushButton::clicked, this, [this]() { promptManage(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        ProviderSpec::Provider edited = m_target->current();
        edited.models.clear();
        commitEditedCurrent(m_target, edited, window());
    });
    connect(m_target, &ModelServiceTarget::targetChanged, this, [this]() { updateValue(); });
}

void ModelListSettingCard::retranslate()
{
    setTitleInfo(tr("可选模型"), QString());
    m_manageButton->setText(tr("管理"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ModelListSettingCard::updateValue()
{
    const QStringList mine = m_target->current().models;
    if (mine.isEmpty())
    {
        // 本 provider 一个模型都没挂：下拉此刻用的是别家或内置兜底，如实标注来源，
        // 不再像旧版那样把「未配置」与「配置了但为空」混成一句。
        setInfoValue(tr("本服务商暂无模型"),
                     AgentConst::modelOptions().join(QStringLiteral(", ")),
                     tr("用「管理」添加或从服务端拉取。未添加前，下拉显示的是其它服务商或内置候选。"));
        return;
    }
    setInfoValue(tr("下拉候选"), mine.join(QStringLiteral(", ")),
                 tr("共 %1 项（本服务商）").arg(mine.size()));
}

void ModelListSettingCard::promptManage()
{
    const ProviderSpec::Provider cur = m_target->current();

    FluPMenu menu(window());
    menu.setTitle(tr("可选模型"));
    auto *addItem = menu.addAction(tr("添加模型…"));
    auto *fetchItem = menu.addAction(m_fetching ? tr("正在拉取…") : tr("从服务端拉取…"));
    fetchItem->setEnabled(!m_fetching); // 在途期间禁掉，防连点后到的响应覆盖先到的
    QAction *clearItem = nullptr;
    QList<QAction *> removeItems;
    if (!cur.models.isEmpty())
    {
        menu.addSeparator();
        for (const QString &m : cur.models)
            removeItems.append(menu.addAction(tr("移除 %1").arg(m)));
        menu.addSeparator();
        clearItem = menu.addAction(tr("清空本服务商模型清单"));
    }

    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;
    if (chosen == addItem)
    {
        promptAddOne();
        return;
    }
    if (chosen == fetchItem)
    {
        fetchFromServer();
        return;
    }
    if (clearItem && chosen == clearItem)
    {
        ProviderSpec::Provider edited = cur;
        edited.models.clear();
        commitEditedCurrent(m_target, edited, window());
        return;
    }
    for (int i = 0; i < removeItems.size(); ++i)
    {
        if (chosen != removeItems.at(i))
            continue;
        const QList<ProviderSpec::Provider> next =
            ProviderSpec::removeModelFromProvider(m_target->providers(), cur.id, cur.models.at(i));
        m_target->commit(next, window());
        return;
    }
}

void ModelListSettingCard::promptAddOne()
{
    // 只录**一个**模型名——这是本次改造的核心取舍：录入单位与存储单位对齐后，
    // 用户不必为了加一项而重打整张清单，也不可能因为漏个逗号抹掉别项。
    const auto [input, accepted] = InputDialog::getInputText(
        window(), tr("添加模型"),
        tr("输入模型 ID（大小写敏感，须与服务端一致），如 qwen3.8-flash。"), QString());
    if (!accepted)
        return;
    const QString name = ProviderSpec::normalizeModelName(input);
    if (m_target->current().models.contains(name))
    {
        // addModelToProvider 对「本家已有」是幂等静默（返回原表、error 空），
        // 但界面上什么都不说会被当成「点了没反应」，这里补一句如实反馈。
        FluMessageBox(tr("无需添加"), tr("该模型已在本服务商清单中。"), window()).exec();
        return;
    }
    QString error;
    const QList<ProviderSpec::Provider> next =
        ProviderSpec::addModelToProvider(m_target->providers(), m_target->current().id, input, error);
    if (!error.isEmpty())
    {
        // 文案单源 providerErrorText（与地址/密钥卡同一套机器码→文案映射，两处不各拼一份）
        FluMessageBox(tr("无法添加"), providerErrorText(error), window()).exec();
        return;
    }
    m_target->commit(next, window());
}

void ModelListSettingCard::fetchFromServer()
{
    const ProviderSpec::Provider cur = m_target->current();
    if (cur.baseUrl.isEmpty())
    {
        FluMessageBox(tr("无法拉取"), tr("请先为本服务商填写服务地址。"), window()).exec();
        return;
    }
    m_fetching = true;
    m_manageButton->setEnabled(false); // 在途期间锁住入口，回来再解
    const QString providerId = cur.id;
    ModelFetch::Request::fetch(
        cur.baseUrl, cur.token, this,
        [this, providerId](const QStringList &models, const QString &error) {
            m_fetching = false;
            m_manageButton->setEnabled(true);
            if (!error.isEmpty())
            {
                // 拉不到不阻断：手填通路始终可用，这是设计前提而非兜底
                FluMessageBox(tr("拉取失败"),
                              error + tr("仍可用「添加模型」手动填写。"), window())
                    .exec();
                return;
            }
            const QList<ProviderSpec::Provider> table = m_target->providers();
            const int at = ProviderSpec::indexOfId(table, providerId);
            if (at < 0)
                return; // 响应回来时那条 provider 已被删：丢弃，不往已消失的目标上写
            const QStringList added =
                ProviderSpec::mergeableModels(table.at(at), models, table);
            if (added.isEmpty())
            {
                FluMessageBox(tr("拉取完成"),
                              tr("服务端返回 %1 个模型，均已在本服务商清单中。").arg(models.size()),
                              window())
                    .exec();
                return;
            }
            // 勾选式增删需要一套带列表的对话框与三主题 QSS，代价远大于收益；这里把
            // 「全部可加的」一次性并入并如实报数，多余项可用「管理 → 移除」逐条摘掉。
            // mergeableModels 已滤掉本家已有与别家占用的名字，故这里不会再触发撞名拒绝。
            QList<ProviderSpec::Provider> next = table;
            ProviderSpec::Provider edited = table.at(at);
            for (const QString &m : added)
            {
                if (edited.models.size() >= ProviderSpec::kModelsPerProviderMax)
                    break;
                edited.models.append(m);
            }
            next[at] = edited;
            m_target->commit(next, window());
        });
}

SettingsPage::SettingsPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    // 页面留白单源（P2-D2）：此处四个裸 35 与 ChatSessionPage 同源——水平即 kSideMargin、
    // 垂直即 kPageVMargin。此前 SettingsPage 不引 LayoutConstants.h，同一个 35 在两头各写
    // 一遍，改页面留白会漏掉本页（表现为设置页与其他页留白不一致）。
    vMainLayout->setContentsMargins(LayoutConst::kSideMargin, LayoutConst::kPageVMargin,
                                    LayoutConst::kSideMargin, LayoutConst::kPageVMargin);
    vMainLayout->setAlignment(Qt::AlignTop);
    setLayout(vMainLayout);

    auto scrollView = new FluVScrollView;
    scrollView->getMainLayout()->setAlignment(Qt::AlignTop);
    vMainLayout->addWidget(scrollView, 1);

    /// apperance&behavior;
    m_appearanceLabel = new FluLabel;
    m_appearanceLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_appearanceLabel->setText(tr("外观与行为"));
    scrollView->getMainLayout()->addWidget(m_appearanceLabel, 0, Qt::AlignTop);


    /// app Theme;
    m_appThemeBox = new FluSettingsSelectBox;
    m_appThemeBox->setTitleInfo(tr("应用主题"), tr("选择应用显示的主题。"));
    m_appThemeBox->setIcon(FluAwesomeType::Color);
    m_appThemeBox->getComboBox()->addItem(tr("浅色"));
    m_appThemeBox->getComboBox()->addItem(tr("深色"));
    m_appThemeBox->getComboBox()->addItem(tr("AtomOneDark")); // 主题专名，各语言恒等
    m_appThemeBox->getComboBox()->setCurrentIndex((int)FluThemeUtils::getUtils()->getTheme());
    // context=this（第十一轮 F5）：随页面析构自动断开，全仓 lambda connect 唯一缺省处
    connect(m_appThemeBox->getComboBox(), &FluComboBox::currentIndexChanged, this, [=](int index) {
        if (index == (int)FluThemeUtils::getUtils()->getTheme())
            return;

        if (index == 0)
            FluThemeUtils::getUtils()->setTheme(FluTheme::Light);
        else if (index == 1)
            FluThemeUtils::getUtils()->setTheme(FluTheme::Dark);
        else
            FluThemeUtils::getUtils()->setTheme(FluTheme::AtomOneDark);
    });
    // 标题栏 FluThemeButton 换主题不经过本下拉 → 订阅 themeChanged 回灌选中项。
    // 缺这条时：从标题栏切到深色后进设置页，下拉仍显示「浅色」，且用户再选「浅色」
    // 会因上面「index == 当前主题」的早退什么都不做，控件看起来失效。
    connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged, this, [this](FluTheme theme) {
        auto *combo = m_appThemeBox ? m_appThemeBox->getComboBox() : nullptr;
        if (!combo || combo->currentIndex() == (int)theme)
            return;
        // 屏蔽回灌触发的 currentIndexChanged：否则会反向再 setTheme 一次（同值无害但多余）
        const bool blocked = combo->blockSignals(true);
        combo->setCurrentIndex((int)theme);
        combo->blockSignals(blocked);
    });

    scrollView->getMainLayout()->addWidget(m_appThemeBox, 0, Qt::AlignTop);

    /// language;（i18n 第八轮：接线改走 I18n 权威存储，原 FluConfigUtils 直写
    /// CWD 相对 config.ini 且无任何消费方，切换完全无效——本控件此前只是假开关）
    m_languageBox = new FluSettingsSelectBox;
    m_languageBox->setTitleInfo(tr("语言"), tr("选择界面显示的语言。"));
    m_languageBox->setIcon(FluAwesomeType::Globe);
    // 语言选项用自名（endonym）且不进 tr()：任何界面语言下均须以本族语呈现，
    // 用户才认得出要切过去的是什么
    m_languageBox->getComboBox()->addItem(QStringLiteral("English"));
    m_languageBox->getComboBox()->addItem(QStringLiteral("简体中文"));
    // 初值同步先于 connect：构造期 setCurrentIndex 不会误触发下方切换流程
    m_languageBox->getComboBox()->setCurrentIndex(I18n::language() == QStringLiteral("en-US") ? 0 : 1);

    connect(m_languageBox->getComboBox(), &FluComboBox::currentIndexChanged, this, [this](int index) {
        const QString lang = (index == 0) ? QStringLiteral("en-US") : QStringLiteral("zh-CN");
        if (lang == I18n::language())
            return; // 回选当前值不动作
        I18n::setLanguage(lang);
        // FluentUI 控件文案构造期定死，无运行中重译 → 重启生效（裁决见 I18n.h）
        FluMessageBox box(tr("语言设置"), tr("语言切换将在重启后生效。是否立即重启？"), this);
        if (box.exec() == QDialog::Accepted)
            I18n::requestRestart(this); // 内部先走主窗口退出守卫；取消则仅存设置不重启
    });

    scrollView->getMainLayout()->addWidget(m_languageBox, 0, Qt::AlignTop);


    /// add spacing;
    scrollView->getMainLayout()->addSpacing(20);

    /// work directory;
    m_workDirLabel = new FluLabel;
    m_workDirLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_workDirLabel->setText(tr("工作目录"));
    scrollView->getMainLayout()->addWidget(m_workDirLabel, 0, Qt::AlignTop);

    m_workDirCard = new WorkDirSettingCard;
    scrollView->getMainLayout()->addWidget(m_workDirCard, 0, Qt::AlignTop);


    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// context（第九轮：上下文压缩主上限可设置，随设置即时生效于压缩管线）
    m_contextLabel = new FluLabel;
    m_contextLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_contextLabel->setText(tr("上下文"));
    scrollView->getMainLayout()->addWidget(m_contextLabel, 0, Qt::AlignTop);

    m_contextCard = new ContextLimitSettingCard;
    scrollView->getMainLayout()->addWidget(m_contextCard, 0, Qt::AlignTop);


    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// max rounds（第十二轮：主循环单回合工具调用轮次上限可设置，下一回合生效）
    m_maxRoundsLabel = new FluLabel;
    m_maxRoundsLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_maxRoundsLabel->setText(tr("最大轮次"));
    scrollView->getMainLayout()->addWidget(m_maxRoundsLabel, 0, Qt::AlignTop);

    m_maxRoundsCard = new MaxRoundsSettingCard;
    scrollView->getMainLayout()->addWidget(m_maxRoundsCard, 0, Qt::AlignTop);


    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// permission（s17.2：工具执行权限审批总开关，落 config.json 键 permissionApproval。
    /// 控件沿用主题/语言盒同款 FluSettingsSelectBox——页内既有的「选择型」范式，零新控件类型）
    m_permissionLabel = new FluLabel;
    m_permissionLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_permissionLabel->setText(tr("权限"));
    scrollView->getMainLayout()->addWidget(m_permissionLabel, 0, Qt::AlignTop);

    m_permissionBox = new FluSettingsSelectBox;
    m_permissionBox->setTitleInfo(tr("工具执行权限审批"), tr("命中询问规则时是否需要你批准。"));
    m_permissionBox->setIcon(FluAwesomeType::Shield); // 权限语义（Lock 已被凭据卡占用）
    m_permissionBox->setToolTip(tr("关闭后工具不再弹审批卡、直接执行；bash 硬拒绝黑名单、workflow 的 deny "
                                   "策略与工作区越界拦截不受影响。改完下一个工具调用即生效，无需重启。"));
    m_permissionBox->getComboBox()->addItem(tr("需要审批"));
    m_permissionBox->getComboBox()->addItem(tr("无需审批"));
    // 初值同步先于 connect：构造期 setCurrentIndex 不误触发写盘（与语言盒同纪律）
    m_permissionBox->getComboBox()->setCurrentIndex(AgentConst::permissionApprovalEnabled() ? 0 : 1);
    connect(m_permissionBox->getComboBox(), &FluComboBox::currentIndexChanged, this, [this](int index) {
        const bool required = (index == 0);
        if (required == AgentConst::permissionApprovalEnabled())
            return; // 回选当前值不动作
        applySetting(AgentConst::kPermissionApprovalKey, required, window());
    });
    scrollView->getMainLayout()->addWidget(m_permissionBox, 0, Qt::AlignTop);

    //// add spacing（s17.2 引入「权限」组时漏了这条——页内其余六个组边界一律 20，此处曾是唯一断层）
    scrollView->getMainLayout()->addSpacing(20);

    /// model service（LLM 服务地址 / API Key 可设置，取代原环境变量；写配置文件后即时覆盖全局配置）
    m_modelLabel = new FluLabel;
    m_modelLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_modelLabel->setText(tr("模型服务"));
    scrollView->getMainLayout()->addWidget(m_modelLabel, 0, Qt::AlignTop);

    // 同组三卡紧邻不加 addSpacing：与「外观与行为」组（主题盒 + 语言盒）既有排布同款
    // s18.3：target 必须先于四张卡构造并作为它们的 parent——四卡持裸指针，且 target 的
    // 生命周期要覆盖整页（页面是主窗口构造期即时 new 的常驻页，不是懒建）。
    m_modelTarget = new ModelServiceTarget(this);

    // 同组四卡紧邻不加 addSpacing：与「外观与行为」组（主题盒 + 语言盒）既有排布同款
    m_providerCard = new ProviderSettingCard(m_modelTarget);
    scrollView->getMainLayout()->addWidget(m_providerCard, 0, Qt::AlignTop);

    m_apiUrlCard = new ApiUrlSettingCard(m_modelTarget);
    scrollView->getMainLayout()->addWidget(m_apiUrlCard, 0, Qt::AlignTop);

    m_apiTokenCard = new ApiTokenSettingCard(m_modelTarget);
    scrollView->getMainLayout()->addWidget(m_apiTokenCard, 0, Qt::AlignTop);

    m_modelListCard = new ModelListSettingCard(m_modelTarget);
    scrollView->getMainLayout()->addWidget(m_modelListCard, 0, Qt::AlignTop);


    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// update（s17.12：自更新——组标题同款「上下文」写法，卡紧随其下）
    // 整块交给 UpdateSettingCard：判据在 UpdateRules.h（可 ctest）、编排在 UpdateManager，
    // 本页只挂标题 + 卡。刻意**不在构造期检查**——本页面由主窗口构造函数即时 new
    // （LiteHarness.cpp），在这里打网络等于每次启动程序都访问一次 GitHub；
    // 自动校准改挂 showEvent（首次可见），见 SettingsPage::showEvent。
    m_updateLabel = new FluLabel;
    m_updateLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_updateLabel->setText(tr("更新"));
    scrollView->getMainLayout()->addWidget(m_updateLabel, 0, Qt::AlignTop);

    m_updateCard = new UpdateSettingCard;
    scrollView->getMainLayout()->addWidget(m_updateCard, 0, Qt::AlignTop);

    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// about
    m_aboutLabel = new FluLabel;
    m_aboutLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_aboutLabel->setText(tr("关于"));
    scrollView->getMainLayout()->addWidget(m_aboutLabel, 0, Qt::AlignTop);

    /// version;
    m_versionBox = new FluSettingsVersionBox;
    m_versionBox->getTitleLabel()->setText(tr("lite-harness")); // 品牌名，豁免翻译（维持原样）
    m_versionBox->getInfoLabel()->setText(tr("@2026 lite harness. 保留所有权利。"));
    // 版本号 = 运行时 applicationVersion（CMake project VERSION 单源，经
    // App.cpp setApplicationVersion 注入），数字豁免翻译；原硬编码
    // "0.0.1" 与 CMake 版本脱节，本案修结；现值 = project VERSION 拼 s 前缀（如 s18.2）
    m_versionBox->getVersionLabel()->setText(QCoreApplication::applicationVersion());

    QIcon appIcon = QIcon(":/res/LiteHarness.ico");
    m_versionBox->getIconLabel()->setPixmap(appIcon.pixmap(QSize(45, 45)));

    m_infoLabel = new FluLabel;
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setLabelStyle(FluLabelStyle::BodyTextBlockStyle);
    m_infoLabel->setText(
        tr("LiteHarness 是一款轻量级的 C++ 编码代理 harness 应用，旨在填补 C++ 生态中 harness 实现的空白。"
           "它作为一个动手学习项目，逐步演示如何使用 Qt 与现代 C++ 从零构建一个 harness。"));
    m_versionBox->addWidget(m_infoLabel);


    scrollView->getMainLayout()->addWidget(m_versionBox, 0, Qt::AlignTop);

    // QSS 首刷 + themeChanged 订阅收敛到 ThemeAware::bind。
    // 修正既有缺陷：原构造不首刷 SettingsPage.qss，须等首次主题切换才生效
    ThemeAware::bind("SettingsPage.qss", this);
}

// ---- s17.12 自更新设置卡 ----
// 分工纪律：本卡只做「令牌 → 文案」与按钮动作。版本比较、路径判定、删谁的决定一律在
// UpdateRules.h（可 ctest）与 UpdateManager（IO 编排）里——写进本卡即失去全部直测覆盖。

UpdateSettingCard::UpdateSettingCard(QWidget *parent)
    : InfoSlotSettingCard(parent)
{
    setTitleInfo(tr("版本更新"), QString()); // 说明位归状态管（同上下文上限卡的「值」语义）
    setIcon(FluAwesomeType::Sync);       // 同步语义；Sync 未被同页其余六卡占用
    getComboBox()->hide();               // 本卡不用下拉，右侧改放自定义操作行

    m_updateManager = new UpdateManager(this);

    m_checkButton = new FluPushButton(tr("检查更新"), this);
    m_checkButton->setFixedSize(cardUpdateActionButtonSize());
    m_applyButton = new FluPushButton(tr("下载并更新"), this);
    m_applyButton->setFixedSize(cardUpdateActionButtonSize());
    m_restartButton = new FluPushButton(tr("重启"), this);
    // 尺寸走 kCardActionWidth=64 那档（两字文案桶，与「修改/清除」同档，实测 naturalW 32）：
    // 160 档是给 4-5 字文案定的，「重启」两字用它会把本卡三枚按钮的行宽推到 524px 挤掉标题区。
    // 已知边界登记：英文「Restart」naturalW≈57，80% 档 scaledSize(64,80)=51 会裁 6px。
    m_restartButton->setFixedSize(cardActionButtonSize());
    m_restartButton->setEnabled(false); // 常态禁用：未下载完成不可点，staged 后由 onStaged 解禁
    m_applyButton->setEnabled(false); // 三枚按钮常驻可见：只用禁用表达状态，不隐藏、不收起

    // 下载进度环。**刻意不用 vendor 默认的 60×60**：那是独立控件页的尺寸，塞进这张
    // 30 高操作行的卡里会把行高撑到 60，与同页其余六卡视觉不齐（本仓反复登记过的那类缺陷）。
    // 尺寸走 LayoutConst::kCardProgressRingSize + UiScale::scaled，与同排按钮同步缩放。
    // 常态隐藏不占位，与「下载并更新」钮同款口径：没在下载时不该有个转圈的东西占着。
    m_progressRing = new FluProgressRing(this);
    const int ringSide = UiScale::scaled(LayoutConst::kCardProgressRingSize);
    m_progressRing->setFixedSize(ringSide, ringSide);
    m_progressRing->setMinMaxValue(0, 100);
    m_progressRing->setShowText(false); // 百分比唯一出口是说明位文案，环只画形状
    // 必须显式关掉透明 track，这是本控件唯一一处「qss 管不到、只能在代码里修」的面：
    // 构造里 m_isTransparentTrack 默认 true，而 paintEvent 是
    // `pen.setColor(m_trackColor); if (m_isTransparentTrack) pen.setColor(Qt::transparent);`
    // ——不关的话 vendor 三主题 qss 里的 qproperty-trackColor 全是死代码，0% 时整环
    // 只剩一段弧、用户看不出「还剩多少」，进度指示的参照物没了。
    // 该成员**没有 Q_PROPERTY**（头里只声明了 animValue/workAngle/三色 + trackColor），
    // 所以 qproperty-transparentTrack 写进任何 qss 都不生效，只能代码里设。
    // 取色本身不需要本仓插手：FluProgressRing.qss 三主题齐全，经 controls/CMakeLists.txt
    // 的 qt_add_resources（**不是** ControlsQss.qrc——只 grep 后者会误判成没注册）进资源，
    // 环构造期即自加载、并自订阅 themeChanged，暗色下拿的是 vendor 的 118,185,237 / 82,139,255。
    m_progressRing->setTransparentTrack(false);
    m_progressRing->setVisible(false);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_progressRing); // 环在钮左侧：进度在左、动作在右，与阅读顺序一致
    row->addWidget(m_checkButton);
    row->addWidget(m_applyButton);
    row->addWidget(m_restartButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），图标/标题布局不变

    applyStatus();

    connect(m_checkButton, &QPushButton::clicked, this, [this]() { startUpdateCheck(true); });
    connect(m_applyButton, &QPushButton::clicked, this, [this]() { handleApplyClicked(); });
    connect(m_restartButton, &QPushButton::clicked, this, [this]() { handleRestartClicked(); });

    connect(m_updateManager, &UpdateManager::checkFinished, this, &UpdateSettingCard::onCheckFinished);
    connect(m_updateManager, &UpdateManager::stageChanged, this, &UpdateSettingCard::onStageChanged);
    connect(m_updateManager, &UpdateManager::progressChanged, this, &UpdateSettingCard::onProgressChanged);
    connect(m_updateManager, &UpdateManager::staged, this, &UpdateSettingCard::onStaged);
}

void UpdateSettingCard::checkNow()
{
    startUpdateCheck(false);
}

void UpdateSettingCard::retranslate()
{
    setTitleInfo(tr("版本更新"), QString());
    m_checkButton->setText(tr("检查更新"));
    m_applyButton->setText(tr("下载并更新"));
    m_restartButton->setText(tr("重启"));
    applyStatus(); // 状态文案由令牌重生成，不在这里另拼一套
}

void UpdateSettingCard::startUpdateCheck(bool forced)
{
    // forced = 手动点按钮，允许再打一次 API；非 forced = showEvent 自动校准，同一卡实例
    // 只查一次（未认证限流 60 次/小时/IP，反复切进切出设置页不该烧配额）。
    if (m_checkStarted && !forced)
        return;
    m_checkStarted = true;
    m_stateToken = QStringLiteral("checking");
    m_progressText.clear();
    // 样本必须一起清：只清文案不清 received/total，下一轮进入 downloading 时
    // applyProgressRing() 会拿**上一轮**的旧分母先渲染出一个百分比（下完过一次就是 100%），
    // 首个 downloadProgress 到来后它才纠正——这段时间环显示的是一个编出来的进度。
    m_progressReceived = 0;
    m_progressTotal = -1;
    m_applyButton->setEnabled(false);
    applyStatus();
    m_restartButton->setEnabled(false);
    // m_swapApplied 刻意不复位：换体已落盘时再检查→再下载→再点重启，若旗标被清掉就会
    // 二次 applySwap——其改名腾位步先 remove(backup)，删掉的正是旧版可执行体的唯一副本。
    m_updateManager->checkLatest();
}

QString UpdateSettingCard::statusText() const
{
    const QString local = QCoreApplication::applicationVersion();
    const QString token = m_stateToken;

    // 进行中 / 成功态
    if (token == QLatin1String("checking"))
        return tr("正在检查更新…");
    if (token == QLatin1String("up_to_date"))
        return tr("已是最新版本（%1）").arg(local); // 版本数字豁免翻译
    if (token == QLatin1String("update_available"))
        return tr("发现新版本 %1（当前 %2）").arg(m_remoteTag, local);
    if (token == QLatin1String("downloading")) {
        if (m_progressText.isEmpty())
            return tr("正在下载 %1…").arg(m_remoteTag);
        return tr("正在下载 %1… %2").arg(m_remoteTag, m_progressText);
    }
    if (token == QLatin1String("verifying"))
        return tr("正在校验 SHA256…");
    if (token == QLatin1String("extracting"))
        return tr("校验通过，正在解包…");
    if (token == QLatin1String("swapping"))
        return tr("正在换体…");
    if (token == QLatin1String("ready_to_restart"))
        // 换体未做、重启未问：说清「点重启才落盘」，不写成已更新。
        return tr("已就绪，点击重启完成更新（%1）").arg(m_remoteTag);
    if (token == QLatin1String("restart_pending"))
        // 换体已成功，只是重启被取消或没拉起来。说成「更新失败」会诱导用户重装，是更坏的结果。
        return tr("已更新到 %1，重启后生效").arg(m_remoteTag);

    // 失败态：每条都给得出「为什么」，绝不与「已是最新」共用一句文案。
    if (token == QLatin1String("dev_layout"))
        return tr("开发态构建，已禁用");
    if (token == QLatin1String("no_config_root"))
        return tr("未找到数据根 config，已拒绝");
    if (token == QLatin1String("read_only_install"))
        return tr("安装目录不可写，无法更新");
    if (token == QLatin1String("bad_repo_slug"))
        return tr("更新源配置有误");
    if (token == QLatin1String("no_win64_asset"))
        return tr("%1 没有 win64 安装包").arg(m_remoteTag);
    if (token == QLatin1String("no_digest"))
        return tr("发布缺少 SHA256 校验和，已拒绝");
    if (token == QLatin1String("digest_mismatch"))
        return tr("校验和不匹配，已丢弃未换体");
    if (token == QLatin1String("package_layout_rejected"))
        return tr("包内路径越界，已拒绝换体");
    if (token == QLatin1String("another_instance_swapping"))
        return tr("另一个实例正在换体，稍后再试");
    if (token == QLatin1String("pending_marker"))
        return tr("存在未完成的更新残留，已拒绝换体（请先重启完成上一次更新）");
    if (token == QLatin1String("swap_declined"))
        return tr("已暂存未换体，可重试");
    if (token.startsWith(QLatin1String("network:")))
        return tr("检查失败：%1").arg(token.section(QLatin1Char(':'), 1).trimmed());
    if (token.startsWith(QLatin1String("download:")))
        return tr("下载中断：%1").arg(token.section(QLatin1Char(':'), 1).trimmed());
    if (token.startsWith(QLatin1String("open_failed:")))
        return tr("暂存文件写入失败：%1").arg(token.section(QLatin1Char(':'), 1).trimmed());
    if (token.startsWith(QLatin1String("rename_failed:")) || token.startsWith(QLatin1String("copy_failed:")))
        return tr("换体失败已回滚（%1）").arg(token.section(QLatin1Char(':'), 1).trimmed());
    if (token.startsWith(QLatin1String("tar_")))
        return tr("解包失败（%1）未换体").arg(token);
    if (token == QLatin1String("cancelled"))
        return tr("已取消");
    if (token.isEmpty())
        return tr("尚未检查");
    return tr("更新未完成（%1）").arg(token); // 未知令牌如实带出，不伪装成已知状态
}

void UpdateSettingCard::applyStatus()
{
    // 走基类说明位：前缀恒完整、值按剩余宽度中间省略、ToolTip 给全量——长错误串与
    // 进度文案正是这个形态要吃的东西（裸 FluLabel + wordWrap 会撑开行高、与同页六卡不齐）。
    setInfoValue(tr("状态"), statusText());
    // 令牌只有一个消费出口：文案与进度环在这里一起落，不在各槽里分别调。
    // 分开的写法迟早会出现「改了令牌忘了收环」，而那种分叉冒烟看不出来（环还在转，
    // 文案已说成功），只会以「更新成功后环一直转」的形式被用户撞见。
    applyProgressRing();
    // 按钮态也收口在这里（与环同理，防各槽分别维护迟早分叉）：
    // ① 忙态禁「检查更新」——在途时再查会覆写管理器的单 reply 指针，下载数据从此
    //    不落盘，好包被误报校验和不匹配（管理器侧另有 busy 卫兵兜底）。
    const QString &token = m_stateToken;
    const bool busy = token == QLatin1String("checking") || token == QLatin1String("downloading")
                      || token == QLatin1String("verifying") || token == QLatin1String("extracting")
                      || token == QLatin1String("swapping");
    m_checkButton->setEnabled(!busy);
    // ② 已换体则「重启」恒可点：换体已落盘、只差一次重启，任何后续检查/下载/失败
    //    都不该把这条唯一生效通路关掉（此前 startUpdateCheck 无条件禁它，检查一失败
    //    用户就在 UI 上无路可走，只能手动关程序）。
    if (m_swapApplied)
        m_restartButton->setEnabled(true);
}

void UpdateSettingCard::applyProgressRing()
{
    const bool downloading = (m_stateToken == QLatin1String("downloading"));
    if (!downloading) {
        // 顺序是判据的一部分：**先停无限动画，再隐藏**。QPropertyAnimation 不看可见性，
        // 只 setVisible(false) 的话那条 loopCount(-1) 的旋转动画会在后台一直跑到底，
        // 白烧 CPU 且下次显示时相位随机。
        m_progressRing->setWorking(false);
        m_progressRing->setVisible(false);
        return;
    }

    const int percent = UpdateRules::progressPercentOrUnknown(m_progressReceived, m_progressTotal);
    if (percent < 0) {
        // 分母未知（-1）：走不定态旋转，而不是拿 0 去除、也不是显示一个假百分比。
        // 这正是 assetSizeOrUnknown 刻意把「未知」留成 -1 而不是 0 所要防的事。
        m_progressRing->setWorking(true);
    } else {
        m_progressRing->setWorking(false);
        m_progressRing->setCurValue(percent); // 200ms OutCubic 平滑推进，不逐包跳格
    }
    m_progressRing->setVisible(true);
}

void UpdateSettingCard::onCheckFinished(bool hasUpdate, const QString &remoteTag, const QString &error)
{
    // 忙态卫兵的回流不落文案也不动样本：在途流程的观测面由它自己的终态负责。
    if (error == QLatin1String("busy"))
        return;
    m_remoteTag = remoteTag;
    if (!error.isEmpty()) {
        m_stateToken = error;
        m_applyButton->setEnabled(false);
    } else if (hasUpdate) {
        m_stateToken = QStringLiteral("update_available");
        m_applyButton->setEnabled(true);
    } else {
        m_stateToken = QStringLiteral("up_to_date");
        m_applyButton->setEnabled(false);
    }
    applyStatus();
}

void UpdateSettingCard::onStageChanged(UpdateManager::Stage stage, const QString &detail)
{
    // 令牌优先取 detail（它带具体错误码），detail 空时按阶段名映射。
    if (!detail.isEmpty()) {
        m_stateToken = detail;
        applyStatus();
        return;
    }
    switch (stage) {
    case UpdateManager::Stage::Checking:    m_stateToken = QStringLiteral("checking"); break;
    case UpdateManager::Stage::Downloading: m_stateToken = QStringLiteral("downloading"); break;
    case UpdateManager::Stage::Verifying:   m_stateToken = QStringLiteral("verifying"); break;
    case UpdateManager::Stage::Extracting:  m_stateToken = QStringLiteral("extracting"); break;
    case UpdateManager::Stage::Swapping:    m_stateToken = QStringLiteral("swapping"); break;
    case UpdateManager::Stage::Swapped:
    case UpdateManager::Stage::Restarting:  m_stateToken = QStringLiteral("restart_pending"); break;
    case UpdateManager::Stage::ReadyToSwap: return; // 紧接着的 onStaged 会落文案，这里不抢
    case UpdateManager::Stage::Failed:      return; // 同上：staged / checkFinished 负责出话
    case UpdateManager::Stage::Idle:        return;
    }
    applyStatus();
}

void UpdateSettingCard::onProgressChanged(qint64 received, qint64 total)
{
    m_progressReceived = received;
    m_progressTotal = total;
    // 文案与环读同一条判据，而不是两处各判一次「有没有分母」：各判一次迟早会分叉成
    // 「说明位写着 47%、环却在空转」或反过来，而这两种分叉在冒烟里都只表现为「好像没事」。
    // 分母为 0 不显示百分比的口径也在这里单源（assetSizeOrUnknown 刻意留 -1 而非 0）。
    const int percent = UpdateRules::progressPercentOrUnknown(received, total);
    if (percent >= 0)
        m_progressText = QStringLiteral("%1%").arg(percent);
    else
        m_progressText = QStringLiteral("%1 MB").arg(received / (1024 * 1024));
    if (m_stateToken == QLatin1String("downloading"))
        applyStatus();
}

void UpdateSettingCard::handleApplyClicked()
{
    // 这一步只做「下载 + 校验 + 解包到暂存区」，全程可弃、不碰安装树，所以不必再确认；
    // 唯一不可逆的换体留在 onStaged 里问。
    m_stateToken = QStringLiteral("downloading");
    m_progressText.clear();
    // 这里也必须清样本，不能只靠 startUpdateCheck 那次：失败重试路径（onStaged 把按钮放回来
    // 再点）不经过任何检查动作，样本还留着上一轮中断时的 received/total，环会直接从那个
    // 数字起转，而不是从不定态重新开始。
    m_progressReceived = 0;
    m_progressTotal = -1;
    m_applyButton->setEnabled(false); // 下载期禁用防重入（常驻可见，不收起）
    applyStatus();
    m_restartButton->setEnabled(false);
    // m_swapApplied 不复位，理由同 startUpdateCheck：旗标只随换体成功置 true，
    // 进程存活期内没有任何路径使它「为 true 但实际未换体」，复位只会放行二次换体。
    m_updateManager->stageUpdate(m_updateManager->remoteAssetUrl(),
                                 m_updateManager->remoteDigest(),
                                 m_updateManager->remoteSize());
}

void UpdateSettingCard::onStaged(bool ok, const QString &error)
{
    if (error == QLatin1String("busy"))
        return; // 同 onCheckFinished：忙态回流不打断在途流程
    if (!ok) {
        m_stateToken = error;
        // 失败后把按钮放回来：远端资产信息仍在管理器里，用户可直接重试，
        // 不必先「检查更新」再点一次（初稿一律隐藏，等于把重试入口也藏了）。
        m_applyButton->setEnabled(true);
        applyStatus();
        return;
    }

    // 下载、校验、解包都过了：换体是唯一不可逆步骤，不自动执行，也不自动弹框。
    // 重启按钮在这里解禁——常态禁用（未下载完成不可点），staged 后由用户主动点击触发确认框。
    m_stateToken = QStringLiteral("ready_to_restart");
    m_applyButton->setEnabled(false);
    m_restartButton->setEnabled(true);
    applyStatus();
}

void UpdateSettingCard::handleRestartClicked()
{
    // parent 必须传 window()：FluMessageBox 的构造用 setGeometry(0,0,parentW,parentH) 把遮罩
    // 铺满**父控件**，其内部卡片固定 318×218、按钮行贴在 y≈218。传 this（本卡操作行，高约 30）
    // 会把对话框裁成卡片大小、「确定/取消」整条被切掉——用户看不到按钮，exec() 就地永久阻塞。
    QWidget *anchor = window();
    if (!anchor) {
        // FluMessageBox 的约定是 parent 不可为 null（构造里直接 m_parentWidget->size()）。
        m_stateToken = QStringLiteral("restart_pending");
        applyStatus();
        return;
    }
    // 换体是否已落盘决定问法：已换体时只问重启，绝不重跑 applySwap——暂存目录在换体末尾已被
    // removeRecursively，重跑必 copy_failed 触发回滚，把已换好的新体删掉、把旧体改回原位。
    const QString info = m_swapApplied
        ? tr("%1 已换体，重启后生效，重启会中断未完成的回合。现在重启？").arg(m_remoteTag)
        : tr("%1 已下载并校验通过，换体后需重启生效，重启会中断未完成的回合。现在更新并重启？")
              .arg(m_remoteTag);
    FluMessageBox box(tr("更新已就绪"), info, anchor);
    // 默认键钉「取消」：与 LiteHarness::closeEvent 同一条数据丢失防线——FluMessageBox 不给按钮
    // 设默认键，裸用回车会命中排首位的「确定」。
    if (auto *cancel = box.findChild<FluPushButton *>())
        cancel->setDefault(true);
    if (box.exec() != QDialog::Accepted) {
        // 取消时的令牌必须按 m_swapApplied 分流，不能一律 swap_declined：已换体后那句
        // 「已暂存未换体，可重试」与实际状态相反，会把用户引向重装或再点「下载并更新」，
        // 而这条路径上重跑 stageUpdate 只是白下一遍——实际只差一次重启。
        m_stateToken = m_swapApplied ? QStringLiteral("restart_pending")
                                     : QStringLiteral("swap_declined");
        applyStatus();
        return;
    }

    if (!m_swapApplied) {
        QString failure;
        if (!m_updateManager->applySwap(&failure)) {
            m_stateToken = failure;
            m_applyButton->setEnabled(false);
            m_restartButton->setEnabled(false);
            applyStatus();
            return;
        }
        m_swapApplied = true;
    }
    m_applyButton->setEnabled(false);
    // 重启被主窗口 close() 守卫挡下（有回合在跑且用户不退出）时保持可点：换体与 marker 已在位，
    // 用户可再点一次；下次正常启动也会由 cleanupResidueAtStartup 收尾。
    m_restartButton->setEnabled(true);
    if (!m_updateManager->requestRestartAfterSwap(window())) {
        m_stateToken = QStringLiteral("restart_pending");
        applyStatus();
    }
}

void SettingsPage::showEvent(QShowEvent *event)
{
    BasePage::showEvent(event);
    // 首次可见才自动校准：只查一次的旗标与限流口径都在卡内，本页只做转发。
    if (m_updateCard)
        m_updateCard->checkNow();
}

void SettingsPage::changeEvent(QEvent *event)
{
    // i18n 第八轮：当前链路 translator 在启动前装载（构造期即目标语言），本钩子
    // 兜底同进程 LanguageChange 广播场景（applyLanguage 重装 translator 触发）
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    BasePage::changeEvent(event);
}

void SettingsPage::retranslateUi()
{
    m_appearanceLabel->setText(tr("外观与行为"));

    m_appThemeBox->setTitleInfo(tr("应用主题"), tr("选择应用显示的主题。"));
    m_appThemeBox->getComboBox()->setItemText(0, tr("浅色"));
    m_appThemeBox->getComboBox()->setItemText(1, tr("深色"));
    m_appThemeBox->getComboBox()->setItemText(2, tr("AtomOneDark"));

    m_languageBox->setTitleInfo(tr("语言"), tr("选择界面显示的语言。"));
    // combo 项为自名（构造注释），语言变化不重译

    m_workDirLabel->setText(tr("工作目录"));
    if (m_workDirCard)
        m_workDirCard->retranslate();

    m_contextLabel->setText(tr("上下文"));
    if (m_contextCard)
        m_contextCard->retranslate();

    m_maxRoundsLabel->setText(tr("最大轮次"));
    if (m_maxRoundsCard)
        m_maxRoundsCard->retranslate();

    m_permissionLabel->setText(tr("权限"));
    m_permissionBox->setTitleInfo(tr("工具执行权限审批"), tr("命中询问规则时是否需要你批准。"));
    m_permissionBox->setToolTip(tr("关闭后工具不再弹审批卡、直接执行；bash 硬拒绝黑名单、workflow 的 deny "
                                   "策略与工作区越界拦截不受影响。改完下一个工具调用即生效，无需重启。"));
    m_permissionBox->getComboBox()->setItemText(0, tr("需要审批"));
    m_permissionBox->getComboBox()->setItemText(1, tr("无需审批"));

    m_modelLabel->setText(tr("模型服务"));
    // 四卡同组同口径重译（顺序与构造序一致：provider 卡在首）。漏掉任何一张，切语言后
    // 那张的标题/按钮会滞留旧语言——它们都是构造期定值，不重译不会自己变。
    if (m_providerCard)
        m_providerCard->retranslate();
    if (m_apiUrlCard)
        m_apiUrlCard->retranslate();
    if (m_apiTokenCard)
        m_apiTokenCard->retranslate();
    if (m_modelListCard)
        m_modelListCard->retranslate();

    // s17.12 自更新：组标题在此重译（与「上下文」等其余五组同款），卡内文案走卡自己的 retranslate。
    m_updateLabel->setText(tr("更新"));
    // 整卡重译（按钮文案 + 状态令牌重生成文案），状态文案的唯一出口在
    // UpdateSettingCard::statusText()，本页不再另拼一套。
    if (m_updateCard)
        m_updateCard->retranslate();

    m_aboutLabel->setText(tr("关于"));
    m_versionBox->getInfoLabel()->setText(tr("@2026 lite harness. 保留所有权利。"));
    m_infoLabel->setText(
        tr("LiteHarness 是一款轻量级的 C++ 编码代理 harness 应用，旨在填补 C++ 生态中 harness 实现的空白。"
           "它作为一个动手学习项目，逐步演示如何使用 Qt 与现代 C++ 从零构建一个 harness。"));
}
