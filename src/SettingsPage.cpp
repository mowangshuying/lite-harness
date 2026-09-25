#include "SettingsPage.h"
#include "ThemeAware.h"
#include <FluUtils.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QFileInfo>
#include <QFileDialog>
#include <FluVScrollView.h>
#include <FluSettingsSelectBox.h>
#include <FluLabel.h>
#include <FluSettingsVersionBox.h>
#include <FluPushButton.h>

namespace {

// 默认工作目录：QSettings 用法，组织/应用名已在 App.cpp 全局设定（LiteHarness/LiteHarness），
// 默认构造命中与旧显式双参构造相同的注册表键；与主题/语言配置互不影响
// （themeChanged、setLanguage 走 FluentUI 自身机制，非此处）。
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

// 默认工作目录设置卡：复用 FluSettingsSelectBox 外观（图标+标题+说明），
// 隐藏其右侧下拉框，替换为「路径值 + 修改 + 清除」操作行。不新增 Q_OBJECT（
// 基类已 moc；本类仅构造期一次性装配，无需自身信号）。
class WorkDirSettingCard : public FluSettingsSelectBox
{
public:
    explicit WorkDirSettingCard(QWidget *parent = nullptr)
        : FluSettingsSelectBox(parent)
    {
        setTitleInfo(tr("默认工作目录"), tr("新建会话将继承该工作目录。"));
        setIcon(FluAwesomeType::Folder);
        getComboBox()->hide(); // 本卡不用下拉，右侧改放自定义操作行

        m_valueLabel = new QLabel(this);
        m_valueLabel->setTextFormat(Qt::PlainText); // 路径按纯文本处理，避免被当作富文本解析
        m_valueLabel->setMaximumWidth(320);
        m_valueLabel->setMinimumWidth(0);
        m_valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *modifyButton = new FluPushButton(tr("修改"), this);
        modifyButton->setFixedSize(64, 30);
        auto *clearButton = new FluPushButton(tr("清除"), this);
        clearButton->setFixedSize(64, 30);

        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->addWidget(m_valueLabel);
        row->addWidget(modifyButton);
        row->addWidget(clearButton);
        m_mainLayout->addLayout(row, 0); // 追加到卡片右侧（原下拉框位），保持图标/标题布局不变

        updateValue();

        connect(modifyButton, &QPushButton::clicked, this, [this]() {
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
        connect(clearButton, &QPushButton::clicked, this, [this]() {
            writeDefaultWorkDir(QString());
            updateValue();
        });
    }

private:
    void updateValue()
    {
        const QString stored = readDefaultWorkDir();
        m_valueLabel->setText(workDirDisplayText(stored));
        m_valueLabel->setToolTip(stored);
    }

    QLabel *m_valueLabel = nullptr;
};

} // namespace

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
    auto appearanceAndBehaviorLabel = new FluLabel;
    appearanceAndBehaviorLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    appearanceAndBehaviorLabel->setText(tr("Appearance & Behavior"));
    scrollView->getMainLayout()->addWidget(appearanceAndBehaviorLabel, 0, Qt::AlignTop);


    /// app Theme;
    auto appThemeBox = new FluSettingsSelectBox;
    appThemeBox->setTitleInfo(tr("App theme"), tr("Select which app theme to display."));
    appThemeBox->setIcon(FluAwesomeType::Color);
    appThemeBox->getComboBox()->addItem(tr("Light"));
    appThemeBox->getComboBox()->addItem(tr("Dark"));
    appThemeBox->getComboBox()->addItem(tr("AtomOneDark"));
    appThemeBox->getComboBox()->setCurrentIndex((int)FluThemeUtils::getUtils()->getTheme());
    connect(appThemeBox->getComboBox(), &FluComboBox::currentIndexChanged, [=](int index) {
        if (index == (int)FluThemeUtils::getUtils()->getTheme())
            return;

        if (index == 0)
            FluThemeUtils::getUtils()->setTheme(FluTheme::Light);
        else if (index == 1)
            FluThemeUtils::getUtils()->setTheme(FluTheme::Dark);
        else
            FluThemeUtils::getUtils()->setTheme(FluTheme::AtomOneDark);
    });

    scrollView->getMainLayout()->addWidget(appThemeBox, 0, Qt::AlignTop);

    /// language;
    auto languageSelectBox = new FluSettingsSelectBox;
    languageSelectBox->setTitleInfo(tr("Language"), tr("Select which language to display."));
    languageSelectBox->setIcon(FluAwesomeType::Globe);
    languageSelectBox->getComboBox()->addItem(tr("en-US"));
    languageSelectBox->getComboBox()->addItem(tr("zh-CN"));

    if (FluConfigUtils::getUtils()->getLanguage() == "en-US")
        languageSelectBox->getComboBox()->setCurrentIndex(0);
    else if (FluConfigUtils::getUtils()->getLanguage() == "zh-CN")
        languageSelectBox->getComboBox()->setCurrentIndex(1);

    connect(languageSelectBox->getComboBox(), &FluComboBox::currentIndexChanged, [=](int index) {
        if (index == 0)
            FluConfigUtils::getUtils()->setLanguage("en-US");
        else if (index == 1)
            FluConfigUtils::getUtils()->setLanguage("zh-CN");
    });

    scrollView->getMainLayout()->addWidget(languageSelectBox, 0, Qt::AlignTop);


    /// work directory;
    auto workDirLabel = new FluLabel;
    workDirLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    workDirLabel->setText(tr("工作目录"));
    scrollView->getMainLayout()->addWidget(workDirLabel, 0, Qt::AlignTop);

    auto workDirCard = new WorkDirSettingCard;
    scrollView->getMainLayout()->addWidget(workDirCard, 0, Qt::AlignTop);


    //// add spacing
    scrollView->getMainLayout()->addSpacing(20);

    /// about
    auto aboutLabel = new FluLabel;
    aboutLabel->setLabelStyle(FluLabelStyle::BodyStrongTextBlockStyle);
    aboutLabel->setText(tr("About"));
    scrollView->getMainLayout()->addWidget(aboutLabel, 0, Qt::AlignTop);

    /// version;
    // auto settingsVersionBox = new FluSettingsSelectBox;
    auto settingsVersionBox = new FluSettingsVersionBox;
    settingsVersionBox->getTitleLabel()->setText(tr("lite-harness"));
    settingsVersionBox->getInfoLabel()->setText(tr("@2026 lite harness. All rights reserved."));
    settingsVersionBox->getVersionLabel()->setText(tr("0.0.1"));

    QIcon appIcon = QIcon(":/res/LiteHarness.ico");
    settingsVersionBox->getIconLabel()->setPixmap(appIcon.pixmap(QSize(45, 45)));

    auto infoLabel = new FluLabel;
    infoLabel->setWordWrap(true);
    infoLabel->setLabelStyle(FluLabelStyle::BodyTextBlockStyle);
    infoLabel->setText(
        tr("LiteHarness is a lightweight C++ harness application, designed to fill the gap of harness implementations in the C++ ecosystem. "
           "It serves as a hands-on learning project that demonstrates, step by step, how to build a harness from the ground up using Qt and modern C++."));
    settingsVersionBox->addWidget(infoLabel);


    scrollView->getMainLayout()->addWidget(settingsVersionBox, 0, Qt::AlignTop);

    // QSS 首刷 + themeChanged 订阅收敛到 ThemeAware::bind。
    // 修正既有缺陷：原构造不首刷 SettingsPage.qss，须等首次主题切换才生效
    ThemeAware::bind("SettingsPage.qss", this);
}