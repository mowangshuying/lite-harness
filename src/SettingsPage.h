#pragma once

#include "BasePage.h"

#include <FluSettingsSelectBox.h>

class FluLabel;
class FluPushButton;
class FluSettingsVersionBox;
class QEvent;

// 说明位值卡基类：把「简短解释：值」渲染进基类 m_infoLabel（标题下方那一格），右侧操作行
// 只留按钮。原「右侧值区 + 说明位解释文案」两处分置的写法，在此统一成一条说明位文本，
// 长值不再与按钮抢横向空间。
// 省略口径沿用 WorkDirPathBar / 原 WorkDirSettingCard：minimumWidth(0) + Expanding +
// ElideMiddle + ToolTip 全量 + Resize 重算。QLabel 不会自动省略，不设 minimumWidth(0)
// 时长值 sizeHint 会把操作按钮挤出卡片（默认 Preferred 虽含 ShrinkFlag，仍受
// minimumSizeHint 顶住，窄宽下顶不住）。
// **只对值部分省略，解释前缀恒完整**：若对「前缀：值」整串做 ElideMiddle，省略点会落在
// 前缀与值的交界上，等于同时毁掉说明和值。
// 带 Q_OBJECT 的唯一理由：分隔符要 tr()。中文用全角「：」，英文须回落半角 ": "，否则英文界面
// 出现 CJK 宽标点。本类是六卡唯一共用分隔符产出点，放此处一条词条即可覆盖全组（若下放到各卡，
// 同一字符串会按六个词法类名生成六条重复词条）。tr() 上下文对齐要求同下：类必须带 Q_OBJECT。
class InfoSlotSettingCard : public FluSettingsSelectBox
{
    Q_OBJECT
public:
    // prefix 为空即纯值展示（工作目录卡）；toolTip 为空即回落「prefix：值」全量做省略兜底。
    // 机密值须由调用方保证传入的 value 已脱敏——基类会把 value 原样写进 ToolTip。
    void setInfoValue(const QString& prefix, const QString& value,
                      const QString& toolTip = QString());

protected:
    explicit InfoSlotSettingCard(QWidget* parent = nullptr);

    // m_infoLabel 的 Resize：可用宽度变化即重算中间省略
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshInfoSlot();

    QString m_prefix; // 简短解释（恒完整，不参与省略）
    QString m_value;  // 值本体（按剩余宽度中间省略）
    QString m_toolTip; // 空=回落「prefix：值」全量；非空=调用方指定（如机密状态、原始录入串）
};

// 默认工作目录设置卡（实现在 SettingsPage.cpp，复用其匿名命名空间的 settings.ini 读写助手）。
// 说明位展示当前路径，**无解释前缀**（原「新建会话将继承该工作目录。」提示按需求移除）。
// i18n 第八轮：必须置于头文件并带 Q_OBJECT——若无 Q_OBJECT，成员里的 tr() 会静态绑定到
// 最近祖先的宏生成 tr，运行期翻译上下文变成基类，而 lupdate 按词法类名提取，两者错位
// 导致词条永不命中。
class WorkDirSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit WorkDirSettingCard(QWidget* parent = nullptr);

    // LanguageChange 重译：setTitleInfo 只回填标题（说明位归值管），按钮文案重取 tr()，
    // 路径与占位文案随 updateValue 重算（含中间省略）
    void retranslate();

private:
    void updateValue();

    FluPushButton* m_modifyButton = nullptr;
    FluPushButton* m_clearButton = nullptr;
};

// 上下文上限设置卡（第九轮）：说明位「超过即自动压缩：200,000 字符（≈50,000 token）」，
// 右侧只留「修改」，弹 FluentInputDialog 输入，校验 [kContextCharLimitMin,
// kContextCharLimitMax] 拒绝非法值，写 settings.ini 后立即回显。
// 生效语义：CompactManager 每次管线现取设置值，下一回合生效，无需重启。
// Q_OBJECT 理由同 WorkDirSettingCard（tr 上下文与 lupdate 提取词法类名对齐）。
class ContextLimitSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit ContextLimitSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    FluPushButton* m_modifyButton = nullptr;
};

// 单轮最大调用次数设置卡（第十二轮）：与 ContextLimitSettingCard 同款结构——说明位
// 「超限即终止循环：500」+「修改」弹 FluentInputDialog 输入，校验 [kMaxToolIterationsMin,
// kMaxToolIterationsMax] 拒绝非法值，写 settings.ini 后立即回显。
// 生效语义：AgentLoop 每回合 run() 入口现取设置值，下一回合生效，无需重启。
// 「终止循环」非拟测：撞上限走 AgentLoopRequest.cpp 的
// error(「工具调用轮次超过上限（N 轮），终止循环。」) 分支。
// Q_OBJECT 理由同上两卡。
class MaxRoundsSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit MaxRoundsSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    FluPushButton* m_modifyButton = nullptr;
};

// 服务地址设置卡（模型服务组）：说明位「OpenAI 兼容基址：<url>」+「修改 + 清除」操作行。
// 编辑弹 FluentInputDialog 录入 OpenAI 兼容 API 基础 URL，校验 scheme 为 http/https，
// 非法值 FluMessageBox 拒绝不落盘；空串视为清除。
// 生效语义：写 settings.ini 键 apiBaseUrl 后立即 QOpenAi::setUrl()，下一回合请求即用新值。
// 明文展示（URL 非机密），ToolTip 回落全量 URL 供省略后读全文。
// Q_OBJECT 理由同前三卡。
class ApiUrlSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit ApiUrlSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    FluPushButton* m_modifyButton = nullptr;
    FluPushButton* m_clearButton = nullptr;
};

// API Key 设置卡（模型服务组）：说明位「Bearer 凭据：<脱敏摘要>」+「修改 + 清除」操作行。
// **明文绝不上屏**：值只给 maskedApiToken 摘要（前4 + **** + 后4，长度≤8 固定四星），
// 且 ToolTip 回落的也是同一摘要——基类会把 value 原样写进 ToolTip，故进说明位的必须
// 已是摘要，绝不能把 stored 明文交给基类。
// 编辑框不预填存量明文 + Password 回显；留空=不修改，清除另有「清除」按钮（职责不重叠）。
// 生效语义：写 settings.ini 键 apiToken 后立即 QOpenAi::setToken()，下一回合请求即用新值。
class ApiTokenSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit ApiTokenSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

    FluPushButton* m_modifyButton = nullptr;
    FluPushButton* m_clearButton = nullptr;
};

// 可选模型清单设置卡（模型服务组）：说明位「输入框下拉候选：<清单>」+「修改 + 清除」操作行。
// 未配置时前缀换成「内置默认候选」——否则会拼成「下拉候选：内置默认：…」双冒号。
// 编辑弹 FluentInputDialog 录入逗号分隔模型名（落盘前逐项 trim、丢空、保序去重）。
// 生效语义：写 settings.ini 键 modelOptions；输入框模型下拉在每次弹层展开前重读该键并原地
// 刷新（ChatMsgEdit::reloadModelOptions），故改完配置无需重启即可选到新模型。
// 说明位展示的是**生效清单**（清洗去重后），非原始录入串；原始串经 ToolTip 兜底（两者可能不同）。
// Q_OBJECT 理由同前四卡。
class ModelListSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit ModelListSettingCard(QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    void promptEdit();

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
    ModelListSettingCard* m_modelListCard = nullptr;
    FluLabel* m_aboutLabel = nullptr;
    FluSettingsVersionBox* m_versionBox = nullptr;
    FluLabel* m_infoLabel = nullptr;
};
