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