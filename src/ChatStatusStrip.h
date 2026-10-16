#pragma once

#include <QWidget>
#include <QtGlobal> // qsizetype

#include "UsageTokens.h" // setTokenUsage 形参（纯头，QtCore-only；视图仍不自己算，数据由页面推入）
#include "StreamRate.h"  // setOutputRate 形参（同上：速度算术在纯头，本类只格式化文案）

class QVBoxLayout;
class StripRow;

/* ============================================================================
 * ChatStatusStrip —— 聊天输入框下方的状态条（承载上下文占用的**文字**读数）。
 *
 * 自上而下只有两层：分隔线 + **一行**（占用数字 + 消耗 + 速度）。
 * 占用**条**（#ctxTrack/#ctxFill）不在本组件——它归会话侧栏「上下文」节
 * （见 SessionSidebar.h），本条只承载文字；一行装不下时按各段自然宽比例压缩省略。
 *
 * 「运行状态」灯也不在本组件——三态灯整盏归会话侧栏「状态」节（见 SessionSidebar.h）。
 * s17 曾把灯随两节一起下移，随后按用户要求把灯搬回面板：灯是「现在什么状态」的面板
 * 身份信息，与数字/消耗/速度那类「侧栏收起也必须常驻」的计量数据不同类；两处同时亮灯
 * 会在同一时刻给出两个读数，故本条只留计量文字，不复制一份灯。
 *
 * 设计约束（与 SessionSidebar 同纪律）：
 *   ① 纯视图——不订阅 AgentLoop，所有数据由 ChatSessionPage 接线后调公开 setter 推入；
 *   ② 零线程；算术仍在 UsageTokens.h / StreamRate.h 纯头，本类只格式化文案；
 *   ③ 样式走本仓三主题 stylesheet/<theme>/ChatStatusStrip.qss，构造尾部 ThemeAware::bind 挂接；
 *   ④ 空账纪律不变：三段文字各自无数据即整段隐藏，不显示「0」骗人；**三段全空时连分隔线
 *      一起隐藏**——本组件只剩灯以外的东西，全空时留一条空线会被读成「状态条坏了」；
 *   ⑤ 同栏宽：作为 ChatSessionPage 底部输入组的末位成员，宽度由 applyColumnWidth 钳制，
 *      不另设 maximumWidth（否则与消息列/输入框脱栏）。
 * ========================================================================== */
class ChatStatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit ChatStatusStrip(QWidget* parent = nullptr);

    /// 上下文占用：≈token 估算 ÷ token 预算（与压缩触发口径同源；limit<=0 时按 0% 展示）
    void setContextUsage(qsizetype usedTokens, qsizetype limitTokens);
    /// 上下文**消耗**（区别于占用）：本会话已花掉的输入/输出/缓存命中，只增不减。
    /// 空累计（还没收到任何 usage 回读）隐藏整行——不留「0」骗人，也不留空行占位。
    /// 口径边界写进 tooltip：只含主循环 + 队友回合，不含压缩/记忆/workflow（AsyncRequest 无 usage 通道）
    void setTokenUsage(const UsageTokens::Totals &totals);
    /// 输出速度（token/s）：本轮 + 会话平均。空账（还没拿到任何有效生成窗口）隐藏整行——
    /// 与消耗行同纪律，不显示「0 tok/s」骗人。窗口口径写进 tooltip（生成窗口不含首字延迟）。
    void setOutputRate(const StreamRate::Meter &meter);

private:
    /// 三段全空时整条隐藏（含分隔线）；任一段有数据即整条回显——空账纪律的收尾判定
    void applyEmptiness();

    QVBoxLayout*    m_stripLayout = nullptr;
    QWidget*        m_separator = nullptr;
    StripRow*       m_stripRow = nullptr;
};
