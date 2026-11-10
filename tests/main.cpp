// 测试入口：逐个调用套件，汇总计数，失败以非零退出码交给 ctest 判定。
// 新增套件只需在此加一行调用（套件实现见 tests/tst_<模块>.cpp）。
//
// 假绿门禁（s17.6 引入全局下界，s17.9 补逐套件下界）：套件的诚实跳过通道是
// TestHarness::skip + return 0——它既不计失败也不产生断言。于是「环境变量没配」的环境
// （曾经的 CI）里 4/5 套件整组 SKIP，进程仍返回 0，ctest 判通过：门禁形同虚设。
//
// 两道门禁各挡一类失效：
//   ① 全局 LITE_TEST_MIN_PASS（CI 注入）——挡「夹具变量整体缺失」这种一次性掉几百条的
//      塌方（本机实测：带夹具 2075 / 不带 1099，差 976，远大于余量）。
//   ② 下面的 kSuiteFloors 逐套件下界——挡「某个套件静默消失」。全局下界挡不住这条：
//      31 个套件里 30 个的实跑条数都小于全局余量（204），删掉 main 里任意一行 runSuite
//      调用（除 agentteams 263 条）CI 仍然全绿。逐套件表把「每个套件至少跑到多少条」
//      变成硬条件，并且**表里有的名字必须被跑到**——套件被取消注册或改名同样判失败。
//
// 下界取值口径（重要，别照抄带夹具的数）：一律取该套件**不设 LITE_TEST_TMPROOT** 时的
// 实跑条数，即「最坏情况也成立」的下限，故本地无夹具跑与 CI 带夹具跑都恒满足；
// 后续给套件**加**断言无需改表（下界只朝下约束），只有「套件消失」或「断言被删到低于
// 下限」才触发。四个纯夹具套件（messagebus / taskstore_lease / worktree / agentteams）
// 无夹具时本就 0 条，下界记 0——它们靠「必须被跑到」那条兜住，不靠条数。
// 本机不设 LITE_TEST_MIN_PASS 时行为不变（②始终生效，与夹具无关）。

#include "TestHarness.h"

#include <QtGlobal>
#include <cstdio>
#include <cstring>

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
int tst_heatmapgrid();        // 热力图网格算术（HeatmapGrid.h）：371 满矩形 / 末格恒为 today / cellOf↔dateOfCell 互逆 / 窗口外，零夹具恒跑

// 逐套件下界表（口径见文件头②）：floor = 该套件不设 LITE_TEST_TMPROOT 时的实跑条数。
// 全表求和 = 1099，与本机无夹具全量实跑 pass=1099 逐条对上（带夹具为 2075）。
struct SuiteFloor {
    const char *name;
    int floor;
};
static const SuiteFloor kSuiteFloors[] = {
    {"lineending", 36},         {"messagebus", 0},          {"taskstore_lease", 0},
    {"worktree", 0},            {"agentteams", 0},          {"mcpmanager", 45},
    {"agentrecovery", 34},      {"backgroundtasks", 43},    {"workflowmanager", 118},
    {"workflowruntime", 3},     {"compacttokens", 48},      {"settingscache", 35},
    {"usagetokens", 82},        {"streamrate", 30},         {"goalmanager", 209},
    {"cronmanager", 26},        {"uiscale", 68},            {"fluconfigstore", 49},
    {"fluconfigutils", 4},      {"litepaths", 29},          {"pathguard", 21},
    {"configmerge", 14},        {"sessionstore", 4},        {"uiux", 11},
    {"tooltagkind", 26},        {"toolnames", 7},           {"navkeys", 14},
    {"layoutconst", 23},        {"agentconst", 16},         {"compacttranscript", 40},
    {"activityledger", 37},        {"heatmapgrid", 27},
};
static constexpr int kSuiteFloorCount =
    static_cast<int>(sizeof(kSuiteFloors) / sizeof(kSuiteFloors[0]));

