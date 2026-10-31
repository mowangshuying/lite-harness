// tst_litepaths.cpp —— 运行时路径锚点单源直测（src/LitePaths.h，header-only 零 CMake 改动）
// 被测：存储根反推 / 根下固定文件 / 会话存储区中间层 / 单会话数据根（含空 ID 回退）。
//
// 本套件为什么必须存在（不是凑覆盖率）：锚点派生错一格**没有任何失败信号**。
//   · 根算成 <exe>/config 而非 <exe>/../config：程序照跑，只是数据落到 bin 里，
//     下次重新打包整个 build/bin 被 CMake 的 staging 清理剥掉——用户表现为「升级后历史全没」；
//   · `.lite-harness` 中间层被拼两次：引擎各自再拼叶子段，落出 .lite-harness/.lite-harness/，
//     老数据读不到且不报错（AgentConstants.h 明令拼接形态逐字符不可改）；
//   · 空 dataId 回退形态改掉：LiteHarness 的删除护栏就失去判据，最坏情况递归删掉整个会话存储区。
// 三条都是「跑得通但结果不对」的路径，只能靠断言钉死，冒烟运行看不全。
//
// 与 tst_uiscale / tst_settingscache 同口径：只测**判据与折算法**，不证明 App.cpp 真的
// 在建根之后才读写配置（那是接线，验证手段是编译期等价 + 冒烟运行），也不证明
// 三个消费点都改走了本头（grep 收口，见 AGENTS.md「数据与路径」条）。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行——本头只做字符串派生，
// 不 mkpath、不判存在，因此没有需要门控的落盘副作用。

#include "TestHarness.h"

#include "LitePaths.h"

#include <QString>

namespace {

using TestHarness::check; // 本套件断言密度高，逐条写 TestHarness:: 前缀会淹没判据本身

// 固定桩：发布包与开发态两种 exe 形态。真值取自 CMakeLists 的产物布局
// （发布包 <pkg>/bin/lite-harness.exe + <pkg>/config；开发态 build/bin + build/config），
// 两处都满足「存储根 = exe 目录的上一级里的 config」。
const char *kPkgExeDir = "D:/lite-harness-s17.2-win64/bin";
const char *kPkgRoot = "D:/lite-harness-s17.2-win64/config";
const char *kDevExeDir = "D:/WsyWorkSpaces/github/lite-harness/build/bin";
const char *kDevRoot = "D:/WsyWorkSpaces/github/lite-harness/build/config";

// 会话数据根的叶子段形态：sessions/<id>
const char *kDataId = "ce0b7305";

} // namespace

