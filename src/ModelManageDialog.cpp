#include "ModelManageDialog.h"

#include "SettingsPage.h" // providerErrorText / maskedApiToken / commitEditedCurrent / ModelServiceTarget 单源
#include "AgentConstants.h"
#include "InputDialog.h"
#include "LayoutConstants.h"
#include "ModelListFetcher.h"
#include "UiScale.h"
#include "ThemeAware.h"

#include <FluComboBox.h>
#include <FluLineEdit.h>
#include <FluMessageBox.h>
#include <FluPushButton.h>
#include <FluStyleButton.h>
#include <FluToggleSwitch.h>
#include <FluVScrollView.h>
#include <FluVSplitLine.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// 行内各列固定宽（滑条列吃剩余宽）：列头行与数据行共用，保证纵向对齐。
constexpr int kNameW = 230;
constexpr int kThinkW = 80;
constexpr int kEffortW = 96;
constexpr int kShowW = 80;
constexpr int kWindowW = 220;
constexpr int kRemoveW = 64;

void clearLayout(QVBoxLayout *layout)
{
    while (layout->count()) {
        QLayoutItem *item = layout->takeAt(0);
        delete item->widget();
        delete item;
    }
}

} // namespace

ModelManageDialog::ModelManageDialog(ModelServiceTarget *target, QWidget *parent)
    : QDialog(parent), m_target(target), m_parentWidget(parent)
{
    m_pending = ModelPrefs::allPrefs();
    m_custom = ModelPrefs::customNames();
    m_originalCustom = m_custom;

    // —— 窗口骨架：照 InputDialog（遮罩不进布局，几何三处手动铺满） ——
    auto boxLayout = new QHBoxLayout(this);
    m_windowMask = new QWidget(this);
    m_windowMask->setObjectName("windowMask");

    m_card = new QFrame(this);
    m_card->setObjectName("centerWidget");
    m_card->setFixedSize(qMin(UiScale::scaled(980), 1080), qMin(UiScale::scaled(640), 720));
    boxLayout->addWidget(m_card, 1, Qt::AlignCenter);

    auto cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // —— 头部行：自定义模型标题 + 关闭 ——
    auto header = new QHBoxLayout();
    header->setContentsMargins(24, 16, 16, 8);
    header->setSpacing(8);

    auto *title = new QLabel(tr("自定义模型"), m_card);
    title->setObjectName("dialogTitle");
    header->addWidget(title);
    header->addStretch(1);
    cardLayout->addLayout(header);
    
    cardLayout->addWidget(new FluVSplitLine(m_card));
    cardLayout->addSpacing(15);

    // —— 服务配置：服务商选择 + 当前 provider 的地址与密钥（修改/清除即时落盘） ——
    auto service = new QVBoxLayout();
    service->setContentsMargins(24, 0, 24, 8);
    service->setSpacing(6);

    const QSize actionSize(UiScale::scaled(LayoutConst::kCardActionWidth),
                           UiScale::scaled(LayoutConst::kCardActionHeight));

    auto providerRow = new QHBoxLayout();
    providerRow->setContentsMargins(0, 0, 0, 0);
    providerRow->setSpacing(8);
    auto *providerLabel = new QLabel(tr("服务商"), m_card);
    providerLabel->setObjectName("serviceLabel");
    providerLabel->setFixedWidth(UiScale::scaled(90));
    m_providerCombo = new FluComboBox(m_card);
    m_providerCombo->setFixedWidth(UiScale::scaled(200));
    m_providerAddButton = new FluPushButton(tr("新增"), m_card);
    m_providerAddButton->setFixedSize(actionSize);
    m_providerRemoveButton = new FluPushButton(tr("删除"), m_card);
    m_providerRemoveButton->setFixedSize(actionSize);
    providerRow->addWidget(providerLabel);
    providerRow->addWidget(m_providerCombo);
    providerRow->addStretch(1);
    providerRow->addWidget(m_providerAddButton);
    providerRow->addWidget(m_providerRemoveButton);
    service->addLayout(providerRow);
    auto serviceRow = [this, service, actionSize](const QString &labelText, QLabel *value,
                                                  FluPushButton *modify, FluPushButton *clear) {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *label = new QLabel(labelText, m_card);
        label->setObjectName("serviceLabel");
        label->setFixedWidth(UiScale::scaled(90));
        value->setObjectName("serviceValue");
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        modify->setFixedSize(actionSize);
        clear->setFixedSize(actionSize);
        row->addWidget(label);
        row->addWidget(value, 1);
        row->addWidget(modify);
        row->addWidget(clear);
        service->addLayout(row);
    };

    m_urlValue = new QLabel(m_card);
    auto *urlModify = new FluPushButton(tr("修改"), m_card);
    auto *urlClear = new FluPushButton(tr("清除"), m_card);
    serviceRow(tr("服务地址"), m_urlValue, urlModify, urlClear);

    m_tokenValue = new QLabel(m_card);
    auto *tokenModify = new FluPushButton(tr("修改"), m_card);
    auto *tokenClear = new FluPushButton(tr("清除"), m_card);
    serviceRow(tr("API Key"), m_tokenValue, tokenModify, tokenClear);

    // —— 拉取 / 添加模型：置于 API Key 行下方 ——
    auto actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    m_fetchButton = new FluPushButton(tr("拉取"), m_card);
    m_fetchButton->setFixedSize(actionSize); // 与上方新增/删除/修改/清除同尺寸同右缘
    m_addButton = new FluPushButton(tr("添加"), m_card);
    m_addButton->setFixedSize(actionSize);
    actionRow->addStretch(1);
    actionRow->addWidget(m_fetchButton);
    actionRow->addWidget(m_addButton);
    service->addLayout(actionRow);
    cardLayout->addLayout(service);

    // —— 列头行：与数据行同列宽 ——
    auto columns = new QHBoxLayout();
    columns->setContentsMargins(24, 4, 24, 4);
    columns->setSpacing(12);
    auto colLabel = [this](const QString &text, int width) {
        auto *label = new QLabel(text, m_card);
        label->setObjectName("colHeader");
        label->setAlignment(Qt::AlignCenter);
        if (width > 0)
            label->setFixedWidth(UiScale::scaled(width));
        return label;
    };
    columns->addWidget(colLabel(tr("模型名称"), kNameW));
    columns->addWidget(colLabel(tr("思考"), kThinkW));
    columns->addWidget(colLabel(tr("思考强度"), kEffortW));
    columns->addWidget(colLabel(tr("显示状态"), kShowW));
    columns->addWidget(colLabel(tr("上下文窗口"), kWindowW));
    columns->addStretch(1);
    columns->addWidget(colLabel(QString(), kRemoveW));
    cardLayout->addLayout(columns);

    // —— 滚动区：FluentUI 纵向滚动控件（自带 FluScrollBar），仅自定义模型 ——
    m_scroll = new FluVScrollView(m_card);
    m_modelPage = m_scroll->widget();
    m_modelPage->setObjectName("modelPage");
    m_modelLayout = m_scroll->getMainLayout();
    m_modelLayout->setContentsMargins(24, 0, 24, 8);
    m_modelLayout->setSpacing(4);
    m_modelLayout->setAlignment(Qt::AlignTop);
    cardLayout->addWidget(m_scroll, 1);

    cardLayout->addWidget(new FluVSplitLine(m_card));

    // —— 底部行：说明 + 取消/保存设置 ——
    auto btnWidget = new QWidget(m_card);
    btnWidget->setObjectName("btnWidget");
    btnWidget->setFixedHeight(UiScale::scaled(80));
    auto btnLayout = new QHBoxLayout(btnWidget);
    btnLayout->setContentsMargins(24, 0, 24, 0);
    btnLayout->setSpacing(10);

    m_footerInfo = new QLabel(
        tr("这些自定义模型的偏好对所有任务生效。运行中的回复保持当前设置，下一轮开始使用新设置。"),
        btnWidget);
    m_footerInfo->setObjectName("footerInfo");
    m_footerInfo->setWordWrap(true);
    btnLayout->addWidget(m_footerInfo, 1);

    m_cancelButton = new FluPushButton(tr("取消"), btnWidget);
    m_cancelButton->setFixedWidth(UiScale::scaled(100));
    m_saveButton = new FluStyleButton(btnWidget);
    m_saveButton->setText(tr("保存设置"));
    m_saveButton->setFixedWidth(UiScale::scaled(110));
    btnLayout->addWidget(m_cancelButton);
    btnLayout->addWidget(m_saveButton);
    cardLayout->addWidget(btnWidget);

    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    connect(m_cancelButton, &FluPushButton::clicked, this, &QDialog::reject);
    connect(m_saveButton, &FluStyleButton::clicked, this, [this]() { save(); });
    connect(m_addButton, &FluPushButton::clicked, this, [this]() { addModel(); });
    connect(m_fetchButton, &FluPushButton::clicked, this, [this]() { fetchFromServer(); });
    connect(urlModify, &FluPushButton::clicked, this, [this]() { promptEditUrl(); });
    connect(urlClear, &FluPushButton::clicked, this, [this]() {
        ProviderSpec::Provider edited = m_target->current();
        edited.baseUrl.clear();
        commitEditedCurrent(m_target, edited, this);
    });
    connect(tokenModify, &FluPushButton::clicked, this, [this]() { promptEditToken(); });
    connect(tokenClear, &FluPushButton::clicked, this, [this]() {
        ProviderSpec::Provider edited = m_target->current();
        edited.token.clear();
        commitEditedCurrent(m_target, edited, this);
    });
    connect(m_target, &ModelServiceTarget::targetChanged, this, [this]() {
        refreshProviderCombo();
        updateServiceValues();
        // 换服务商必须连着重画模型行：此前只刷了地址/密钥那两行，模型列表停在
        // 上一个服务商的内容上（rebuildRows 当时又取的是跨服务商摊平的生效清单，
        // 于是「切了没反应」与「新服务商显示旧模型」两条症状叠成同一个现象）。
        rebuildRows();
    });
    connect(m_providerCombo, &FluComboBox::currentIndexChanged, this, [this](int index) {
        if (m_repopulatingProvider)
            return;
        const QList<ProviderSpec::Provider> list = m_target->providers();
        if (index >= 0 && index < list.size())
            m_target->setProviderId(list.at(index).id);
    });
    connect(m_providerAddButton, &FluPushButton::clicked, this, [this]() { promptAddProvider(); });
    connect(m_providerRemoveButton, &FluPushButton::clicked, this, [this]() { promptRemoveProvider(); });

    if (m_parentWidget) {
        setGeometry(0, 0, m_parentWidget->width(), m_parentWidget->height());
        m_windowMask->resize(m_parentWidget->size());
        m_parentWidget->installEventFilter(this);
    } else {
        m_windowMask->hide();
    }

    refreshProviderCombo();
    updateServiceValues();
    rebuildRows();

    ThemeAware::bind("ModelManageDialog.qss", this);
}

