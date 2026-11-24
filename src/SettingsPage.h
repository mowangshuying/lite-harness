#pragma once

#include "BasePage.h"
// s17.12：槽签名用了 UpdateManager::Stage 嵌套枚举，前向声明不够（枚举需要完整类型），
// 只能真 include。本头因此间接带进 QObject，与既有 BasePage 同量级，不新增链接依赖。
#include "UpdateManager.h"
// s18.3 多 Provider：ModelServiceTarget 的接口按值收发 QList<ProviderSpec::Provider>，
// 前向声明不够（返回类型需完整），只能真 include。该头纯 QtCore，不新增链接依赖。
#include "ProviderSpec.h"

#include <FluSettingsSelectBox.h>

class FluLabel;
class FluPushButton;
class FluProgressRing; // s17.12：下载进度环，只在 UpdateSettingCard 里 new，头侧指针即可
class FluSettingsVersionBox;
class ModelServiceTarget;
class QEvent;
class QShowEvent; // s17.12：showEvent 形参类型；BasePage 未覆写该钩子，此处只需前向声明

// provider 机器码 → 用户文案单源（s18.5 提为跨 TU：模型管理弹窗复用，定义在 SettingsPage.cpp）
QString providerErrorText(const QString &error);

// API Key 脱敏摘要单源（模型管理弹窗复用）：长值前 4 + **** + 后 4，短值固定四星。
QString maskedApiToken(const QString &stored);

// 以「改当前 provider 的一个字段」形态提交整表；失败已弹错，返回 false 调用方放弃刷新。
bool commitEditedCurrent(ModelServiceTarget *target, const ProviderSpec::Provider &edited,
                         QWidget *anchor);

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

// 默认工作目录设置卡（实现在 SettingsPage.cpp，复用其匿名命名空间的配置文件读写助手）。
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

// 上下文上限设置卡（第九轮；s17.9 起单位定为 token）：说明位「超限自动压缩：200,000 token」，
// 右侧只留「修改」，弹 InputDialog 输入，校验 [kContextCharLimitMin,
// kContextCharLimitMax] 拒绝非法值，写 config.json 后立即回显。
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
// 「超限即终止循环：500」+「修改」弹 InputDialog 输入，校验 [kMaxToolIterationsMin,
// kMaxToolIterationsMax] 拒绝非法值，写 config.json 后立即回显。
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

// 模型服务组共享的「当前在编辑哪个服务商」。
//
// 为什么要有这个对象：s18.3 多 Provider 之前，端点/密钥/模型清单各自直读写三个全局
// 键（apiBaseUrl/apiToken/modelOptions），互不知情是安全的——反正全进程只有一份凭据。
// 有了 provider 之后，「当前编辑哪家的地址/密钥/模型」成了共同前提，若各处自己存一份
// 选中态，切一家后必然出现「地址已是乙、密钥还是甲」的错配写盘——那会静默把甲的 key
// 配到乙的端点上。故选中态单点持有，模型管理弹窗只读它、并由 targetChanged 统一重画。
//
// providerId 为空串 = 「旧三键形态」：config.json 未配 app/providers 时的兼容路径。
// 此时退回改造前的全局键读写，**未迁移用户的行为逐字不变**，不是第三种状态。
class ModelServiceTarget : public QObject
{
    Q_OBJECT
public:
    explicit ModelServiceTarget(QObject* parent = nullptr);

    QString providerId() const { return m_providerId; }
    void setProviderId(const QString& id); // 同值早退，不发信号

    // 每次现取盘（SettingsCache 已按文件身份缓存，热改即生效），不缓存副本——
    // 缓存了就会与「手改 config.json 下一次取值即生效」的既有语义打架。
    QList<ProviderSpec::Provider> providers() const;
    // 选中项下标；未选中或表空返回 -1。
    int currentIndex() const;
    // 选中项本体；无选中返回空表形态的 Provider（各字段空）。
    ProviderSpec::Provider current() const;

    // 落盘整表并广播重画。返回 false = 写失败（已弹「保存失败」，调用方不必再提示）。
    // anchor 必须由调用方（某张卡）传 window()：本类是 QObject 而非 QWidget，自己取不到
    // 主窗口，而 FluMessageBox 约定 parent 不可为 null。
    bool commit(const QList<ProviderSpec::Provider>& next, QWidget* anchor);

signals:
    void targetChanged();

private:
    QString m_providerId;
};

// 可选模型清单设置卡（模型服务组）：说明位「输入框下拉候选：<清单>」+「管理」操作钮。
// 未配置时清单为空（s18.5 起不再回退内置默认），说明位如实显示「本服务商暂无模型」。
// 管理弹 ModelManageDialog，仅维护自定义模型及每模型偏好。
// 生效语义：写 config.json 键 modelOptions；输入框模型下拉在每次弹层展开前重读该键并原地
// 刷新（ChatMsgEdit::reloadModelOptions），故改完配置无需重启即可选到新模型。
// 说明位展示的是**生效清单**（清洗去重后），非原始录入串；原始串经 ToolTip 兜底（两者可能不同）。
// Q_OBJECT 理由同其他设置卡：tr() 上下文须与 lupdate 提取的词法类名对齐。
class ModelListSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit ModelListSettingCard(ModelServiceTarget* target, QWidget* parent = nullptr);

    void retranslate();

private:
    void updateValue();
    // 「管理」开 ModelManageDialog：仅自定义模型，每模型偏好四控件 + 添加/拉取/移除/清空。
    void promptManage();

    ModelServiceTarget* m_target = nullptr;
    FluPushButton* m_manageButton = nullptr;
};

