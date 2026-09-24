#pragma once

// Agent 常量单源头：收敛散落的魔法数/字面量（模型清单、max_tokens、bash 超时、
// 输出截断上限）。纯 header-only，被 include 即可编译，无需加入 CMake 源列表。
// 使用方：AgentLoop.cpp / SubAgent.cpp / ChatMsgEdit.cpp。

#include <QString>
#include <QStringList>
#include <QtGlobal> // qsizetype

namespace AgentConst {

// 可选模型清单（原 ChatMsgEdit.cpp 静态字面量迁入）：不做注册表/配置等多余抽象；
// 首项为回落默认项（AgentLoop::model() 不在列表内时下拉显示并选中它）
inline const QStringList kModelOptions = {QStringLiteral("qwen3.8-flash"),
                                         QStringLiteral("qwen3.8-max")};

// 回落默认模型 = 清单首项（AgentLoop 构造期 MODEL_ID 环境变量为空时使用）
inline const QString kDefaultModel = kModelOptions.first();

// LLM 请求输出上限（lcc s06 create 调用显式 max_tokens=8000，主/子两条链一致）
constexpr int kMaxTokens = 8000;

// bash 工具执行超时（毫秒）：前台/后台/子代理三处共用
constexpr int kBashTimeoutMs = 120000;

// bash 超时回填给模型的错误文案：由 kBashTimeoutMs 派生，避免常量与文案漂移
// （逐字符等于原硬编码 "Error: Timeout (120s)"，前缀 "Error: " 为主循环错误族格式）
inline const QString kBashTimeoutError =
    QStringLiteral("Error: Timeout (%1s)").arg(kBashTimeoutMs / 1000);

// 工具输出字符截断上限（bash 前台/后台、read_file 共用；lcc [:50000] 语义）
constexpr qsizetype kOutputCharLimit = 50000;

} // namespace AgentConst
