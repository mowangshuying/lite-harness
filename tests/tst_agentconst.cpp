// tst_agentconst.cpp —— 智能体数值域与键名契约（src/AgentConstants.h，纯 QtCore）
// 被测：min/default/max 区间自洽、键名非空唯一、关键常量取值为正。
//
// 为什么必须测：数值域写反（min > max）时，取值函数会把**所有**合法配置判成非法、
// 全部静默回退默认 —— 表现是「设置页改了不生效」，没有任何报错。头文件里已有
// static_assert（P3-5）做编译期兜底，本套件是行为级复核：static_assert 只保证区间
// 有序，不保证键名唯一、不保证常量本身是正数。
//
// 注意：本套件只断言 constexpr 常量，不调用 *_value() 取值函数 —— 后者经
// AppSettings::value() 走 reader()，要求 QCoreApplication 实例且路径钉死在测试 exe
// 目录（会污染），属接线层，不在本套件职责内。
//
// 零夹具依赖，任何环境全量执行。

#include "TestHarness.h"

#include "AgentConstants.h"

#include <QString>
#include <QStringList>

namespace {

using TestHarness::check;

} // namespace

int tst_agentconst()
{
    const int before = TestHarness::failCount();

    // ---- ① 区间自洽（与头文件 static_assert 同判据，行为级复核）----
    check(AgentConst::kContextCharLimitMin < AgentConst::kContextCharLimitDefault
              && AgentConst::kContextCharLimitDefault < AgentConst::kContextCharLimitMax,
          "agentconst: contextCharLimit min < default < max");

    // ---- ①' 预算派生：声明值即窗口，**不得再有任何二次换算**（s17.9 丁-3 裁决）----
    // 为什么钉这条：旧实现把用户声明的上限 ÷4 当 token 预算，那个 4 假设「内容全是英文」，
    // 而分子 estimateTokens 对中文按 1:1 计（实测 qwen3.8-flash 中文 ≈1.00 字符/token）——
    // 于是声明 1,024,000 的窗口实际只生效 256,000，中文会话提前 4 倍被压缩。端点既无
    // /tokenize（实测 404）也不在 /models 报窗口元数据，窗口问不到、只能由用户声明，
    // 那声明值就该是权威值。这三条断言把「恒等」钉死：谁改回 ÷4（或任何系数）即当场转红。
    // 取几个跨量级的探针值逐一对账，顺带兜住「恒等函数被写成某个区间才成立」的写法漂移。
    check(AgentConst::contextTokenBudgetFrom(10000) == 10000,
          "budget identity: lower bound passes through unchanged (no /4)");
    check(AgentConst::contextTokenBudgetFrom(1024000) == 1024000,
          "budget identity: declared window is the budget verbatim (no /4)");
    check(AgentConst::contextTokenBudgetFrom(5000000) == 5000000,
          "budget identity: upper bound passes through unchanged (no /4)");
    // 与头文件 static_assert 同判据：编译期已拦一道，这里是行为级复核（防有人只删断言不改函数）
    check(AgentConst::contextTokenBudgetFrom(AgentConst::kContextCharLimitDefault)
              == AgentConst::kContextCharLimitDefault,
          "budget identity: default value passes through unchanged");
    check(AgentConst::kMaxToolIterationsMin < AgentConst::kMaxToolIterationsDefault
              && AgentConst::kMaxToolIterationsDefault < AgentConst::kMaxToolIterationsMax,
          "agentconst: maxToolIterations min < default < max");
    // maxRetries 下界是 0（= 关闭重试），闭区间
    check(AgentConst::kMaxRetriesMin <= AgentConst::kMaxRetriesDefault
              && AgentConst::kMaxRetriesDefault <= AgentConst::kMaxRetriesMax,
          "agentconst: maxRetries min <= default <= max");

    // ---- ② 关键常量为正（0 或负会让功能静默失效）----
    check(AgentConst::kMaxTokens > 0, "agentconst: max tokens positive");
    check(AgentConst::kMaxConsecutive529 > 0, "agentconst: consecutive 529 cap positive");
    check(AgentConst::kMaxRecoveryRetries > 0, "agentconst: recovery retries positive");
    check(AgentConst::kContextCharLimitMin > 0, "agentconst: context char limit min positive");

    // ---- ③ 键名非空且唯一（重复键 = 后写覆盖先写，两个功能互相踩）----
    const QStringList keys = {
        AgentConst::kContextCharLimitKey,
        AgentConst::kMaxToolIterationsKey,
        AgentConst::kMaxRetriesKey,
        AgentConst::kPermissionApprovalKey,
    };
    bool allNonEmpty = true;
    bool allUnique = true;
    bool noSpaces = true;
    for (int i = 0; i < keys.size(); ++i)
    {
        if (keys.at(i).isEmpty())
            allNonEmpty = false;
        if (keys.at(i).contains(QLatin1Char(' ')))
            noSpaces = false;
        for (int j = i + 1; j < keys.size(); ++j)
        {
            if (keys.at(i) == keys.at(j))
                allUnique = false;
        }
    }
    check(allNonEmpty, "agentconst: config keys non-empty");
    check(allUnique, "agentconst: config keys unique");
    check(noSpaces, "agentconst: config keys have no spaces");

    // ---- ④ 键名是裸名（组前缀 app/ 由 AppSettings::scopedKey 统一拼，此处不得自带）----
    bool noPrefix = true;
    for (const QString &k : keys)
    {
        if (k.contains(QLatin1Char('/')))
            noPrefix = false;
    }
    check(noPrefix, "agentconst: config keys are bare (no group prefix)");

    // ---- ⑤ 默认值语义：审批默认开启（解析不出来时宁可多问一次）----
    check(AgentConst::kPermissionApprovalDefault, "agentconst: permission approval defaults to true");

    return TestHarness::failCount() - before;
}