// 自更新设置卡（s17.12）：与 ContextLimitSettingCard 同款结构——标题「更新」+ 说明位承载
// 状态文本 + 右侧动作行两个按钮（「检查更新」常驻、「下载并更新」仅在确认有新版本时出现）。
// 刻意复用 InfoSlotSettingCard 而非裸 Label+按钮行：说明位的「前缀完整、值中间省略、
// ToolTip 全量」正是长状态串（错误码、进度）需要的形态，裸 FluLabel + wordWrap 会把
// 卡片行高撑开、与同页其余六卡视觉不齐（用户反馈的正是这一点）。
// 判据一律在 UpdateRules.h（可 ctest）、编排在 UpdateManager（链 Qt6::Network 不可测）；
// 本卡只做展示与按钮动作，不含任何版本比较/路径/删除决定。
// Q_OBJECT 理由同 WorkDirSettingCard：tr() 上下文必须与 lupdate 提取的词法类名对齐，
// 否则词条永不命中（且 lrelease 会静默丢弃 unfinished）。
class UpdateSettingCard : public InfoSlotSettingCard
{
    Q_OBJECT
public:
    explicit UpdateSettingCard(QWidget* parent = nullptr);

    // 自动校准入口：由 SettingsPage::showEvent 在首次可见时调用（本页面非懒建，
    // 构造期打网络等于每次启动都访问 GitHub）。同一卡实例只查一次，手动点按钮可重置。
    void checkNow();

    // LanguageChange 重译：标题/按钮文案重取 tr()，状态行按令牌重生成
    void retranslate();

private:
    void startUpdateCheck(bool forced);
    void handleApplyClicked();
    void handleRestartClicked();
    void onCheckFinished(bool hasUpdate, const QString& remoteTag, const QString& error);
    void onStageChanged(UpdateManager::Stage stage, const QString& detail);
    void onProgressChanged(qint64 received, qint64 total);
    void onStaged(bool ok, const QString& error);
    void applyStatus();      // 令牌 → 说明位（唯一出口，重译与槽都走它）
    // 令牌的另一个消费面：进度环。与说明位同由 applyStatus() 驱动，**不在各槽里单独调**
    // ——否则「改了令牌忘了收环」会和文案/环分叉，而这类分叉在冒烟里看不出来。
    void applyProgressRing();
    QString statusText() const;

    UpdateManager* m_updateManager = nullptr;
    FluPushButton* m_checkButton = nullptr;
    FluPushButton* m_applyButton = nullptr;
    FluPushButton* m_restartButton = nullptr;
    // 下载进度环：**只在 downloading 阶段可见**，其余阶段既隐藏也已停掉无限动画。
    // 环内不写字——百分比的唯一出口仍是说明位文案（`statusText()`），两处各写一遍
    // 就会在语言切换时有一处滞留，且 20px 的环里塞数字本来也读不清。
    FluProgressRing* m_progressRing = nullptr;
    // 分母与分子原值：环与文案都从 `UpdateRules::progressPercentOrUnknown` 读它们，
    // 判据单源在纯头，渲染层不再各自决定「这算不算知道分母」。
    qint64 m_progressReceived = 0;
    qint64 m_progressTotal = -1; // -1 = 未知（同 assetSizeOrUnknown 的口径），不是 0

    // 状态令牌：文案唯一来源。槽只改令牌、statusText() 只读令牌——两处各拼一套
    // 就会在语言切换时有一处滞留旧文案。
    QString m_stateToken;
    QString m_remoteTag;
    QString m_progressText; // 百分比数字豁免翻译，单独存不混进令牌
    bool m_checkStarted = false;
    bool m_swapApplied = false; // 换体已落盘旗标：置位后重启按钮再点只问重启，绝不重跑 applySwap；
                                // 进程存活期内绝不复位（复位会放行二次换体、毁掉旧体唯一备份）
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

    // s17.12 自更新：首次可见才打网络。本页面是主窗口构造期即时 new 的
    // （LiteHarness.cpp 的 `new SettingsPage` 在构造函数里，**不是懒建**），
    // 所以检查若放构造期就等于每次启动访问 GitHub——放这里才对得上
    // 「进设置页自动校准」这个需求，也才谈得上限流配额（未认证 60 次/小时/IP）。
    void showEvent(QShowEvent* event) override;

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
    // s17.2 权限审批总开关（config.json 键 permissionApproval）：下拉两项，与主题/语言盒同款
    FluLabel* m_permissionLabel = nullptr;
    FluSettingsSelectBox* m_permissionBox = nullptr;
    FluLabel* m_modelLabel = nullptr;
    // s18.3 多 Provider：模型服务组共享的编辑目标（本页持有，消费方只持裸指针）。
    // 生命周期依据：卡片都是本页的子控件、且都在构造期建好，故 target 必须是本页
    // 的子对象（parent=this）并**先于卡片构造**创建——否则卡片拿到空指针。
    ModelServiceTarget* m_modelTarget = nullptr;
    ModelListSettingCard* m_modelListCard = nullptr;
    FluLabel* m_aboutLabel = nullptr;
    FluSettingsVersionBox* m_versionBox = nullptr;
    FluLabel* m_infoLabel = nullptr;

    // ---- s17.12 自更新（判据单源 UpdateRules.h，编排单源 UpdateManager）----
    // 组标题 + 卡同款排布（对齐「上下文」+ 上下文上限卡）：标题在本页、卡内只放条目名与状态。
    // 检查时机 = 首次 showEvent（见上方注释：本页面并非懒建，构造期打网络即启动期打网络）。
    FluLabel *m_updateLabel = nullptr;
    UpdateSettingCard *m_updateCard = nullptr;
    // 检查时机与限流旗标都在卡内（UpdateSettingCard::checkNow / startUpdateCheck），
    // 本页只在 showEvent 转发一次、在 retranslateUi 转发一次重译。
};
