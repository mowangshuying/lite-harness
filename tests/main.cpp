// 测试入口：逐个调用套件，汇总计数，失败以非零退出码交给 ctest 判定。
// 新增套件只需在此加一行调用（套件实现见 tests/tst_<模块>.cpp）。

#include "TestHarness.h"

#include <cstdio>

int tst_lineending();

int main()
{
    int failed = 0;
    failed += tst_lineending();

    std::printf("pass=%d fail=%d\n", TestHarness::passCount(), TestHarness::failCount());
    // failed 与 failCount() 应一致；不一致说明有套件漏返回失败数，一并视为不通过
    return (failed == 0 && TestHarness::failCount() == 0) ? 0 : 1;
}
