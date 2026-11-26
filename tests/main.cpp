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
int tst_heatmapgrid();        // 热力图网格算术（HeatmapGrid.h）：371 满矩形 / 末格恒为 today / cellOf↔dateOfCell 互逆 / 窗口外 / 月份标签按月号去重、首列补标让位给同号 1 号锚点，零夹具恒跑
int tst_memoryrules();      // 记忆纯判据（MemoryRules.h）：frontmatter 回落/slug 撞名/括号配对状态机/去重门槛/写读往返，零夹具恒跑
int tst_toolschema();       // 工具 schema 单源（ToolSchema.h）：信封骨架/required⊆properties/类型专属关键字配对/todo enum+maxItems 同源/无 default/additionalProperties 四把锁/workflow 委托同源，零夹具恒跑
int tst_sessionkeys();      // 会话键算术纯头（SessionKeys.h）：两族键形态/归属反解/生成 dataId 形状/不相交的真实理由(长度非字母表)/撞车谓词与旧注释证伪，零夹具恒跑
int tst_projectinstructions(); // s17.11 项目指令文件纯头（ProjectInstructions.h）：两级候选顺序/命中即止的探测次数/空段省略/截断如实标注/'%'不展开/签名三字段任一变即换，零夹具恒跑
int tst_updaterules();         // s17.12 自更新判据纯头（UpdateRules.h）：版本数值比较(补丁位陷阱/三段形态 s17.13.1 起/一段形态 s18 起)/资产择优/digest fail-closed/zip 条目映射挡 zip-slip/包根推导/换体计划确定性/marker 一条不可删即整体作废，零夹具恒跑
int tst_providerspec();      // s18.3 多 Provider 数据层纯头（ProviderSpec.h）：基址归一与合法性、id 确定性防撞、解析容错与上限、旧三键回落、按模型名路由、/models 三形态解析、拉取合并撞名拦截，零夹具恒跑
int tst_contexttrim();         // 上下文成组裁剪内核纯头（ContextTrim.h）：配对不变量/keepHead 头部保护/oldest-first/孤立 tool 兜底/目标比 4/5 与 1/2/幂等，零夹具恒跑
int tst_scrollsettle();        // s18.5 贴底算术与跟随态判据纯头（ScrollSettle.h）：range 算术/新件 hidden 对布局 min 贡献 0/两阶段落点/缺口/近底阈值 80 边界，零夹具恒跑
int tst_modelprefs();          // s18.5 模型管理弹窗每模型偏好纯头（ModelPrefs.h）：effort 四档归一/window 三档互映/严格布尔/显示过滤保序/窗口回落/thinkingParams 关思考不带 effort/自定义清单两形态，零夹具恒跑
int tst_toolcallacc();         // SSE tool_calls 增量累加器纯头（ToolCallAccum.h）：增量缓冲终局组装/占位过滤/index 对齐/覆写口径/首帧双写修复/旧接线参照等价，零夹具恒跑
int tst_pastetext();           // 粘贴文本归一纯头（PasteText.h）：行尾归一/C0 与隐形字符剥离/空白族折叠/三类保留项/幂等/代理对不拆，零夹具恒跑
int tst_messagecontent();      // s18.6 图片输入批消息 content 形态纯头（MessageContent.h）：纯串/parts 两形态组装/摊平带标记/拼接不丢图/张数去重/发送末段展开与三降级/历史往返，零夹具恒跑
int tst_imageattach();         // s18.6 图片附件载荷纯头（ImageAttach.h）：魔数嗅探含五类正例与六类负例/MIME↔扩展名两向往返/内容哈希命名/附件名形态守卫（越界·分隔符·盘符全拒）/引用两向/体量与张数闸门边界/data URL 拼装，零夹具恒跑

