#pragma once

#include "BasePage.h"

#include <FluSettingsSelectBox.h>

class FluLabel;
class FluPushButton;
class FluSettingsVersionBox;
class QEvent;
class QLabel;

// 默认工作目录设置卡（实现在 SettingsPage.cpp，复用其匿名命名空间的 settings.ini 读写助手）。
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

// 上下文上限设置卡（第九轮）：数值展示 + 「修改」弹 FluentInputDialog 输入，
// 校验 [kContextCharLimitMin, kContextCharLimitMax] 拒绝非法值，写 settings.ini 后立即回显。
// 生效语义：CompactManager 每次管线现取设置值，下一回合生效，无需重启。
// 与 WorkDirSettingCard 同理：置于头文件带 Q_OBJECT，保证 tr() 运行期上下文
// 与 lupdate 提取上下文一致，否则译文永不命中。
class ContextLimitSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    explicit ContextLimitSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    QLabel* m_valueLabel = nullptr;
    FluPushButton* m_modifyButton = nullptr;
};

// 单轮最大调用次数设置卡（第十二轮）：与 ContextLimitSettingCard 同款结构——
// 数值展示 + 「修改」弹 FluentInputDialog 输入，校验 [kMaxToolIterationsMin,
// kMaxToolIterationsMax] 拒绝非法值，写 settings.ini 后立即回显。
// 生效语义：AgentLoop 每回合 run() 入口现取设置值，下一回合生效，无需重启。
// Q_OBJECT 理由同上两卡（tr 上下文与 lupdate 提取对齐）。
class MaxRoundsSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    explicit MaxRoundsSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    QLabel* m_valueLabel = nullptr;
    FluPushButton* m_modifyButton = nullptr;
};

// 服务地址设置卡（模型服务组）：WorkDirSettingCard 同款结构——值区 + 「修改 + 清除」操作行，
// 编辑弹 FluentInputDialog 录入 OpenAI 兼容 API 基础 URL，校验 scheme 为 http/https，
// 非法值 FluMessageBox 拒绝不落盘；空串视为清除。
// 生效语义：写 settings.ini 键 apiBaseUrl 后立即 QOpenAi::setUrl()，下一回合请求即用新值。
// 值区明文展示（URL 非机密）。
// Q_OBJECT 理由同前三卡（tr 上下文与 lupdate 提取词法类名对齐）。
class ApiUrlSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    explicit ApiUrlSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    QLabel* m_valueLabel = nullptr;
    FluPushButton* m_modifyButton = nullptr;
    FluPushButton* m_clearButton = nullptr;
};

// API Key 设置卡（模型服务组）：与 ApiUrlSettingCard 同结构，但值区**永不显示明文**——
// 长值只展示前 4 + 星 + 后 4 的脱敏摘要（短值固定四星），tooltip 只提示「已保存」。
// 编辑框预填当前存量明文（本机 settings.ini 属主可见可改，属用户裁决），空串=清除。
// 生效语义：写 settings.ini 键 apiToken 后立即 QOpenAi::setToken()，下一回合请求即用新值。
class ApiTokenSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    explicit ApiTokenSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

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
    FluLabel* m_contextLabel = nullptr;
    ContextLimitSettingCard* m_contextCard = nullptr;
    FluLabel* m_maxRoundsLabel = nullptr;
    MaxRoundsSettingCard* m_maxRoundsCard = nullptr;
    FluLabel* m_modelLabel = nullptr;
    ApiUrlSettingCard* m_apiUrlCard = nullptr;
    ApiTokenSettingCard* m_apiTokenCard = nullptr;
    FluLabel* m_aboutLabel = nullptr;
    FluSettingsVersionBox* m_versionBox = nullptr;
    FluLabel* m_infoLabel = nullptr;
};