void ModelManageDialog::refreshProviderCombo()
{
    const QList<ProviderSpec::Provider> list = m_target->providers();
    m_repopulatingProvider = true;
    m_providerCombo->blockSignals(true);
    m_providerCombo->clear();
    QStringList names;
    for (const ProviderSpec::Provider &p : list)
        names << (p.name.isEmpty() ? p.id : p.name);
    m_providerCombo->addItems(names);
    const int at = m_target->currentIndex();
    if (at >= 0)
        m_providerCombo->setCurrentIndex(at);
    m_providerCombo->blockSignals(false);
    m_repopulatingProvider = false;

    // 只剩一条时禁删：删空会让 loadProviders() 回落到旧三键镜像，表现为「删了又自己长回来」。
    m_providerRemoveButton->setEnabled(list.size() > 1);
    m_providerRemoveButton->setToolTip(list.size() > 1 ? QString()
                                                       : tr("至少需保留一个服务商。"));
}

void ModelManageDialog::promptAddProvider()
{
    const auto [input, accepted] = InputDialog::getInputText(
        this, tr("新增服务商"), tr("给这个服务商起个名字，如 DeepSeek。"), QString());
    if (!accepted)
        return;
    ProviderSpec::Provider fresh;
    fresh.name = ProviderSpec::normalizeProviderName(input);
    if (!commitEditedCurrent(m_target, fresh, this))
        return;
    const QList<ProviderSpec::Provider> after = m_target->providers();
    if (!after.isEmpty())
        m_target->setProviderId(after.last().id);
}

