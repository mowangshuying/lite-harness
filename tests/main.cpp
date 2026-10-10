// 测试入口：逐个调用套件，汇总计数，失败以非零退出码交给 ctest 判定。
// 新增套件只需在此加一行调用（套件实现见 tests/tst_<模块>.cpp）。
//
// 假绿门禁（本轮新增）：套件的诚实跳过通道是 TestHarness::skip + return 0——它既不计
// 失败也不产生断言。于是「环境变量没配」的环境（曾经的 CI）里 4/5 套件整组 SKIP，
// 进程仍返回 0，ctest 判通过：门禁形同虚设。修法是把「实际执行了多少断言」变成硬条件，
// 由 CI 注入 LITE_TEST_MIN_PASS 承担；本机不设该变量时行为不变（无夹具照常绿行）。

#include "TestHarness.h"

#include <QtGlobal>
#include <cstdio>

int tst_lineending();
int tst_messagebus();
int tst_taskstore_lease();
int tst_worktree();
int tst_agentteams();

int main()
{
    int failed = 0;
    failed += tst_lineending();
    failed += tst_messagebus();
    failed += tst_taskstore_lease();
    failed += tst_worktree();
    failed += tst_agentteams();

    const int passed = TestHarness::passCount();
    const int failedTotal = TestHarness::failCount();
    const int skipped = TestHarness::skipCount();
    std::printf("pass=%d fail=%d skip=%d\n", passed, failedTotal, skipped);
    // failed 与 failCount() 应一致；不一致说明有套件漏返回失败数，一并视为不通过
    if (failed != 0 || failedTotal != 0)
        return 1;

    // 断言下界：低于该量级即说明有套件没跑到（夹具缺失/环境变量未注入），判不通过。
    // 阈值取全量套件实测量级的下浮值，留个别环境夹具（junction / git）偶发不可用的余量；
    // 新增套件时同步上调，CI 侧的实际数值见 .github/workflows/Windows-Qt6.9.0.yml。
    // 用 qEnvironmentVariableIntValue 而非 std::getenv：后者在 MSVC /W4 下报 C4996
    int minPass = 0;
    if (qEnvironmentVariableIsSet("LITE_TEST_MIN_PASS"))
        minPass = qEnvironmentVariableIntValue("LITE_TEST_MIN_PASS");
    if (minPass > 0 && passed < minPass)
    {
        std::printf("GATE FAIL: pass=%d < LITE_TEST_MIN_PASS=%d (suites skipped? "
                    "check LITE_TEST_TMPROOT)\n",
                    passed, minPass);
        return 2;
    }
    return 0;
}
