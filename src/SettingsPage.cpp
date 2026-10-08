#include "SettingsPage.h"
#include "AgentConstants.h"
#include "FluentInputDialog.h"
#include "I18n.h"
#include "QOpenAi.h" // 写入 ini 后即时覆盖全局配置（setUrl / setToken）
#include "ThemeAware.h"
#include <FluUtils.h>
#include <FluMessageBox.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QLocale>
#include "AppSettings.h"
#include <QFileInfo>
#include <QFileDialog>
#include <QUrl>
#include <FluVScrollView.h>
#include <FluLabel.h>
#include <FluSettingsVersionBox.h>
#include <FluPushButton.h>

namespace {

// 默认工作目录：settings.ini 配置文件（AppSettings 单源，exe 同目录）；
// 与主题/语言配置互不影响（语言权威存储见 I18n.cpp，主题走 FluentUI themeChanged）。
const QString kDefaultWorkDirKey = QStringLiteral("defaultWorkDir");

QString readDefaultWorkDir()
{
    return AppSettings::ini().value(kDefaultWorkDirKey).toString();
}

void writeDefaultWorkDir(const QString &value)
{
    AppSettings::ini().setValue(kDefaultWorkDirKey, value); // 空串=清除，读取侧 isEmpty 判缺省
}

// 未设置时的占位提示（浅色卡片右侧值区展示）
QString workDirDisplayText(const QString &stored)
{
    return stored.isEmpty() ? QObject::tr("未设置（使用进程当前目录）") : stored;
}

// 模型服务：settings.ini 键 apiBaseUrl / apiToken（QOpenAi::initFromSettings 读同源，
// 设置页写入后立即 QOpenAi::setUrl/setToken 覆盖全局 client，无需重启）。
// token 明文存 ini 属用户裁决：exe 同目录本机文件，属主可见可改（原环境变量方案已废弃）。
const QString kApiBaseUrlKey = QStringLiteral("apiBaseUrl");
const QString kApiTokenKey = QStringLiteral("apiToken");

QString readApiBaseUrl()
{
    return AppSettings::ini().value(kApiBaseUrlKey).toString();
}

void writeApiBaseUrl(const QString &value)
{
    AppSettings::ini().setValue(kApiBaseUrlKey, value); // 空串=清除，读取侧 isEmpty 判缺省
}

QString readApiToken()
{
    return AppSettings::ini().value(kApiTokenKey).toString();
}

void writeApiToken(const QString &value)
{
    AppSettings::ini().setValue(kApiTokenKey, value);
}

// 可选模型清单：settings.ini 键 modelOptions（逗号分隔）。解析与回退单源在
// AgentConst::modelOptions()（未配置/写空即内置两项）；输入框下拉在每次弹层展开前
// 重读该键，故本页改完配置无需重启即可选到新模型（见 ChatMsgEdit::reloadModelOptions）。
const QString kModelOptionsKey = QStringLiteral("modelOptions");

QString readModelOptionsRaw()
{
    return AgentConst::iniTextValue(kModelOptionsKey); // 手改不带引号（ini 列表语法）也读得回
}

void writeModelOptions(const QString &value)
{
    AppSettings::ini().setValue(kModelOptionsKey, value); // 空串=清除，读取侧回退内置清单
}

// 录入规范化：逐项去空白、丢空项、保序去重（重名会让下拉出现同文两项，选中歧义且
// 占位宽度白涨）；返回逗号分隔的落盘串。大小写原样保留——模型名大小写敏感
// （QOpenAi 直接把它塞进请求体 model 字段，改大小写可能 404）
QString normalizeModelOptions(const QString &input)
{
    // 口径单源：切分/去空/去重/条数上限全走 AgentConst::parseModelOptions（与下拉框读取侧
    // 同一实现），本卡不再自带一份解析——否则写侧不截断、读侧截断，两处规则必然漂移
    return AgentConst::parseModelOptions(input).join(QLatin1Char(','));
}

// Base URL 合法性：须为绝对 URL 且 scheme 为 http/https（QUrl 对裸主机名给出空 scheme，
// 天然落回拒绝）；比对 QOpenAi 端点拼接方式，无 scheme 的值写进去只会请求必失败
bool isHttpBaseUrl(const QString &input)
{
    const QUrl url(input);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

// API Key 值区脱敏摘要：长值取前 4 + 星 + 后 4；短值（<=8）一律固定四星——
// 否则前后各 4 会覆盖整个 Key，等于把明文摆上界面
QString maskedApiToken(const QString &stored)
{
    if (stored.length() <= 8)
        return QStringLiteral("****");
    return stored.left(4) + QStringLiteral("****") + stored.right(4);
}

} // namespace

// 默认工作目录设置卡：复用 FluSettingsSelectBox 外观（图标+标题+说明），
// 隐藏其右侧下拉框，替换为「路径值 + 修改 + 清除」操作行。
// 类声明在 SettingsPage.h（Q_OBJECT 上下文对齐译词条，注释见彼处）。
WorkDirSettingCard::WorkDirSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("默认工作目录"), tr("新建会话将继承该工作目录。"));
    setIcon(FluAwesomeType::Folder);
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText); // 路径按纯文本处理，避免被当作富文本解析
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
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
        writeDefaultWorkDir(dir);
        updateValue();
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        writeDefaultWorkDir(QString());
        updateValue();
    });
}