void ModelManageDialog::promptRemoveProvider()
{
    const ProviderSpec::Provider cur = m_target->current();
    if (m_target->providers().size() <= 1)
        return;
    const QString label = cur.name.isEmpty() ? cur.id : cur.name;
    if (FluMessageBox(tr("删除服务商"),
                      tr("确定删除「%1」？其端点、密钥与模型清单都会移除。").arg(label),
                      this)
            .exec()
        != QDialog::Accepted)
        return;
    const QList<ProviderSpec::Provider> next = ProviderSpec::removeProvider(m_target->providers(), cur.id);
    if (!m_target->commit(next, this))
        return;
    // 被删服务商的自定义模型同步出清单，避免弹窗留下无来源的死行。
    bool changed = false;
    for (const QString &model : cur.models) {
        if (m_custom.removeAll(model) > 0) {
            m_pending.remove(model);
            changed = true;
        }
    }
    if (changed && !AppSettings::setValue(ModelPrefs::customNamesKey(), m_custom))
        FluMessageBox(tr("保存失败"), tr("config.json 写入失败，模型来源记录未保存。"), this).exec();
    m_target->setProviderId(next.isEmpty() ? QString() : next.first().id);
    rebuildRows();
}

void ModelManageDialog::updateServiceValues()
{
    const ProviderSpec::Provider cur = m_target->current();
    m_urlValue->setText(cur.baseUrl.isEmpty() ? tr("未设置") : cur.baseUrl);
    m_urlValue->setToolTip(cur.baseUrl);
    // 密钥明文绝不上屏也不进 ToolTip：值区只给脱敏摘要，悬停只确认「已保存」。
    m_tokenValue->setText(cur.token.isEmpty() ? tr("未设置") : maskedApiToken(cur.token));
    m_tokenValue->setToolTip(cur.token.isEmpty() ? QString() : tr("已保存"));
}

