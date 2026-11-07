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
int tst_mcpmanager();
int tst_agentrecovery();      // s15：零夹具，任何环境全跑
int tst_backgroundtasks();    // s15：零夹具，任何环境全跑
int tst_workflowmanager();    // s16：纯函数组恒跑 + 文件产物组夹具门控
int tst_workflowruntime();    // s16：limits/脚本组恒跑 + 端到端组夹具门控
int tst_compacttokens();      // 计量单源纯头（CompactTokens.h），零夹具，任何环境全跑
int tst_settingscache();      // 设置读缓存纯头（SettingsCache.h），判据组恒跑 + 文件组夹具门控
int tst_usagetokens();        // 用量计量纯头（UsageTokens.h），零夹具，任何环境全跑
int tst_streamrate();         // 输出速度纯头（StreamRate.h），零夹具，任何环境全跑
int tst_goalmanager();        // s17：goal 引擎六门/七决策/转录预算，零夹具，任何环境全跑
int tst_cronmanager();        // s17 P1：cron 判据 + 台账快照，零夹具（durable=false 不触盘），任何环境全跑
int tst_uiscale();            // UI 优化 P1-11：字号/DPI 策略名/几何编解码判据，零夹具，任何环境全跑
int tst_fluconfigstore();     // FluentUI 配置定位层（FluConfigStore.h）：三段定位/格式折叠/JSON 后端，判据组恒跑 + 文件组夹具门控
int tst_fluconfigutils();     // FluentUI 配置门面（FluConfigUtils）：configure 注入→后端重建→读写往返，缺省组恒跑 + 落盘组夹具门控
int tst_litepaths();          // 运行时路径锚点单源（LitePaths.h）：存储根反推/中间层只一次/空 ID 回退，零夹具恒跑
int tst_pathguard();          // 沙箱围栏多根判定（AgentPathGuard.h）：边界/大小写/fail-closed，零夹具恒跑
int tst_configmerge();        // 配置合流不变量（P2）：两侧同路径同格式、合写不互覆、json 大小写口径，判据组恒跑 + 落盘组夹具门控
int tst_sessionstore();       // 会话索引并入 config.json（app/sessions）：读写换轨、不抹别人键、坏形态降级、旧 index.json 迁移四分支
int tst_uiux();               // P1 UI 优化：命中区下限/组前缀契约/写失败可观测/数值域自洽，判据组恒跑 + 写失败组夹具门控
int tst_tooltagkind();        // P2：工具名→标签类别映射（ToolTagKindNames.h），零夹具恒跑
int tst_toolnames();          // P2：工具名常量表契约（ToolNames.h 交叉映射），零夹具恒跑
int tst_navkeys();            // P2：导航键前缀一致性（NavKeys.h），零夹具恒跑
int tst_layoutconst();        // P2：布局常量自洽（LayoutConstants.h），零夹具恒跑
int tst_agentconst();         // P2：智能体数值域与键名契约（AgentConstants.h），零夹具恒跑
int tst_compacttranscript();  // 压缩转写回溯（CompactTranscript.h），零夹具恒跑
int tst_activityledger();     // 会话每日 token 消耗账本（ActivityLedger.h）：阈值/日期键/累加/求和/截断恒跑 + 读写往返/脏数据降级夹具门控

static int runSuite(const char *name, int (*fn)())
{
    const int before = TestHarness::passCount();
    const int f = fn();
    std::printf("SUITE %s pass=%d fail=%d\n", name, TestHarness::passCount() - before, f);
    return f;
}

int main()
{
    int failed = 0;
    failed += runSuite("lineending", tst_lineending);
    failed += runSuite("messagebus", tst_messagebus);
    failed += runSuite("taskstore_lease", tst_taskstore_lease);
    failed += runSuite("worktree", tst_worktree);
    failed += runSuite("agentteams", tst_agentteams);
    failed += runSuite("mcpmanager", tst_mcpmanager);
    failed += runSuite("agentrecovery", tst_agentrecovery);
    failed += runSuite("backgroundtasks", tst_backgroundtasks);
    failed += runSuite("workflowmanager", tst_workflowmanager);
    failed += runSuite("workflowruntime", tst_workflowruntime);
    failed += runSuite("compacttokens", tst_compacttokens);
    failed += runSuite("settingscache", tst_settingscache);
    failed += runSuite("usagetokens", tst_usagetokens);
    failed += runSuite("streamrate", tst_streamrate);
    failed += runSuite("goalmanager", tst_goalmanager);
    failed += runSuite("cronmanager", tst_cronmanager);
    failed += runSuite("uiscale", tst_uiscale);
    failed += runSuite("fluconfigstore", tst_fluconfigstore);
    failed += runSuite("fluconfigutils", tst_fluconfigutils);
    failed += runSuite("litepaths", tst_litepaths);
    failed += runSuite("pathguard", tst_pathguard);
    failed += runSuite("configmerge", tst_configmerge);
    failed += runSuite("sessionstore", tst_sessionstore);
    failed += runSuite("uiux", tst_uiux);
    failed += runSuite("tooltagkind", tst_tooltagkind);
    failed += runSuite("toolnames", tst_toolnames);
    failed += runSuite("navkeys", tst_navkeys);
    failed += runSuite("layoutconst", tst_layoutconst);
    failed += runSuite("agentconst", tst_agentconst);
    failed += runSuite("compacttranscript", tst_compacttranscript);
    failed += runSuite("activityledger", tst_activityledger);

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
