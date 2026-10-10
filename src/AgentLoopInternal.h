#pragma once

// AgentLoop 拆分后的跨编译单元共享内部工具（声明单源）。
//
// 定位：不是对外 API。仅供 src/AgentLoop*.cpp 与友元所在 TU（SubAgent.cpp /
// ChatSessionPage.cpp）使用；其它模块请勿依赖本头的符号稳定性。
// 归属：AgentLoop.cpp 原匿名命名空间里被多个职责单元共用的那几个纯函数，
// 按「谁最贴近其语义谁定义」落到对应 .cpp，本头只负责声明。
//   - toolSummary / callToolName / callToolArgs  定义于 AgentLoopTools.cpp（工具分发侧）
//   - askPrefix / bashDenyList                   定义于 AgentLoopPermission.cpp（权限门侧）

#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QString>
#include <QStringList>

namespace AgentLoopDetail
{

// 工具调用的关键参数摘要（lcc s02 tool_use info：bash→command、glob→pattern、文件工具→path、
// todo_write/task/load_skill/任务图/cron 各取其一）。返回空串表示该工具无摘要可展示。
QString toolSummary(const QString &toolName, const QJsonObject &args);

// PreToolUse 钩子的「需询问」返回协议前缀（"ASK:"，C++ 移植约定，有意偏差）：
// 钩子返回 ASK:<reason> 时由 executeTool 识别并发 permissionRequired 异步挂起队列；
// 不带该前缀的非空返回值一律视为硬拦截文本（回填为 tool_result）。
const QString &askPrefix();

// bash 危险命令硬禁止列表（lcc fddb23e G4 单源）：权限门 checkDenyList、主循环
// executeBashAsync 与子代理（经 AgentLoopDetail::bashDenyList）三方共用。
const QStringList &bashDenyList();

// tool_call 的读取口径（arguments 为流式拼装出的 JSON 字符串）。主循环、钩子链、子代理与
// 历史回放共用本组函数，避免各处手写 function.name / function.arguments 取值与 JSON 解析。

// 工具名 / arguments 原文
QString callToolName(const QJsonObject &toolCall);
QString callToolArgsText(const QJsonObject &toolCall);

// 一次工具调用的解析结果：name 与 args 是权威读法；errorText 非空即 arguments JSON 非法
// （此时 args 为空对象），其内容就是应当回填给模型的错误文案——主循环与子代理同源。
struct ToolCallView
{
    QString name;
    QString argsText;
    QJsonObject args;
    QString errorText;
    bool ok() const { return errorText.isEmpty(); }
};

// 严格解析：非法 arguments 填 errorText（含原始参数前 100 字符，便于模型自纠）
ToolCallView parseToolCall(const QJsonObject &toolCall);

// 宽容解析：空串/非法 JSON 一律得空对象（钩子链、历史回放等只读预览路径用）
QJsonObject parseToolArgsText(const QString &argsText);

// 钩子链便捷读法：等价 parseToolCall(toolCall).args
QJsonObject callToolArgs(const QJsonObject &toolCall);

// 上下文超限错误判定单源（规格修3 §5.4）：小写包含式匹配主流 OpenAI 兼容端点的溢出
// 文案族（含 4xx 响应体透传后的 message 文本）；命中即触发反应式压缩（预算 1 次不变）。
// 定义于 AgentLoopRequest.cpp（主循环反应式压缩消费侧）；s13 队友流错误侧共享本判定
//（AgentLoopTeam.cpp 经 guard->tryReactiveCompact 走队友自身压缩，预算独立）。
bool isContextOverflowError(const QString &msg);

// tool_result 消息合成单源（OpenAI 协议硬要求：每个 tool_call 必须有对应 tool 消息，
// 断配对即上游 400）。此前 {role, tool_call_id, content} 三字段在六处手写装配——stop() 的
// "Permission denied" 与 "(stopped)"、cancelSubAgent 的两处 "(cancelled)"、onToolFinished
// 的正常回填、历史恢复占位——配对完整性靠人肉同步维持。收敛后协议键拼写与字段装配只有一个
// 事实源；content 一律由调用方给出（含 C 类禁翻区中文源，本函数不做 tr()）。
QJsonObject makeToolResult(const QString &callId, const QString &content);

// 会话历史 → wire messages 数组的快照单源（首轮请求、压缩后重发、续跑重发、子代理请求
// 共用）。此前同款 for-append 循环散落七处，且「排除 system」的 mid(1) 口径另散五处——
// 本函数只负责快照，口径（全量 / mid(1)）仍由调用方显式表达，勿在此隐式裁剪。
QJsonArray snapshotMessages(const QVector<QJsonObject> &messages);

} // namespace AgentLoopDetail