void ModelManageDialog::promptEditUrl()
{
    const auto [input, accepted] = InputDialog::getInputText(
        this, tr("设置服务地址"), tr("以 /v1 等版本路径结尾，不含补全端点。"),
        m_target->current().baseUrl);
    if (!accepted)
        return;
    const QString cleaned = input.trimmed();
    if (!cleaned.isEmpty() && !ProviderSpec::isValidBaseUrl(cleaned))
    {
        FluMessageBox(tr("无法保存"), providerErrorText(QStringLiteral("invalid-base-url")), this)
            .exec();
        return;
    }
    ProviderSpec::Provider edited = m_target->current();
    edited.baseUrl = cleaned;
    commitEditedCurrent(m_target, edited, this);
}

void ModelManageDialog::promptEditToken()
{
    // 不预填存量明文 + Password 回显；留空＝不修改，清除走独立按钮。
    const auto [input, accepted] = InputDialog::getInputText(
        this, tr("设置 API Key"), tr("输入新的 Key 并保存；留空表示不修改。"),
        QString(), QLineEdit::EchoMode::Password);
    if (!accepted)
        return;
    const QString cleaned = input.trimmed();
    if (cleaned.isEmpty())
        return;
    ProviderSpec::Provider edited = m_target->current();
    edited.token = cleaned;
    commitEditedCurrent(m_target, edited, this);
}

