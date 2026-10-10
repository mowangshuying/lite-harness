#pragma once

// 测试断言极简骨架（header-only，无状态）：只做「计数 + 打印失败项」，由 main 汇总后
// 以非零退出码交给 ctest 判定。
//
// 为何不引 Qt6::Test：本仓可单测的对象全是纯函数（LineEnding 行尾口径、cron 表达式匹配、
// token 估算、bash 危险词判定、frontmatter 解析、TaskStore 内核…），不需要信号槽、
// 数据驱动表或 QTest 的事件循环；少一个组件依赖与 moc 环节，本地 cl 与 CI 都能直接起来，
// 也避免测试目标把 Qt6::Test 的 DLL 拖进运行环境。
//
// 新增套件：写 tests/tst_<模块>.cpp，暴露 `int tst_<模块>()`（内部用 TestHarness::check，
// 返回本套件失败数），再到 tests/main.cpp 里调用一行。

#include <cstdarg>
#include <cstdio>

namespace TestHarness {

// 计数器用函数内 static + inline 取引用：C++17 下跨 TU 唯一实例，无需额外定义文件
inline int &passCount()
{
    static int n = 0;
    return n;
}

inline int &failCount()
{
    static int n = 0;
    return n;
}

// SKIP 计数（s13 后新增）：套件对「夹具不可用」的诚实通道是 printf + return 0，
// 而 main 只累加失败数——于是未注入 LITE_TEST_TMPROOT 的 CI 里 4/5 套件整组跳过仍判
// 通过（假绿）。计数本身不判失败（本机无夹具时照常绿行），门禁由 LITE_TEST_MIN_PASS
// 断言下界承担，见 tests/main.cpp。
inline int &skipCount()
{
    static int n = 0;
    return n;
}

// 断言：失败即打印并计数，不中断后续用例（一次跑完看全貌，而非逐个修逐个跑）
inline void check(bool ok, const char *what)
{
    if (ok)
    {
        ++passCount();
        return;
    }
    ++failCount();
    std::printf("FAIL: %s\n", what);
}

// SKIP 通知：打印 + 计数。格式串沿用各处原样（自带结尾 \n），故本函数不再补换行
inline void skip(const char *fmt, ...)
{
    ++skipCount();
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

} // namespace TestHarness
