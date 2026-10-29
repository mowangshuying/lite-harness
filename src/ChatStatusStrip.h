#pragma once

#include <QWidget>
#include <QtGlobal> // qsizetype

#include "UsageTokens.h" // setTokenUsage 形参（纯头，QtCore-only；视图仍不自己算，数据由页面推入）
#include "StreamRate.h"  // setOutputRate 形参（同上：速度算术在纯头，本类只格式化文案）

class QVBoxLayout;
class StripRow;

/* ============================================================================
 * ChatStatusStrip —— 聊天输入框下方的状态条（承载会话计量的**文字**读数）。
 *
 * 自上而下只有一层：**一行**（轮步 + 消耗 + 速度）。
 * 占用**条**（#ctxTrack/#ctxFill）与占用**数字**都不在本组件——同归会话侧栏「上下文」节
 * （见 SessionSidebar.h；数字曾住本行首段，按用户裁决移入侧栏与条同节，侧栏收起时随条
 * 一并不可见是明示代价）；本条只承载文字，一行装不下时按各段自然宽比例压缩省略。
 *
 * 「运行状态」灯也不在本组件——三态灯整盏归会话侧栏「状态」节（见 SessionSidebar.h）。
 * s16.1 曾把灯随两节一起下移，随后按用户要求把灯搬回面板：灯是「现在什么状态」的面板
 * 身份信息，与消耗/速度那类「侧栏收起也必须常驻」的计量数据不同类；两处同时亮灯
 * 会在同一时刻给出两个读数，故本条只留计量文字，不复制一份灯。
 *
 * 设计约束（与 SessionSidebar 同纪律）：
 *   ① 纯视图——不订阅 AgentLoop，所有数据由 ChatSessionPage 接线后调公开 setter 推入；
 *   ② 零线程；算术仍在 UsageTokens.h / StreamRate.h 纯头，本类只格式化文案；
 *   ③ 样式走本仓三主题 stylesheet/<theme>/ChatStatusStrip.qss，构造尾部 ThemeAware::bind 挂接；
 *   ④ 空账纪律不变：三段文字各自无数据即整段隐藏，不显示「0」骗人；**三段全空时整条
 *      隐藏**——全空时留一行空白会被读成「状态条坏了」；轮步段的空账判据与另两段不同
 *      （看「开过回合没有」而非「有没有回读数据」，`steps==0` 是合法读数照实显示）；
 *   ⑤ 同栏宽：作为 ChatSessionPage 底部输入组的末位成员，宽度由 applyColumnWidth 钳制，
 *      不另设 maximumWidth（否则与消息列/输入框脱栏）。
 * ========================================================================== */
class ChatStatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit ChatStatusStrip(QWidget* parent = nullptr);

    /// 上下文**消耗**（区别于占用）：本会话已花掉的输入/输出/缓存命中，只增不减。
    /// 空累计（还没收到任何 usage 回读）隐藏整行——不留「0」骗人，也不留空行占位。
    /// 口径边界写进 tooltip：只含主循环 + 队友回合，不含压缩/记忆/workflow（AsyncRequest 无 usage 通道）
    void setTokenUsage(const UsageTokens::Totals &totals);
    /// 输出速度（token/s）：本轮 + 会话平均。空账（还没拿到任何有效生成窗口）隐藏整行——
    /// 与消耗行同纪律，不显示「0 tok/s」骗人。窗口口径写进 tooltip（生成窗口不含首字延迟）。
    void setOutputRate(const StreamRate::Meter &meter);
    /// 回合/步数读数：本会话开过多少个回合、当前回合内已发起第几轮工具调用。
    /// 空账 = `turns <= 0`（会话从没跑过任何回合）整段隐藏——「第 0 轮 · 第 0 步」不是
    /// 读数而是没账，与消耗/速度段同纪律。`steps` 为 0 是**合法值**（本回合还没调用工具
    /// 就直接答完），照实显示「第 0 步」，不隐藏：隐藏会让「第 3 轮」单独出现并读成
    /// 「第 3 轮什么都没干」。
    /// `stepLimit`=步数的上限（tooltip 里要写「撞上限即终止」这个数），**由页面传入**：
    /// 本类是纯视图，不读 settings.ini（同「不订阅 AgentLoop」那条纪律的另一半——视图也
    /// 不自己取设置），页面 `refreshContextUsage` 已在读 `contextTokenBudget()`，同批读上限
    /// 顺手带进来，口径与压缩/轮次上限的入口单取一致。
    void setTurnStep(int turns, int steps, int stepLimit);

private:
    /// 三段全空时整条隐藏；任一段有数据即整条回显——空账纪律的收尾判定
    void applyEmptiness();

    QVBoxLayout*    m_stripLayout = nullptr;
    StripRow*       m_stripRow = nullptr;
};