void WorkDirSettingCard::retranslate()
{
    setTitleInfo(tr("默认工作目录"), tr("新建会话将继承该工作目录。"));
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void WorkDirSettingCard::updateValue()
{
    const QString stored = readDefaultWorkDir();
    m_valueLabel->setText(workDirDisplayText(stored));
    m_valueLabel->setToolTip(stored);
}

// 上下文上限设置卡（第九轮）：同款 FluSettingsSelectBox 外观（图标+标题+说明），
// 隐藏下拉框换「数值 + 修改」操作行。展示/回写均经 AgentConst::contextCharLimitValue()
// 单点取值（未设置/非法自动回退默认 200000），与 CompactManager 消费侧同源不分叉；
// 写 settings.ini 后 CompactManager 下一回合管线现取即生效，无需重启。
ContextLimitSettingCard::ContextLimitSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("上下文上限（字符）"), tr("会话上下文超过该字符数时自动压缩。"));
    setIcon(FluAwesomeType::Trim); // 裁减语义
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText);
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
    row->addWidget(m_modifyButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
}

void ContextLimitSettingCard::retranslate()
{
    setTitleInfo(tr("上下文上限（字符）"), tr("会话上下文超过该字符数时自动压缩。"));
    m_modifyButton->setText(tr("修改"));
    updateValue();
}

void ContextLimitSettingCard::updateValue()
{
    // 千分位展示（c-locale 固定逗号分组，不随界面语言变）；编辑预填用裸数字防解析歧义。
    // 键语义保持字符域（规格修4：settings.ini 零新键零迁移）；括号内 token 仅为
    // 展示层派生提示 = contextCharLimit/4（计量预算口径，与侧栏/触发同源），不参与校验回写。
    const QLocale loc(QLocale::c());
    m_valueLabel->setText(tr("%1 字符（≈%2 token）")
                              .arg(loc.toString(AgentConst::contextCharLimitValue()),
                                   loc.toString(AgentConst::contextTokenBudget())));
}

void ContextLimitSettingCard::promptEdit()
{
    const QString rangeHint =
        tr("范围 %1 ~ %2（字符）。")
            .arg(AgentConst::kContextCharLimitMin)
            .arg(AgentConst::kContextCharLimitMax);
    // FluentInputDialog 约定 parent 传主窗口（遮罩铺满）；预填当前生效值（裸数字）
    const auto [input, accepted] = FluentInputDialog::getInputText(
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
    AppSettings::ini().setValue(AgentConst::kContextCharLimitKey, parsed);
    updateValue();
}

// 单轮最大调用次数设置卡（第十二轮）：ContextLimitSettingCard 同款结构。
// 展示/回写均经 AgentConst::maxToolIterationsValue() 单点取值（未设置/非法自动
// 回退默认 500），与 AgentLoop 回合入口快照同源不分叉；写 settings.ini 后下一回合生效。
// 数值域 [10,1000] 无需千分位，直接裸整数展示。
MaxRoundsSettingCard::MaxRoundsSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("单轮最大调用次数"), tr("限制单个回合内工具调用的最大轮数。"));
    setIcon(FluAwesomeType::Calculator); // 计数语义（todo 卡同款图标族，轮次即计数）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText);
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
    row->addWidget(m_modifyButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
}

void MaxRoundsSettingCard::retranslate()
{
    setTitleInfo(tr("单轮最大调用次数"), tr("限制单个回合内工具调用的最大轮数。"));
    m_modifyButton->setText(tr("修改"));
    updateValue();
}

void MaxRoundsSettingCard::updateValue()
{
    m_valueLabel->setText(QString::number(AgentConst::maxToolIterationsValue()));
}

