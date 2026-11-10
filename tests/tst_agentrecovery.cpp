// tst_agentrecovery.cpp —— s15 错误恢复纯头单测套件（对照 lcc s15 af26862 recovery.py 的契约串/判据）
// 被测：src/AgentRecovery.h（RecoveryRules：逐字契约串 + 鸭子式文本判据）
//       src/AgentConstants.h（s15 恢复常量门 :99-101）
//
// 本套件零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。
// 刻意跳过 fallbackModelIdValue() 的断言：它经 AppSettings::value() 读
// 存储根下的 config.json（`LitePaths::configFilePath()`），该文件是否存在属环境状态而非被测契约——断言之
// 会让结果随环境漂绿漂红，宁缺勿错（配置解析口径已由设置页冒烟覆盖）。
// 判据断言用 QStringLiteral 承载中文测试文本：/utf-8 下 src/ 已有 32 处同款先例。

#include "TestHarness.h"

#include "AgentRecovery.h"
#include "AgentConstants.h"

#include <QString>

namespace {

// 契约串逐字门：这两句原样发往模型 / 落进历史，改一个字即行为漂移（lcc s15 af26862）
void testContractStrings()
{
    // AgentRecovery.h :24 —— CONTINUATION_PROMPT 逐字（max_tokens 溢出两拍恢复的第二拍续写指令）
    TestHarness::check(
        RecoveryRules::continuationPrompt()
            == QStringLiteral("Continue from the previous response. Do not repeat completed work."),
        "continuationPrompt 与 lcc s15 CONTINUATION_PROMPT 逐字相等");

    // AgentRecovery.h :33 —— ASYNC_APPROVAL_DENIED 逐字（异步回合 fail-closed 固定拒绝文案）
    TestHarness::check(
        RecoveryRules::asyncApprovalDenied()
            == QStringLiteral(
                "Permission denied: interactive approval is unavailable during an asynchronous turn"),
        "asyncApprovalDenied 与 lcc s15 ASYNC_APPROVAL_DENIED 逐字相等");

    // 承重前缀门：宿主 AgentLoop::isToolFailure 与 TeammateRuntime 镜像均按
    // startsWith("Permission denied") 判失败（lcc s15 af26862 口径）——将来此串若被悄悄
    // 改名丢了前缀，异步拒批会被误判成功，本道门防的是这种静默失效
    TestHarness::check(
        RecoveryRules::asyncApprovalDenied().startsWith(QStringLiteral("Permission denied")),
        "asyncApprovalDenied 保留承重前缀 Permission denied（isToolFailure 前缀判定依赖）");
}

// 限流族判据（AgentRecovery.h :43-50：小写化后 contains ratelimit/429/"rate limit"/rate_limit 任一）
void testRateLimitClassifier()
{
    // 命中族：四个判据子串逐一 + 大小写混合 + 传输层真实中文错误体形态
    TestHarness::check(RecoveryRules::isRateLimitText(
                           QStringLiteral("RateLimitError: too many requests")),
                       "含 ratelimit 的类型名风格串命中（lcc 鸭子判据的类型名族折叠）");
    TestHarness::check(RecoveryRules::isRateLimitText(
                           QStringLiteral("HTTP 429 Too Many Requests")),
                       "429 子串命中");
    TestHarness::check(RecoveryRules::isRateLimitText(
                           QStringLiteral("rate limit exceeded")),
                       "空格词组 rate limit 命中");
    TestHarness::check(RecoveryRules::isRateLimitText(
                           QStringLiteral("upstream rate_limit reached")),
                       "下划线连写 rate_limit 命中");
    TestHarness::check(RecoveryRules::isRateLimitText(
                           QStringLiteral("RaTeLiMiT ErrOr")),
                       "判据统一小写化，大小写混合等价命中");
    TestHarness::check(RecoveryRules::isRateLimitText(QStringLiteral(
                           "HTTP 429 错误（重试 2 次后仍失败）: Too Many Requests")),
                       "传输层重试耗尽的中文错误文本按 429 命中（QOpenAi 实际文本形态）");

    // 不命中族：边界必须稳定——超时/过载各属别的判据族，误命中会扩大重试与换模面
    TestHarness::check(!RecoveryRules::isRateLimitText(QString()),
                       "空串不命中");
    TestHarness::check(!RecoveryRules::isRateLimitText(
                           QStringLiteral("请求过于频繁，请稍后再试")),
                       "无 ASCII 关键词的中文文本不命中（判据只认英文子串与状态码）");
    TestHarness::check(!RecoveryRules::isRateLimitText(
                           QStringLiteral("connection timeout after 30s")),
                       "超时文本不属限流族");
    TestHarness::check(!RecoveryRules::isRateLimitText(
                           QStringLiteral("HTTP 529 overloaded")),
                       "过载文本不属限流族（两判据互斥独立，防交叉误触发）");
}

// 过载族判据（AgentRecovery.h :55-60：小写化后 contains overloaded 或 529）
void testOverloadedClassifier()
{
    // 命中族：连续命中达 AgentConst::kMaxConsecutive529 即触发 fallback 换模（消费点 QOpenAi 传输层）
    TestHarness::check(RecoveryRules::isOverloadedText(
                           QStringLiteral("OverloadedError: server busy")),
                       "含 overloaded 的类型名风格串命中");
    TestHarness::check(RecoveryRules::isOverloadedText(
                           QStringLiteral("HTTP 529")),
                       "529 状态码子串命中");
    TestHarness::check(RecoveryRules::isOverloadedText(
                           QStringLiteral("model is overloaded, retry later")),
                       "消息内嵌 overloaded 命中");
    TestHarness::check(RecoveryRules::isOverloadedText(
                           QStringLiteral("OverLoAdEd on 529")),
                       "大小写混合经小写归一，双关键词等价命中");

    // 不命中族
    TestHarness::check(!RecoveryRules::isOverloadedText(QString()),
                       "空串不命中");
    TestHarness::check(!RecoveryRules::isOverloadedText(
                           QStringLiteral("服务器过载，稍后重试")),
                       "纯中文「过载」不命中（判据只认 ASCII 子串）");
    TestHarness::check(!RecoveryRules::isOverloadedText(
                           QStringLiteral("HTTP 429 Too Many Requests")),
                       "限流 429 不属过载族（换模只在真过载时发生，误判会无故切服务）");
    TestHarness::check(!RecoveryRules::isOverloadedText(
                           QStringLiteral("rate limit exceeded")),
                       "空格词组 rate limit 不属过载族");
}

// s15 恢复常量门（对照 lcc s15 af26862 recovery.py 常量块 :17-21；AgentConstants.h 单源 :99-101）
void testRecoveryConstants()
{
    TestHarness::check(AgentConst::kEscalatedMaxTokens == 16000,
                       "kEscalatedMaxTokens=16000（max_tokens 溢出第一拍升档重发）");
    TestHarness::check(AgentConst::kMaxConsecutive529 == 2,
                       "kMaxConsecutive529=2（连续过载计数达界即换 fallback 模型）");
    TestHarness::check(AgentConst::kMaxRecoveryRetries == 2,
                       "kMaxRecoveryRetries=2（恢复级重试预算，后续 lane 消费）");
}

// 工具成败判据（RecoveryRules::isToolFailureText）——全仓唯一真值源。
// 为什么必须钉死：这条判据决定一次工具调用是「成功回灌」还是「失败回灌」，历史上它在
// AgentLoop::isToolFailure 与队友侧 teamToolOutputLooksFailed 各有一份逐字副本、且**零直测**；
// 任一侧漏改即「主循环判失败、队友判成功」静默分叉，而队友侧 UI 不着色、漂移看不见。
// 现两侧都转发本口，这里把判据本身钉住。
void testToolFailureClassifier()
{
    // 四族前缀各自命中（约定：handler/钩子/BashRunner/TaskStore 的失败输出必以其一开头）
    TestHarness::check(RecoveryRules::isToolFailureText(
                           QStringLiteral("Error: something broke")),
                       "Error: 前缀判失败");
    TestHarness::check(RecoveryRules::isToolFailureText(
                           QStringLiteral("Permission denied")),
                       "Permission denied 裸相等判失败（短拒串形态）");
    TestHarness::check(RecoveryRules::isToolFailureText(
                           QStringLiteral("Blocked: rm -rf /")),
                       "Blocked: 前缀判失败");
    TestHarness::check(RecoveryRules::isToolFailureText(
                           QStringLiteral("Unknown tool: frobnicate")),
                       "Unknown tool: 前缀判失败");

    // 跨模块契约：异步回合的定值长拒串**必须**判失败。这条把 AGENTS.md 里「asyncApprovalDenied
    // 必须判失败」的注释约定升级成断言——它一旦退化成相等判定，异步拒绝就会被当成功回灌。
    TestHarness::check(RecoveryRules::isToolFailureText(RecoveryRules::asyncApprovalDenied()),
                       "asyncApprovalDenied() 定值串必须判失败（前缀覆盖而非相等）");

    // 反向：成功形态不得误判
    TestHarness::check(!RecoveryRules::isToolFailureText(QString()),
                       "空串不判失败");
    TestHarness::check(!RecoveryRules::isToolFailureText(
                           QStringLiteral("File written successfully.")),
                       "正常成功输出不判失败");

    // 大小写敏感是刻意的：正文内嵌小写 error: 不该把一次成功调用判成失败
    TestHarness::check(!RecoveryRules::isToolFailureText(QStringLiteral("error: lowercase")),
                       "小写 error: 不判失败（判据大小写敏感）");
    TestHarness::check(!RecoveryRules::isToolFailureText(
                           QStringLiteral("log line: error: boom")),
                       "内嵌 error: 不在行首，不判失败");

    // "Errors:" 与 "Error:" 只差一个字母，是最容易被写坏的边界
    TestHarness::check(!RecoveryRules::isToolFailureText(QStringLiteral("Errors were found")),
                       "\"Errors were…\" 不判失败（Error: 需带冒号）");
}

} // namespace

int tst_agentrecovery()
{
    const int before = TestHarness::failCount();
    testContractStrings();       // 3
    testRateLimitClassifier();   // 10
    testOverloadedClassifier();  // 8
    testRecoveryConstants();     // 3
    testToolFailureClassifier(); // 10
    return TestHarness::failCount() - before;
}
