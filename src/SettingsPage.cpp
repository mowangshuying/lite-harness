#include "SettingsPage.h"
#include <FluUtils.h>
#include <QVBoxLayout>
#include <FluVScrollView.h>
#include <FluSettingsSelectBox.h>
#include <FluLabel.h>
#include <FluSettingsVersionBox.h>

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
    settingsVersionBox->getIconLabel()->setPixmap(appIcon.pixmap(QSize(24, 24)));

    auto infoLabel = new FluLabel;
    infoLabel->setWordWrap(true);
    infoLabel->setLabelStyle(FluLabelStyle::BodyTextBlockStyle);
    infoLabel->setText(
        tr("LiteHarness is a lightweight C++ harness application, designed to fill the gap of harness implementations in the C++ ecosystem. "
           "It serves as a hands-on learning project that demonstrates, step by step, how to build a harness from the ground up using Qt and modern C++."));
    settingsVersionBox->addWidget(infoLabel);


    scrollView->getMainLayout()->addWidget(settingsVersionBox, 0, Qt::AlignTop);
}

void SettingsPage::onThemeChanged()
{
    BasePage::onThemeChanged();
    FluStyleSheetUtils::setQssByFileName("SettingsPage.qss", this, FluThemeUtils::getUtils()->getTheme());
}