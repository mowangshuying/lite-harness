#pragma once

#include "BasePage.h"

#include <FluSettingsSelectBox.h>

class FluLabel;
class FluPushButton;
class FluSettingsVersionBox;
class QEvent;
class QLabel;

// 默认工作目录设置卡（实现在 SettingsPage.cpp，复用其匿名命名空间的 QSettings 助手）。
// i18n 第八轮：必须置于头文件并带 Q_OBJECT——若无 Q_OBJECT，成员里的 tr() 会静态绑定到
// 最近祖先的宏生成 tr，运行期翻译上下文变成基类 "FluSettingsSelectBox"，而 lupdate 按
// 词法类名提取为 "WorkDirSettingCard"，两者错位导致词条永不命中。
class WorkDirSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    explicit WorkDirSettingCard(QWidget* parent = nullptr);

    // LanguageChange 重译：setTitleInfo 实证为就地刷新（FluSettingsSelectBox.cpp），
    // 按钮文案重取 tr()，值区占位文本随 updateValue 重算
    void retranslate();

private:
    void updateValue();

    QLabel* m_valueLabel = nullptr;
    FluPushButton* m_modifyButton = nullptr;
    FluPushButton* m_clearButton = nullptr;
};

class SettingsPage : public BasePage
{
    Q_OBJECT
public:
    SettingsPage(QWidget* parent = nullptr);

protected:
    // i18n 第八轮：常驻页面，LanguageChange 时整页重译（combo 弹出项/卡片说明
    // 均构造期定值，不重译则滞留旧语言）
    void changeEvent(QEvent* event) override;

private:
    void retranslateUi();

    // 常驻文案控件引用（重译面）；局部布局/滚动视图不持文案，无需引用
    FluLabel* m_appearanceLabel = nullptr;
    FluSettingsSelectBox* m_appThemeBox = nullptr;
    FluSettingsSelectBox* m_languageBox = nullptr;
    FluLabel* m_workDirLabel = nullptr;
    WorkDirSettingCard* m_workDirCard = nullptr;
    FluLabel* m_aboutLabel = nullptr;
    FluSettingsVersionBox* m_versionBox = nullptr;
    FluLabel* m_infoLabel = nullptr;
};