// 每个表项的实跑 pass 数；-1 = 本轮没跑到（套件被取消注册/改名 → 下面判硬失败）。
static int g_suitePass[kSuiteFloorCount];

static int suiteFloorIndexOf(const char *name)
{
    for (int i = 0; i < kSuiteFloorCount; ++i) {
        if (std::strcmp(kSuiteFloors[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int runSuite(const char *name, int (*fn)())
{
    const int before = TestHarness::passCount();
    const int f = fn();
    const int passed = TestHarness::passCount() - before;
    std::printf("SUITE %s pass=%d fail=%d\n", name, passed, f);

    const int idx = suiteFloorIndexOf(name);
    if (idx < 0) {
        // 表里没有这个套件：新套件忘了登记，或表项被改名——两者都会让下界形同虚设，硬失败。
        std::printf("GATE FAIL: suite '%s' 不在 kSuiteFloors 表内（新增套件须登记下界）\n", name);
        return f + 1;
    }
    g_suitePass[idx] = passed;
    return f;
}

int main()
{
    // -1 = 本轮未跑到。必须显式初始化：静态数组零初始化会让「套件被删/改名」与
    // 「跑到了一次、产出 0 条」不可区分，而前者正是本门禁要抓的失效。
    for (int i = 0; i < kSuiteFloorCount; ++i)
        g_suitePass[i] = -1;

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
    failed += runSuite("heatmapgrid", tst_heatmapgrid);

    const int passed = TestHarness::passCount();
    const int failedTotal = TestHarness::failCount();
    const int skipped = TestHarness::skipCount();
    std::printf("pass=%d fail=%d skip=%d\n", passed, failedTotal, skipped);
    // failed 与 failCount() 应一致；不一致说明有套件漏返回失败数，一并视为不通过
    if (failed != 0 || failedTotal != 0)
        return 1;

    // 门禁②：逐套件下界（无条件生效，与 LITE_TEST_MIN_PASS 无关）。
    // 表里每个名字都必须被跑到、且实跑条数不低于下界——这两条一起把「套件静默消失」
    // 从「靠全局余量兜」变成硬失败。下界取无夹具实跑值，故本机不带 LITE_TEST_TMPROOT
    // 跑也恒满足，不会假红。
    for (int i = 0; i < kSuiteFloorCount; ++i) {
        if (g_suitePass[i] < 0) {
            std::printf("GATE FAIL: suite '%s' 未被执行（注册行被删或改名？）\n",
                        kSuiteFloors[i].name);
            return 2;
        }
        if (g_suitePass[i] < kSuiteFloors[i].floor) {
            std::printf("GATE FAIL: suite '%s' pass=%d < 下界 %d（断言组被跳过/注释掉？）\n",
                        kSuiteFloors[i].name, g_suitePass[i], kSuiteFloors[i].floor);
            return 2;
        }
    }

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

    // 门禁③：skip 上界（CI 注入 LITE_TEST_MAX_SKIP；本机不设则不判）。
    // 为什么需要：TestHarness::skip 既不计失败也不产生断言，全测试目标有 42 处 skip 点，
    // 「夹具形态被改坏」会让整组静默降级而进程仍返回 0——下界①②只能兜住「掉了很多断言」，
    // 兜不住「掉的那部分恰好都以 skip 形式消失」。CI 现值 1（唯一诚实 skip 是 junction 夹具，
    // 见 tst_messagebus），超出即说明有第二处开始跳过，必须人看。
    if (qEnvironmentVariableIsSet("LITE_TEST_MAX_SKIP")) {
        const int maxSkip = qEnvironmentVariableIntValue("LITE_TEST_MAX_SKIP");
        if (maxSkip >= 0 && skipped > maxSkip) {
            std::printf("GATE FAIL: skip=%d > LITE_TEST_MAX_SKIP=%d（有套件静默降级？）\n",
                        skipped, maxSkip);
            return 3;
        }
    }
    return 0;
}