void MaxRoundsSettingCard::promptEdit()
{
    const QString rangeHint =
        tr("范围 %1 ~ %2（轮）。")
            .arg(AgentConst::kMaxToolIterationsMin)
            .arg(AgentConst::kMaxToolIterationsMax);
    // FluentInputDialog 约定 parent 传主窗口（遮罩铺满）；预填当前生效值
    const auto [input, accepted] = FluentInputDialog::getInputText(
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
    AppSettings::ini().setValue(AgentConst::kMaxToolIterationsKey, parsed);
    updateValue();
}

// 服务地址设置卡：WorkDirSettingCard 同款外观与操作行（图标+标题+说明，右侧「值 + 修改 + 清除」）。
// 编辑走 FluentInputDialog 单行输入（parent 传主窗口保遮罩铺满），预填存量值；
// 校验不过弹 FluMessageBox 拒写，空串按清除处理。
ApiUrlSettingCard::ApiUrlSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("服务地址"), tr("OpenAI 兼容 API 的基础 URL（如 https://api.example.com/v1）。"));
    setIcon(FluAwesomeType::Link); // 端点链接语义（Globe 已被语言卡占用）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText); // URL 按纯文本处理，避免被当作富文本解析
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        writeApiBaseUrl(QString());
        QOpenAi::setUrl(QString()); // 即时生效：下一回合请求走空配置报错路径
        updateValue();
    });
}

void ApiUrlSettingCard::retranslate()
{
    setTitleInfo(tr("服务地址"), tr("OpenAI 兼容 API 的基础 URL（如 https://api.example.com/v1）。"));
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ApiUrlSettingCard::updateValue()
{
    const QString stored = readApiBaseUrl();
    m_valueLabel->setText(stored.isEmpty() ? tr("未设置") : stored);
    m_valueLabel->setToolTip(stored);
}

void ApiUrlSettingCard::promptEdit()
{
    const auto [input, accepted] = FluentInputDialog::getInputText(
        window(), tr("设置服务地址"), tr("以 /v1 等版本路径结尾，不含补全端点。"),
        readApiBaseUrl());
    if (!accepted)
        return; // 取消：保持原值
    const QString cleaned = input.trimmed();
    if (!cleaned.isEmpty() && !isHttpBaseUrl(cleaned))
    {
        // 非法 URL 拒绝并提示，不落盘（原值继续生效）
        FluMessageBox(tr("无效地址"),
                      tr("请输入以 http:// 或 https:// 开头的完整地址。"),
                      window())
            .exec();
        return;
    }
    writeApiBaseUrl(cleaned);
    QOpenAi::setUrl(cleaned); // 即时生效，无需重启
    updateValue();
}

// API Key 设置卡：结构与 URL 卡一致，差异全在「机密值」处理——值区只显脱敏摘要、
// tooltip 不带明文；编辑框预填存量明文（本机 ini 属主可见可改，属用户裁决），
// 提示行明确保存即覆盖旧值。
ApiTokenSettingCard::ApiTokenSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("API Key"), tr("Bearer Token，明文保存于 exe 同目录 settings.ini。"));
    setIcon(FluAwesomeType::Lock); // 凭据语义（枚举表无 Key 项）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText);
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位）

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        writeApiToken(QString());
        QOpenAi::setToken(QString()); // 即时生效：下一回合请求走空配置报错路径
        updateValue();
    });
}