QWidget *ModelManageDialog::buildRow(const QString &model)
{
    const ModelPrefs::Prefs prefs = m_pending.value(model);
    auto *row = new QWidget(m_modelPage);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 4, 0, 4);
    h->setSpacing(12);

    auto *name = new QLabel(model, row);
    name->setObjectName("rowName");
    name->setAlignment(Qt::AlignCenter);
    name->setFixedWidth(UiScale::scaled(kNameW));
    name->setToolTip(model);
    h->addWidget(name);

    // FluToggleSwitch::setChecked 会 updateSize() 重置自身固定尺寸，直接进行布局会撑列宽；
    // 故关自动文案 + 固定宽单元格居中，列宽只由单元格决定。
    auto switchCell = [this, row, h](FluToggleSwitch *sw, int width) {
        sw->setEmptyText(true);
        auto *cell = new QWidget(row);
        cell->setFixedWidth(UiScale::scaled(width));
        auto *cl = new QHBoxLayout(cell);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->addWidget(sw, 0, Qt::AlignCenter);
        h->addWidget(cell);
    };

    auto *thinking = new FluToggleSwitch(row);
    thinking->setSize(SwitchSize::Medium);
    thinking->setChecked(prefs.thinking);
    switchCell(thinking, kThinkW);

    auto *effort = new FluComboBox(row);
    effort->addItems({tr("低"), tr("中"), tr("高"), tr("极高")});
    effort->setCurrentIndex(ModelPrefs::effortIndex(prefs.effort));
    effort->setEnabled(prefs.thinking);
    effort->setFixedWidth(UiScale::scaled(kEffortW));
    h->addWidget(effort);

    auto *visible = new FluToggleSwitch(row);
    visible->setSize(SwitchSize::Medium);
    visible->setChecked(prefs.visible);
    switchCell(visible, kShowW);

    // 上下文长度直接录入 token 数（0~1M），空 = 未设回落全局；判据在 ModelPrefs 单源
    auto *windowEdit = new FluLineEdit(row);
    windowEdit->setFixedWidth(UiScale::scaled(kWindowW));
    windowEdit->setAlignment(Qt::AlignCenter);
    windowEdit->setPlaceholderText(
        tr("未设，回落全局 %1").arg(AgentConst::contextTokenBudget()));
    windowEdit->setText(prefs.window > 0 ? QString::number(prefs.window) : QString());
    connect(windowEdit, &QLineEdit::editingFinished, this, [this, model, windowEdit]() {
        const int value = ModelPrefs::windowFromText(windowEdit->text());
        if (value < 0) {
            const int current = m_pending.value(model).window;
            windowEdit->setText(current > 0 ? QString::number(current) : QString());
            return;
        }
        m_pending[model].window = value;
        windowEdit->setText(value > 0 ? QString::number(value) : QString());
    });
    h->addWidget(windowEdit);
    h->addStretch(1);

    auto *remove = new FluPushButton(tr("移除"), row);
    remove->setFixedSize(QSize(UiScale::scaled(LayoutConst::kCardActionWidth),
                               UiScale::scaled(LayoutConst::kCardActionHeight)));
    h->addWidget(remove);

    connect(thinking, &FluToggleSwitch::checkedChanged, this, [this, model, effort](bool on) {
        m_pending[model].thinking = on;
        effort->setEnabled(on);
    });
    connect(effort, &FluComboBox::currentIndexChanged, this, [this, model](int index) {
        m_pending[model].effort = ModelPrefs::effortWireValues().value(index);
    });
    connect(visible, &FluToggleSwitch::checkedChanged, this, [this, model](bool on) {
        m_pending[model].visible = on;
    });
    connect(remove, &FluPushButton::clicked, this, [this, model]() { removeCustom(model); });

    return row;
}