// 逐套件下界表（口径见文件头②）：floor = 该套件不设 LITE_TEST_TMPROOT 时的实跑条数。
// 全表求和 = 1585（s17.12 自更新判据批 +137、进度环分母判据 +18、s17.13.1 三段版本判据 +19、
// s18 一段版本判据 +24、s18 热力图月份标签去重 +12、s18 子代理上下文压缩成组裁剪内核 +48、workflow A+B+C 修复批 +3 后），
// 与本机无夹具全量实跑 pass=1585 fail=0 skip=35 逐条对上；带夹具实跑 pass=2596 fail=0 skip=1
// （s18 子代理压缩批两形态复测；旧注「夹具通道跑不起来」系 s17.13.1 批本机现象，夹具根须落在仓库构建树内）。
// s18.5 贴底算术守卫批 +32 后全表求和 = 1716，与本机无夹具全量实跑 pass=1716 fail=0 skip=35 逐条对上；
// 带夹具实跑 pass=2727 fail=0 skip=1（套件 40 个，三口径 40/40/40）。上面 1585/2596 那组是 s18 子代理
// 压缩批的现值，按「历史登记值原样保留」不动，本块不复用其数字。
// s18.5 模型管理弹窗批 +73（tst_modelprefs，零夹具恒跑）后全表求和 = 1789，与本机无夹具全量实跑
// pass=1789 fail=0 skip=35 逐条对上；带夹具实跑 pass=2805 fail=0 skip=1（套件 41 个，三口径 41/41/41）。
// 串钥与 tool_call 参数修复批 +33（新增 tst_toolcallacc 25 条、providerspec ⑳ 凭据装配 +8，
// 均零夹具恒跑）后全表求和 = 1822，与本机无夹具全量实跑 pass=1822 fail=0 skip=35 逐条对上；
// 带夹具实跑 pass=2838 fail=0 skip=1（套件 42 个，三口径 42/42/42）。
// 输入框粘贴纯净化批 +51（新增整套 tst_pastetext，守 PasteText.h 的归一判据，零夹具恒跑）后全表
// 求和 = 1873，与本机无夹具全量实跑 pass=1873 fail=0 skip=35 逐条对上；带夹具实跑 pass=2889
// fail=0 skip=1（套件 43 个，三口径 43/43/43；上面 1822/2838 那组是本批前一登记的现值，按
// 「历史登记值原样保留」不动，本块不复用其数字）。
// s18.6 图片输入批 +162（新增整套 tst_messagecontent 60 条守 MessageContent.h 的形态判据、
// 整套 tst_imageattach 93 条守 ImageAttach.h 的嗅探/命名/形态守卫/闸门/data URL；另给
// tst_compacttokens 补 ⑦ 组图片定值成本 9 条，48→57）后全表求和 = 2035，与本机无夹具全量
// 实跑 pass=2035 fail=0 skip=35 逐条对上；带夹具实跑 pass=3051 fail=0 skip=1（套件 45 个，
// 三口径 45/45/45）。三组全为零夹具恒跑——图片这件事的判据没有一处需要盘上夹具，读盘在
// 宿主侧、本批判据全是纯函数（附件字节经 std::function 注入桩，同 ProjectInstructions 那套）。
// 上面 1873/2889 那组是本批前一登记的现值，按「历史登记值原样保留」不动。
// 服务商模型列表串家修复轮 +12（给 tst_providerspec 补 ㉑ 组 ownerIdOfModel 判据：归属命中/
// 别家不误命中/查无与空白名不认领/空表/与 route 同 trim 口径/逐条对表 3 条循环展开/
// 手改重名取首个 + allModels 同向，零夹具恒跑）后全表求和 = 2047，与本机无夹具全量实跑
// pass=2047 fail=0 skip=35 逐条对上；带夹具实跑 pass=3063 fail=0 skip=1（套件仍 45 个，
// 三口径 45/45/45，本轮不新增套件）。上面 2035/3051 那组是图片输入批的现值，原样保留。
// cron 7 段式批 +11（tst_cronmanager 26→37：validateCron 7 字段报错族、秒/年 AND 匹配、
// pollDueJobs 逐秒补匹配，零夹具恒跑）后全表求和 = 2058，与本机无夹具全量实跑
// pass=2058 fail=0 skip=35 逐条对上；带夹具实跑 pass=3074 fail=0 skip=1（套件仍 45 个，
// 三口径 45/45/45，本轮不新增套件）。上面 2047/3063 那组是服务商模型列表串家修复轮的现值，原样保留。
// cron 漏秒修复批 +3（tst_cronmanager 37→40：新增 testPollSubSecondDrift 钉住 pollDueJobs 补匹配
// 基准带亚秒余数时「整轮零检查、被跨过的整秒永不回补」——QTimer 1s 节拍相邻两拍的墙钟差可略
// 小于 1s（上拍被事件循环延迟、本拍追平），带 .500 余数的 QDateTime 基准会让 from > moment，
// 秒位限定表达式整周期漏触发（实测 999ms 稳态节拍跑 20000 拍仅覆盖 1 枚整秒）；基准改 epoch
// 整秒轴 m_lastPollSec 后每枚整秒至多查一次且不漏，零夹具恒跑）后全表求和 = 2061，与本机无
// 夹具全量实跑 pass=2061 fail=0 skip=35 逐条对上；带夹具实跑 pass=3077 fail=0 skip=1（套件仍
// 45 个，三口径 45/45/45，本轮不新增套件）。上面 2058/3074 那组是 cron 7 段式批的现值，原样保留。
// cron 锚定与 Quartz 写法批 +12（tst_cronmanager 40→52：testStepAnchorAndQuartzAliases 钉 `*/N`
// 以各字段**自己的域下界**为锚——日位 */2 得 1,3,5… 而非旧 `value%N` 的 2,4,6…（「每 N 天」整体
// 偏一天、1 号永不触发），月位 */3 得 1,4,7,10，年位 */10 以 1970 为锚；逗号项内步长透传锚
// （`2,*/4` 的步长项仍按域下界）；下界 0 字段两锚法逐字等价作回归护栏。另钉 `?` 占位仅日/周位
// 合法且**校验与匹配同源**（否则「登记成功、永不触发」测不出）、周名 SUN..SAT 与月名 JAN..DEC
// 大小写不敏感展开、未知字母名仍逐字拒绝，零夹具恒跑）后全表求和 = 2073，与本机无夹具全量实跑
// pass=2073 fail=0 skip=35 逐条对上；带夹具实跑 pass=3089 fail=0 skip=1（套件仍 45 个，
// 三口径 45/45/45）。上面 2061/3077 那组是 cron 漏秒修复批的现值，原样保留。
// cron 端到端收口批 +4（tst_cronmanager 52→56：在 testStepAnchorAndQuartzAliases 尾部补
// scheduleJob + pollDueJobs 全链路——`0 30 9 ? * MON *` 登记成功、周一带毫秒余数节拍真触发、
// marker 落整秒 09:30:00、周二同时分不触发。纯静态判据测不出「validateCron 与 cronMatches
// 任一侧漏 preprocessCronField」造成的「登记成功、永不触发」，只有走全链路才钉得住同源契约；
// 零夹具恒跑）后全表求和 = 2077，与本机无夹具全量实跑 pass=2077 fail=0 skip=35 逐条对上；
// 带夹具实跑 pass=3093 fail=0 skip=1（套件仍 45 个，三口径 45/45/45）。上面 2073/3089 那组
// 是 cron 锚定与 Quartz 写法批的现值，原样保留。
// 上一登记「求和 1275 / 无夹具 1099」两处本就不等——1099 是
// s17.9 热力图批的旧值随批漏改，自本条起以实跑为准。
struct SuiteFloor {
    const char *name;
    int floor;
};
static const SuiteFloor kSuiteFloors[] = {
    {"lineending", 36},         {"messagebus", 0},          {"taskstore_lease", 0},
    {"worktree", 0},            {"agentteams", 0},          {"mcpmanager", 45},
    {"agentrecovery", 34},      {"backgroundtasks", 43},    {"workflowmanager", 121},
    {"workflowruntime", 3},     {"compacttokens", 57},      {"settingscache", 35},
    {"usagetokens", 82},        {"streamrate", 30},         {"goalmanager", 209},
    {"cronmanager", 56},        {"uiscale", 68},            {"fluconfigstore", 49},
    {"fluconfigutils", 4},      {"litepaths", 29},          {"pathguard", 21},
    {"configmerge", 14},        {"sessionstore", 4},        {"uiux", 11},
    {"tooltagkind", 26},        {"toolnames", 16},          {"navkeys", 14},
    {"layoutconst", 23},        {"agentconst", 16},         {"compacttranscript", 40},
    {"activityledger", 37},        {"heatmapgrid", 39},
    {"memoryrules", 76},
    {"toolschema", 55},
    {"sessionkeys", 45},
    {"projectinstructions", 49},
    {"updaterules", 198},
    {"contexttrim", 48},         {"providerspec", 110},
    {"scrollsettle", 32},
    {"modelprefs", 73},
    {"toolcallacc", 25},
    {"pastetext", 51},
    {"messagecontent", 60},
    {"imageattach", 93},
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
    failed += runSuite("memoryrules", tst_memoryrules);
    failed += runSuite("toolschema", tst_toolschema);
    failed += runSuite("sessionkeys", tst_sessionkeys);
    failed += runSuite("projectinstructions", tst_projectinstructions);
    failed += runSuite("updaterules", tst_updaterules);
    failed += runSuite("contexttrim", tst_contexttrim);
    failed += runSuite("providerspec", tst_providerspec);
    failed += runSuite("scrollsettle", tst_scrollsettle);
    failed += runSuite("modelprefs", tst_modelprefs);
    failed += runSuite("toolcallacc", tst_toolcallacc);
    failed += runSuite("pastetext", tst_pastetext);
    // s18.6 图片输入批：两张新纯头各一套件，全零夹具恒跑
    failed += runSuite("messagecontent", tst_messagecontent);
    failed += runSuite("imageattach", tst_imageattach);

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
