#pragma once

// 日志单源中转（口径详见 AGENTS.md「关键约定」末条）。
//
// 为什么要有这个头：FluentUI 的 FluLogUtils 落盘只写 msg 原文（myMessageOutput 里
// tWrite << msg << "\n"）、不加任何前缀，所以时间/级别/文件/函数/行号只能由 LOG_* 宏提供；
// 本仓调用点因此统一走 LOG_DEBUG/LOG_INFO/LOG_WARN。但 lite-harness-tests 只链 Qt6::Core、
// 不引 FluentUI::Utils（见根 CMakeLists 测试节），而 TaskStore.cpp / AgentTeamsManager.cpp
// 同进两个目标——故经本头切换：主程序落到 FluLogUtils.h 原生宏，测试侧定义 LITE_NO_FLU_LOG
// 后退化成同流修饰符的裸 qDebug 族（ctest 继续读 stdout，测试 exe 不被拖进 Qt6::Gui 的 DLL 依赖）。
//
// 调用点写法：LOG_XXX << …，不再需要括号或 .noquote()。
// noquote 已收进 FluentUI 侧的宏定义内部（qDebug().nospace().noquote() << 前缀），
// 本仓 fork 的 log-noquote 分支已提交并推送。历史上曾要求 (LOG_XXX).noquote()，其两个成因
// 记录在此以免有人再踩：宏尾是 << "] "，裸写 LOG_DEBUG.noquote() 的「.」会绑到字符串字面量
// （C2228）；Qt 6.9 的 qdebug.h 只有成员函数 noquote()，无 Qt::noquote manipulator（C2039）。
//
// 仍保留 nospace 语义 ⇒ 多参数调用点的项间空格要靠字面量显式携带（已全仓补齐，
// 如 "QOpenAi [retry] " << n << " / " << max << " after " << ms << " ms"）。
//
// 不提供 LOG_ERR：上游 LOG_ERR 宏底层是 qDebug() 而非 qCritical()，用它替换 qCritical
// 会让 Qt 日志级别降级、QT_LOGGING_RULES 按级别过滤失效，故全仓 2 处 qCritical 维持原样、
// 不经本头。

#include <QDebug>

#ifdef LITE_NO_FLU_LOG
#define LITE_LOG_DEBUG_FALLBACK qDebug().nospace().noquote()
#define LITE_LOG_INFO_FALLBACK qInfo().nospace().noquote()
#define LITE_LOG_WARN_FALLBACK qWarning().nospace().noquote()
#define LOG_DEBUG LITE_LOG_DEBUG_FALLBACK
#define LOG_INFO LITE_LOG_INFO_FALLBACK
#define LOG_WARN LITE_LOG_WARN_FALLBACK
#else
#include <FluLogUtils.h>
#endif