void ModelManageDialog::rebuildRows()
{
    clearLayout(m_modelLayout);

    // 模型行只取**当前服务商自己的清单**。此前取的是 AgentConst::modelOptions()
    // （跨服务商摊平的生效清单）∩ customModelNames（一张全局表），两个条件都不带
    // 服务商身份，于是新建的服务商一打开就挂着上一个服务商的模型——用户看到的
    // 「服务商之间共用模型」其实是这里根本没按服务商过滤。
    // 能只读 cur.models 的前提是既有不变量「模型名跨服务商唯一」：手动添加走
    // addModel 的生效清单查重、拉取走 mergeableModels 的跨家撞名拦截，两道都拦，
    // 所以「某个模型属于哪个服务商」是良定义的（customModelNames 不参与本判断，
    // 它只服务偏好表的写入与剪除，见 save()）。
    QStringList rows = m_target->current().models;
    rows.removeAll(QString());
    rows.removeDuplicates();
    std::sort(rows.begin(), rows.end());
    for (const QString &model : rows)
        m_modelLayout->addWidget(buildRow(model));
    const int customCount = rows.size();
    if (customCount == 0) {
        auto *empty = new QLabel(tr("暂无自定义模型，可添加或从服务端拉取。"), m_modelPage);
        empty->setObjectName("emptyHint");
        m_modelLayout->addWidget(empty);
    }
    m_modelLayout->addStretch(1);
}

void ModelManageDialog::addModel()
{
    const auto [input, accepted] = InputDialog::getInputText(
        this, tr("添加模型"),
        tr("输入模型 ID（大小写敏感，须与服务端一致），如 qwen3.8-flash。"), QString());
    if (!accepted)
        return;
    const QString name = ProviderSpec::normalizeModelName(input);
    // 撞名要说清撞在**哪家**：只报「已在生效清单中」而不给归属，用户看到的就是
    // 「我在甲里加了个模型，乙里也有」——正是本轮被报成「服务商共用模型」的那条困惑。
    // 名字跨服务商唯一是设计前提（请求按模型名路由到端点，重名即无法裁决该发给谁），
    // 所以这里拒绝而不是允许两家各存一份；归属判据单源 ProviderSpec::ownerIdOfModel。
    const QList<ProviderSpec::Provider> table = m_target->providers();
    const QString owner = ProviderSpec::ownerIdOfModel(table, name);
    if (!owner.isEmpty())
    {
        if (owner == m_target->current().id) {
            FluMessageBox(tr("无需添加"), tr("该模型已在本服务商清单中。"), this).exec();
        } else {
            const int at = ProviderSpec::indexOfId(table, owner);
            const ProviderSpec::Provider owning =
                at >= 0 ? table.at(at) : ProviderSpec::Provider{};
            const QString label = owning.name.isEmpty() ? owning.id : owning.name;
            FluMessageBox(tr("无法添加"),
                          tr("模型名跨服务商必须唯一：「%1」已属于服务商「%2」。")
                              .arg(name, label),
                          this)
                .exec();
        }
        return;
    }
    QString error;
    const QList<ProviderSpec::Provider> next =
        ProviderSpec::addModelToProvider(m_target->providers(), m_target->current().id, input, error);
    if (!error.isEmpty()) {
        FluMessageBox(tr("无法添加"), providerErrorText(error), this).exec();
        return;
    }
    if (!m_target->commit(next, this))
        return;
    // 添加的模型归入自定义窗口
    if (!m_custom.contains(name))
        m_custom.append(name);
    if (!AppSettings::setValue(ModelPrefs::customNamesKey(), m_custom))
        FluMessageBox(tr("保存失败"), tr("config.json 写入失败，模型来源记录未保存。"), this).exec();
    rebuildRows();
}

