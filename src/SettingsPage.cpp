#include "SettingsPage.h"
#include "AgentConstants.h"
#include "FluentInputDialog.h"
#include "I18n.h"
#include "ThemeAware.h"
#include <FluUtils.h>
#include <FluMessageBox.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QLocale>
#include <QSettings>
#include <QFileInfo>
#include <QFileDialog>
#include <FluVScrollView.h>
#include <FluLabel.h>
#include <FluSettingsVersionBox.h>
#include <FluPushButton.h>

namespace {

// 默认工作目录：QSettings 用法，组织/应用名已在 App.cpp 全局设定（LiteHarness/LiteHarness），
// 默认构造命中与旧显式双参构造相同的注册表键；与主题/语言配置互不影响
// （语言权威存储见 I18n.cpp，主题走 FluentUI themeChanged）。
const QString kDefaultWorkDirKey = QStringLiteral("defaultWorkDir");

QString readDefaultWorkDir()
{
    QSettings settings;
    return settings.value(kDefaultWorkDirKey).toString();
}

void writeDefaultWorkDir(const QString &value)
{
    QSettings settings;
    settings.setValue(kDefaultWorkDirKey, value); // 空串=清除，读取侧 isEmpty 判缺省
}

// 未设置时的占位提示（浅色卡片右侧值区展示）
QString workDirDisplayText(const QString &stored)
{
    return stored.isEmpty() ? QObject::tr("未设置（使用进程当前目录）") : stored;
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
// 写注册表后 CompactManager 下一回合管线现取即生效，无需重启。
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
    // 千分位展示（c-locale 固定逗号分组，不随界面语言变）；编辑预填用裸数字防解析歧义
    m_valueLabel->setText(QLocale(QLocale::c()).toString(AgentConst::contextCharLimitValue()));
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
    QSettings settings;
    settings.setValue(AgentConst::kContextCharLimitKey, parsed);
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
    connect(m_appThemeBox->getComboBox(), &FluComboBox::currentIndexChanged, [=](int index) {
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
    // "0.0.1" 与 CMake 0.1.0 脱节，本案修结
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

    m_aboutLabel->setText(tr("关于"));
    m_versionBox->getInfoLabel()->setText(tr("@2026 lite harness. 保留所有权利。"));
    m_infoLabel->setText(
        tr("LiteHarness 是一款轻量级的 C++ 编码代理 harness 应用，旨在填补 C++ 生态中 harness 实现的空白。"
           "它作为一个动手学习项目，逐步演示如何使用 Qt 与现代 C++ 从零构建一个 harness。"));
}