int tst_litepaths()
{
    // ---- ① 存储根反推：唯一锚点是 exe 目录，与进程 CWD 无关 ----
    // 发布包形态：<pkg>/bin → <pkg>/config（".." 必须被 cleanPath 折叠掉，
    // 产出形态里不该再留 ".."——留着就等于把折叠责任推给每个消费点）
    const QString pkgRoot = LitePaths::configRootFor(QString::fromLatin1(kPkgExeDir));
    check(pkgRoot == QString::fromLatin1(kPkgRoot), "configRootFor: package layout -> <exe>/../config");
    check(!pkgRoot.contains(QStringLiteral("..")), "configRootFor: '..' folded out of result");
    check(QDir::isAbsolutePath(pkgRoot), "configRootFor: result is absolute");
    // 开发态形态：build/bin → build/config（与发布包同一条规则，不是两套特例）
    check(LitePaths::configRootFor(QString::fromLatin1(kDevExeDir))
              == QString::fromLatin1(kDevRoot),
          "configRootFor: dev layout -> build/config");

    // 尾斜杠 / 反斜杠 / 冗余分隔符：同一目录的三种写法必须收敛到同一个根。
    // 这条不是洁癖——根来自 applicationDirPath()/argv[0] 反推，Windows 下它本来就给反斜杠形态，
    // 不归一就会让同一个目录派生出多个字符串根。
    //
    // 基准取「同一逻辑目录的朴素写法」的派生值，而不是上面那个发布包样例根：两者是**不同目录**
    // （D:/pkg vs D:/lite-harness-s17.2-win64），拿样例根当基准会恒假——第一版就栽在这里，
    // 三条归一化断言全红而实现无错。判据是「三种写法彼此相等」，基准必须由其中最朴素的一种现推。
    //
    // 反斜杠→正斜杠的转换来自 QDir::cleanPath 的 Windows 段归一（QDir::separator()=='/'），
    // 本仓 CI 与产物均为 MSVC/Windows，故按 Windows 语义断言；若将来上 POSIX，这条要跟着平台门控。
    const QString pkgBinRoot = LitePaths::configRootFor(QStringLiteral("D:/pkg/bin"));
    check(pkgBinRoot == QStringLiteral("D:/pkg/config"),
          "configRootFor: plain form folds '..' onto the sibling config dir");
    check(LitePaths::configRootFor(QStringLiteral("D:/pkg/bin/")) == pkgBinRoot,
          "configRootFor: trailing slash normalized");
    check(LitePaths::configRootFor(QStringLiteral("D:\\pkg\\bin")) == pkgBinRoot,
          "configRootFor: backslash form normalized");
    check(LitePaths::configRootFor(QStringLiteral("D:/pkg/./bin")) == pkgBinRoot,
          "configRootFor: redundant '.' segment folded");

    // 空/全空白入参 → 空串：本头绝不猜一个根。猜出来的 "/config" 在 Windows 下会解析成
    // 「当前驱动器根」，而这条错路径一旦被 static 钉住就永久生效（AppSettings::reader 同款理由）。
    check(LitePaths::configRootFor(QString()).isEmpty(), "configRootFor: empty exeDir -> empty");
    check(LitePaths::configRootFor(QStringLiteral("   ")).isEmpty(),
          "configRootFor: whitespace-only exeDir -> empty");

    // ---- ② 根下固定文件：一个根两个文件，拼法单源 ----
    const QString root = QString::fromLatin1(kPkgRoot);
    check(LitePaths::configFilePathFor(root) == root + QStringLiteral("/config.json"),
          "config file lives directly under the storage root");
    check(LitePaths::indexFilePathFor(root) == root + QStringLiteral("/index.json"),
          "index lives directly under the storage root (not inside .lite-harness)");
    // 空根必须被拦住，而不是拼出 "/config.json" 这种「看起来能用、实际落到驱动器根」的路径。
    // joined() 内是 Q_ASSERT： NDEBUG（Release）下断言被编译掉，故这条形态在此钉**结果**
    // 而非崩溃——空根拼出的串绝不能被当成合法路径静默返回。
    // 注：本条在 Debug 下会先被断言拦停，这正是我们要的行为（测试构建为 Debug/Release 皆可跑，
    // Release 下退化为「不炸但值仍错」的观测，因此这里只断言非 Debug 语义能观察到的部分）。
    // 不断言空根返回值，避免把 Q_ASSERT 的两种编译形态都写进期望。

    // ---- ③ 会话存储区：`.lite-harness` 中间层只出现一次 ----
    const QString store = LitePaths::storeRootFor(root);
    check(store == root + QStringLiteral("/.lite-harness"), "store root is <root>/.lite-harness");
    check(LitePaths::sessionsRootFor(root) == store + QStringLiteral("/sessions"),
          "sessions root is <root>/.lite-harness/sessions");
    check(LitePaths::skillsDirFor(root) == store + QStringLiteral("/skills"),
          "skills dir is <root>/.lite-harness/skills (global, not per-workDir)");
    // 双层嵌套是本头最容易犯且最难发现的错（引擎侧各拼叶子段，中间层多拼一次就静默分叉）。
    // 逐条数出现次数，而不是比对整串——整串比对能过而中间层重复的形态不存在，但反向可以：
    // 一旦有人把 sessions 拼成 .lite-harness/.lite-harness/sessions，整串比对会失败但报错信息含糊。
    auto countOccurrences = [](const QString &hay, const QString &needle) {
        int n = 0;
        int from = 0;
        while ((from = hay.indexOf(needle, from, Qt::CaseSensitive)) != -1) {
            ++n;
            from += needle.size();
        }
        return n;
    };
    check(countOccurrences(store, QStringLiteral(".lite-harness")) == 1,
          "store root contains .lite-harness exactly once");
    check(countOccurrences(LitePaths::sessionsRootFor(root), QStringLiteral(".lite-harness")) == 1,
          "sessions root contains .lite-harness exactly once");
    check(countOccurrences(LitePaths::skillsDirFor(root), QStringLiteral(".lite-harness")) == 1,
          "skills dir contains .lite-harness exactly once");
    check(countOccurrences(LitePaths::sessionDataRootFor(root, QString::fromLatin1(kDataId)),
                           QStringLiteral(".lite-harness")) == 1,
          "session data root contains .lite-harness exactly once");

    // ---- ④ 单会话数据根：隔离形态与空 ID 回退形态 ----
    const QString dataRoot = LitePaths::sessionDataRootFor(root, QString::fromLatin1(kDataId));
    check(dataRoot == LitePaths::sessionsRootFor(root) + QStringLiteral("/")
                          + QString::fromLatin1(kDataId),
          "session data root is <root>/.lite-harness/sessions/<dataId>");
    // 删除护栏（LiteHarness::destroySessionPage）的判据就是这条前缀关系：
    // 只有严格落在 sessionsRoot 之下的目录才允许 removeRecursively。
    // 这条断言是护栏的**前提**，不是护栏的重复——前提漂了，护栏会静默变成恒假或恒真。
    check(dataRoot.startsWith(LitePaths::sessionsRootFor(root) + QLatin1Char('/'),
                              Qt::CaseInsensitive),
          "session data root is strictly under sessions root (delete-guard precondition)");
    check(dataRoot != LitePaths::sessionsRootFor(root),
          "session data root is not the sessions root itself (guard needs strict containment)");

    // 空 dataId → 回退会话存储区本身。改造前 AgentLoop::sessionDataRoot() 就是这个形态，
    // 逐字保留：未注入 ID 的独立构造路径行为不变，且删除护栏据此判「未隔离」而拒删。
    check(LitePaths::sessionDataRootFor(root, QString()) == store,
          "empty dataId falls back to the store root (pre-refactor shape)");
    // 回退形态**不得**落在 sessionsRoot 之下——否则「未隔离」会被误判成合法会话目录，
    // 删除护栏的前缀检查就恒真，删一个未隔离会话等于删全部会话。
    check(!store.startsWith(LitePaths::sessionsRootFor(root) + QLatin1Char('/'),
                            Qt::CaseInsensitive),
          "store root is NOT under sessions root (delete-guard must reject the fallback)");

    // ---- ⑤ 跨锚点一致性：同一个根派生的四个位置必须互不重叠且都在根内 ----
    // 改造前这四条路径分属三套锚（exe 目录 / 进程 CWD / workDir），本组断言是「统一」这件事
    // 本身的可执行定义：锚点若再次分叉，这里立刻红。
    check(LitePaths::configFilePathFor(root).startsWith(root + QLatin1Char('/')),
          "config.json is under the storage root");
    check(LitePaths::indexFilePathFor(root).startsWith(root + QLatin1Char('/')),
          "index.json is under the storage root");
    check(store.startsWith(root + QLatin1Char('/')), "store root is under the storage root");
    check(LitePaths::skillsDirFor(root) != LitePaths::sessionsRootFor(root),
          "skills and sessions are distinct siblings");
    // 索引与会话数据同根，但索引不在 .lite-harness 里（用户裁定的目标布局）。
    check(!LitePaths::indexFilePathFor(root).startsWith(store + QLatin1Char('/')),
          "index.json sits at the storage root, not inside .lite-harness");

    return TestHarness::failCount() == 0 ? 0 : 1;
}