void ModelManageDialog::fetchFromServer()
{
    const ProviderSpec::Provider cur = m_target->current();
    if (cur.baseUrl.isEmpty()) {
        FluMessageBox(tr("无法拉取"), tr("请先为本服务商填写服务地址。"), this).exec();
        return;
    }
    m_fetching = true;
    m_fetchButton->setEnabled(false); // 在途门闩：防连点后到覆盖先到
    const QString providerId = cur.id;
    // parent=this 即生命周期锚：弹窗关闭即作废回调，无悬空 this
    ModelFetch::Request::fetch(
        cur.baseUrl, cur.token, this,
        [this, providerId](const QStringList &models, const QString &error) {
            m_fetching = false;
            m_fetchButton->setEnabled(true);
            if (!error.isEmpty()) {
                FluMessageBox(tr("拉取失败"),
                              error + tr("仍可用「+ 添加模型」手动填写。"), this)
                    .exec();
                return;
            }
            const QList<ProviderSpec::Provider> table = m_target->providers();
            const int at = ProviderSpec::indexOfId(table, providerId);
            if (at < 0)
                return; // 响应回来时 provider 已被删：丢弃
            QList<ProviderSpec::Provider> next = table;
            ProviderSpec::Provider edited = table.at(at);
            QStringList managed;
            for (const QString &raw : models) {
                const QString name = ProviderSpec::normalizeModelName(raw);
                if (edited.models.contains(name) && !managed.contains(name))
                    managed.append(name);
            }
            const QStringList added = ProviderSpec::mergeableModels(edited, models, table);
            if (added.isEmpty()) {
                bool changed = false;
                for (const QString &m : managed) {
                    if (!m_custom.contains(m)) {
                        m_custom.append(m);
                        changed = true;
                    }
                }
                if (changed && !AppSettings::setValue(ModelPrefs::customNamesKey(), m_custom))
                    FluMessageBox(tr("保存失败"), tr("config.json 写入失败，模型来源记录未保存。"), this).exec();
                rebuildRows();
                FluMessageBox(tr("拉取完成"),
                              tr("服务端返回 %1 个模型，均已在本服务商清单中。").arg(models.size()),
                              this)
                    .exec();
                return;
            }
            for (const QString &m : added) {
                if (edited.models.size() >= ProviderSpec::kModelsPerProviderMax)
                    break;
                edited.models.append(m);
                if (!managed.contains(m))
                    managed.append(m);
            }
            next[at] = edited;
            if (m_target->commit(next, this)) {
                for (const QString &m : managed) {
                    if (!m_custom.contains(m))
                        m_custom.append(m);
                }
                if (!AppSettings::setValue(ModelPrefs::customNamesKey(), m_custom))
                    FluMessageBox(tr("保存失败"), tr("config.json 写入失败，模型来源记录未保存。"), this).exec();
                rebuildRows();
            }
        });
}

void ModelManageDialog::removeCustom(const QString &model)
{
    // 只从**本服务商**剪除。旧写法遍历整张表逐个 removeAll，等于「在甲下点移除」
    // 连带把乙里同名的条目一起删掉——模型名跨服务商唯一是运行期判据拦的，不是
    // config.json 写不出来，手改过的表一旦撞上就是静默丢数据。
    QList<ProviderSpec::Provider> next = m_target->providers();
    const int at = ProviderSpec::indexOfId(next, m_target->current().id);
    if (at < 0)
        return; // 当前服务商已不在表里（弹窗开着时别处改了配置）：不动盘
    next[at].models.removeAll(model);
    if (!m_target->commit(next, this))
        return;
    m_custom.removeAll(model);
    m_pending.remove(model);
    if (!AppSettings::setValue(ModelPrefs::customNamesKey(), m_custom))
        FluMessageBox(tr("保存失败"), tr("config.json 写入失败，模型来源记录未保存。"), this).exec();
    rebuildRows();
}

void ModelManageDialog::save()
{
    // 只写自定义模型偏好；默认模型已有偏好原样保留，已移除的自定义项才删除。
    QHash<QString, ModelPrefs::Prefs> pruned = ModelPrefs::allPrefs();
    for (const QString &model : m_originalCustom) {
        if (!m_custom.contains(model))
            pruned.remove(model);
    }
    const QStringList effective = AgentConst::modelOptions();
    for (const QString &model : m_custom) {
        if (effective.contains(model))
            pruned.insert(model, m_pending.value(model));
    }
    if (!AppSettings::setValue(ModelPrefs::prefsKey(), ModelPrefs::allToVariantList(pruned))) {
        FluMessageBox(tr("保存失败"), tr("config.json 写入失败，偏好未保存。"), this).exec();
        return;
    }
    accept();
}

void ModelManageDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_parentWidget) {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
    }
}

bool ModelManageDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_parentWidget && event->type() == QEvent::Resize) {
        m_windowMask->resize(m_parentWidget->size());
        resize(m_parentWidget->size());
    }
    return QDialog::eventFilter(obj, event);
}