void ApiTokenSettingCard::retranslate()
{
    setTitleInfo(tr("API Key"), tr("Bearer Token，明文保存于 exe 同目录 settings.ini。"));
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ApiTokenSettingCard::updateValue()
{
    const QString stored = readApiToken();
    // 明文绝不上屏：值区只给摘要，tooltip 只给状态（不复制完整 key 到悬停提示）
    m_valueLabel->setText(stored.isEmpty() ? tr("未设置") : maskedApiToken(stored));
    m_valueLabel->setToolTip(stored.isEmpty() ? QString() : tr("已保存"));
}

void ApiTokenSettingCard::promptEdit()
{
    const auto [input, accepted] = FluentInputDialog::getInputText(
        window(), tr("设置 API Key"), tr("留空表示清除；保存将覆盖已配置的 Key。"), readApiToken());
    if (!accepted)
        return; // 取消：保持原值
    const QString cleaned = input.trimmed();
    writeApiToken(cleaned);
    QOpenAi::setToken(cleaned); // 即时生效（空串=清除），无需重启
    updateValue();
}

// 可选模型清单设置卡：与 URL / Key 卡同款结构（图标+标题+说明，右侧「值 + 修改 + 清除」）。
// 值区展示**生效清单**（含未配置时的内置回退），而非原始配置串——所见即下拉所得。
// 生效路径：本卡只写 settings.ini，不持有也不引用 ChatMsgEdit（两控件解耦）；输入框下拉
// 在每次弹层展开前重读该键并原地刷新，故改完配置无需重启即可选到新模型。
ModelListSettingCard::ModelListSettingCard(QWidget *parent)
    : FluSettingsSelectBox(parent)
{
    setTitleInfo(tr("可选模型"), tr("输入框模型下拉的候选清单，逗号分隔；留空即内置默认项。"));
    setIcon(FluAwesomeType::ReadingList); // 清单语义（Link / Lock 已用于同组两卡）
    getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

    m_valueLabel = new FluLabel(this);
    m_valueLabel->setTextFormat(Qt::PlainText); // 模型名按纯文本处理，避免被当作富文本解析
    m_valueLabel->setMaximumWidth(320);
    m_valueLabel->setMinimumWidth(0);
    m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_modifyButton = new FluPushButton(tr("修改"), this);
    m_modifyButton->setFixedSize(64, 30);
    m_clearButton = new FluPushButton(tr("清除"), this);
    m_clearButton->setFixedSize(64, 30);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_valueLabel);
    row->addWidget(m_modifyButton);
    row->addWidget(m_clearButton);
    m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

    updateValue();

    connect(m_modifyButton, &QPushButton::clicked, this, [this]() { promptEdit(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        writeModelOptions(QString()); // 即时生效：下拉下次展开回退内置清单
        updateValue();
    });
}

void ModelListSettingCard::retranslate()
{
    setTitleInfo(tr("可选模型"), tr("输入框模型下拉的候选清单，逗号分隔；留空即内置默认项。"));
    m_modifyButton->setText(tr("修改"));
    m_clearButton->setText(tr("清除"));
    updateValue();
}

void ModelListSettingCard::updateValue()
{
    const QString stored = readModelOptionsRaw().trimmed();
    const QString shown = AgentConst::modelOptions().join(QStringLiteral(", "));
    if (stored.isEmpty())
    {
        // 未配置：值区仍展示回退后的内置清单（下拉里就是这些），并标注来源——留空不等于
        // 「没有模型可选」，直接把空串摆上界面会让人以为功能坏了
        m_valueLabel->setText(tr("内置默认：%1").arg(shown));
        m_valueLabel->setToolTip(tr("未配置 modelOptions 键，可用本卡「修改」填写。"));
    }
    else
    {
        m_valueLabel->setText(shown); // 展示清洗去重后的生效值，非原始录入串
        m_valueLabel->setToolTip(tr("settings.ini: %1").arg(stored));
    }
}

void ModelListSettingCard::promptEdit()
{
    const auto [input, accepted] = FluentInputDialog::getInputText(
        window(), tr("设置可选模型"), tr("多个模型用英文逗号分隔，如 qwen3.8-flash,qwen3.8-max。"),
        readModelOptionsRaw());
    if (!accepted)
        return; // 取消：保持原值
    // 清洗后全空（只输了逗号或空白）按清除处理而非报错：语义与「清除」按钮一致，
    // 且空清单本就不该落盘——下拉无项可展，AgentLoop 白名单也会把所有模型判成回落
    writeModelOptions(normalizeModelOptions(input));
    updateValue();
}

SettingsPage::SettingsPage(QWidget *parent) : BasePage(parent)
{
    auto vMainLayout = new QVBoxLayout(this);
    vMainLayout->setContentsMargins(35, 35, 35, 35);
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

    /// model service（LLM 服务地址 / API Key 可设置，取代原环境变量；写 ini 后即时覆盖全局配置）
    m_modelLabel = new FluLabel;
    m_modelLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    m_modelLabel->setText(tr("模型服务"));
    scrollView->getMainLayout()->addWidget(m_modelLabel, 0, Qt::AlignTop);

    // 同组三卡紧邻不加 addSpacing：与「外观与行为」组（主题盒 + 语言盒）既有排布同款
    m_apiUrlCard = new ApiUrlSettingCard;
    scrollView->getMainLayout()->addWidget(m_apiUrlCard, 0, Qt::AlignTop);

    m_apiTokenCard = new ApiTokenSettingCard;
    scrollView->getMainLayout()->addWidget(m_apiTokenCard, 0, Qt::AlignTop);

    m_modelListCard = new ModelListSettingCard;
    scrollView->getMainLayout()->addWidget(m_modelListCard, 0, Qt::AlignTop);


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
    // "0.0.1" 与 CMake 版本脱节，本案修结；现值 = project VERSION 拼 s 前缀（如 s12.6）
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

    m_modelLabel->setText(tr("模型服务"));
    if (m_apiUrlCard)
        m_apiUrlCard->retranslate();
    if (m_apiTokenCard)
        m_apiTokenCard->retranslate();
    if (m_modelListCard)
        m_modelListCard->retranslate();

    m_aboutLabel->setText(tr("关于"));
    m_versionBox->getInfoLabel()->setText(tr("@2026 lite harness. 保留所有权利。"));
    m_infoLabel->setText(
        tr("LiteHarness 是一款轻量级的 C++ 编码代理 harness 应用，旨在填补 C++ 生态中 harness 实现的空白。"
           "它作为一个动手学习项目，逐步演示如何使用 Qt 与现代 C++ 从零构建一个 harness。"));
}
