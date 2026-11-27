// tst_bashrunner.cpp —— bash 执行链纯判据直测（src/BashRunner.cpp，仅 QtCore）。
//
// 被测三函数：dangerWarning（危险黑名单短路）/ truncateOutput（输出收尾）/
// finalizeOutput（进程结束取输出）。`start()` 起真进程 + 挂 QTimer，属接线层，不在本套件。
//
// 为什么必须测（AGENTS「覆盖现状」登记 `BashRunner::dangerWarning` 安全关键却 0 直测，本批兑现）：
// ① dangerWarning 的返回串以 "Error: " 开头，这不只是文案——它是「工具失败」的承重判据
//    （AgentRecovery.h::isToolFailure 按前缀判定，tst_backgroundtasks 守的就是这个前缀）。
//    前缀被改 → 被拦下的危险命令在台账/恢复判据里**记成成功**，UI 上看不到失败，
//    而命令确实没执行——「拦住了却报告没拦住」是安全语义里最坏的一种失真。
// ② 匹配口径是「大小写不敏感 contains」而非正则/词边界。改成正则即被元字符炸；
//    改成词边界则 `rm -rf ~` 这类嵌在中间的子串漏放。两者都不报错、只是静默放行。
// ③ truncateOutput 的判据是 `>` 不是 `>=`，且兜底判据是 `isEmpty()` 不是
//    `trimmed().isEmpty()`——差一反向即「超大工具结果整段进上下文，下一请求静默溢出」。
// ④ finalizeOutput 的超时分支必须在读缓冲**之前**返回：kill 之后残留输出若计入结果，
//    就把「超时」伪装成「跑完了但输出是这样」。本套件用 nullptr 进程钉死这一点——
//    超时分支若被改成先碰 process，当场段错误、ctest 判红，这正是要的效果。
//
// 真黑名单表本身（AgentLoopDetail::bashDenyList，14 条目）不在本套件覆盖内：它定义在
// AgentLoopPermission.cpp，该 TU include AgentLoop.h → QOpenAi.h（链 Qt6::Network）+ 带
// Q_OBJECT 需 moc，按 CMakeLists「仅允许 QtCore-only 引擎进测试清单」的硬纪律进不来。
// 本套件测的是**注入 list 后的匹配语义**（dangerWarning 的 denyList 形参就是为此而存在）。
// 表内容本身要直测需先仿 LineEnding.h 抽纯头，登记为下一批最小切口。
//
// 零夹具依赖，任何环境全量执行（进程组在 cmd.exe 不可用时诚实 skip，CI 恒不触发）。

#include "TestHarness.h"

#include "BashRunner.h"
#include "AgentConstants.h"

#include <QProcess>
#include <QString>
#include <QStringList>

namespace {

using TestHarness::check;

// 与生产表**无关**的局部样本：本套件测匹配语义，不测表内容（理由见文件头）。
QStringList sampleList()
{
    return QStringList{
        QStringLiteral("rm -rf /"),
        QStringLiteral("sudo"),
        QStringLiteral("dd if="),
    };
}

} // namespace

int tst_bashrunner()
{
    const int before = TestHarness::failCount();

    // ---- ① dangerWarning：空输入与不命中 ----
    check(BashRunner::dangerWarning(QStringLiteral("git status"), sampleList()).isEmpty(),
          "bashrunner: benign command passes (empty return = not blocked)");
    check(BashRunner::dangerWarning(QStringLiteral("ls"), QStringList()).isEmpty(),
          "bashrunner: empty deny list blocks nothing");
    check(BashRunner::dangerWarning(QString(), sampleList()).isEmpty(),
          "bashrunner: empty command is not blocked");
    check(BashRunner::dangerWarning(QString(), QStringList()).isEmpty(),
          "bashrunner: empty command + empty list -> empty");

    // ---- ② dangerWarning：命中判定 ----
    check(!BashRunner::dangerWarning(QStringLiteral("rm -rf /"), sampleList()).isEmpty(),
          "bashrunner: exact deny entry hits");
    check(!BashRunner::dangerWarning(QStringLiteral("sudo apt install x"), sampleList()).isEmpty(),
          "bashrunner: entry hit at offset 0");
    check(!BashRunner::dangerWarning(QStringLiteral("echo hi && sudo reboot"), sampleList()).isEmpty(),
          "bashrunner: entry hit mid-command (substring, not word boundary)");
    // 这条不是在夸表现好，是在**钉住缺陷**：只带尾斜杠的条目盖不住 `rm -rf ~`。
    // 生产表靠 lite 追加的裸 `rm -rf` 条目兜住它（AgentLoopPermission.cpp 注释自述），
    // 一旦有人「精简」掉那条追加项，本断言仍为真但上游防线消失——故此处把「精确子串
    // 会漏」这个事实本身钉成可见的注释，防下一轮误删追加项。
    check(BashRunner::dangerWarning(QStringLiteral("rm -rf ~"), sampleList()).isEmpty(),
          "bashrunner: 'rm -rf ~' does NOT hit the trailing-slash entry 'rm -rf /' "
          "(exact-substring gap the bare 'rm -rf' addendum exists to cover)");
    check(!BashRunner::dangerWarning(QStringLiteral("dd if=/tmp/a of=/dev/sda"),
                                     sampleList())
               .isEmpty(),
          "bashrunner: 'dd if=' hits");

    // ---- ③ dangerWarning：大小写不敏感（Windows 命令名本就不区分）----
    check(!BashRunner::dangerWarning(QStringLiteral("RM -RF /"), sampleList()).isEmpty(),
          "bashrunner: upper-case command hits lower-case entry");
    check(!BashRunner::dangerWarning(QStringLiteral("sudo"),
                                     QStringList{QStringLiteral("SUDO")}).isEmpty(),
          "bashrunner: lower-case command hits upper-case entry");
    check(!BashRunner::dangerWarning(QStringLiteral("EcHo SuDo"), sampleList()).isEmpty(),
          "bashrunner: mixed case nested in other text hits");

    // ---- ④ dangerWarning：匹配是纯子串、不是正则 ----
    // 若有人「顺手」把 contains 换成 QRegularExpression，这些元字符会当场改变判定。
    check(!BashRunner::dangerWarning(QStringLiteral("x$*y"),
                                     QStringList{QStringLiteral("$*")}).isEmpty(),
          "bashrunner: regex metacharacters match literally (contains, not regex)");
    check(BashRunner::dangerWarning(QStringLiteral("aXb"),
                                    QStringList{QStringLiteral("a*b")}).isEmpty(),
          "bashrunner: '*' in entry is not a wildcard (aXb must NOT match 'a*b')");
    check(!BashRunner::dangerWarning(QStringLiteral("line1\nsudo line2"), sampleList()).isEmpty(),
          "bashrunner: multi-line command still matches");

    // ---- ⑤ dangerWarning：返回文案逐字（承重契约）----
    const QString blocked = QStringLiteral("sudo reboot now");
    const QString reason = BashRunner::dangerWarning(blocked, sampleList());
    check(reason == QStringLiteral("Error: Dangerous command blocked: %1").arg(blocked),
          "bashrunner: reason text is verbatim 'Error: Dangerous command blocked: <command>'");
    check(reason.startsWith(QStringLiteral("Error: ")),
          "bashrunner: reason starts with 'Error: ' (isToolFailure load-bearing prefix)");
    check(reason.contains(blocked),
          "bashrunner: reason echoes the ORIGINAL command, not the matched entry");
    // 把「实现里 .arg(command) 误写成 .arg(danger)」当场抓出来：回显必须保留命令原样大小写，
    // 而命中项是另一串（这里两者大小写不同，误写即转红）。
    check(BashRunner::dangerWarning(QStringLiteral("Please Sudo Now"),
                                    QStringList{QStringLiteral("sudo")})
              == QStringLiteral("Error: Dangerous command blocked: Please Sudo Now"),
          "bashrunner: echo target is the verbatim command, not the (differently-cased) entry");

    // ---- ⑥ dangerWarning：空条目等于全拦（与 AgentPathGuard startsWith("") 同族坑）----
    // QString::contains("") 恒真，故表里混进一枚空串会让**所有** bash 调用被拒。
    // 这条不是「希望如此」，是把静默失效面钉成断言：加条目时误留空串，本条即转红。
    check(!BashRunner::dangerWarning(QStringLiteral("git status"),
                                     QStringList{QStringLiteral("")}).isEmpty(),
          "bashrunner: an empty entry blocks EVERYTHING (contains(\"\") is always true) -- "
          "guards against a stray empty line in the deny table");

    // ---- ⑦ dangerWarning：入参不被改写 ----
    QString keep = QStringLiteral("sudo x");
    BashRunner::dangerWarning(keep, sampleList());
    check(keep == QStringLiteral("sudo x"), "bashrunner: dangerWarning leaves command intact");

    // ---- ⑧ truncateOutput：上限与兜底 ----
    check(BashRunner::truncateOutput(QString()).compare(QStringLiteral("(no output)")) == 0,
          "bashrunner: empty output falls back to '(no output)'");
    check(BashRunner::truncateOutput(QStringLiteral("hello")) == QStringLiteral("hello"),
          "bashrunner: short output passes through unchanged");
    check(BashRunner::truncateOutput(QStringLiteral("   ")) == QStringLiteral("   "),
          "bashrunner: whitespace-only is NOT replaced by '(no output)' (isEmpty, not trimmed)");

    const QString atLimit(QString(AgentConst::kOutputCharLimit, QLatin1Char('a')));
    check(BashRunner::truncateOutput(atLimit).length() == AgentConst::kOutputCharLimit,
          "bashrunner: length == limit is kept whole (judgement is '>', not '>=')");
    check(BashRunner::truncateOutput(atLimit) == atLimit,
          "bashrunner: length == limit returns the identical string");

    const QString overLimit(QString(AgentConst::kOutputCharLimit + 1, QLatin1Char('b')));
    const QString cut = BashRunner::truncateOutput(overLimit);
    check(cut.length() == AgentConst::kOutputCharLimit,
          "bashrunner: length == limit+1 is cut to exactly the limit");
    check(!cut.isEmpty(), "bashrunner: a cut oversized string is not the fallback text");
    check(cut.at(0) == QLatin1Char('b') && cut.at(cut.size() - 1) == QLatin1Char('b'),
          "bashrunner: cut keeps the HEAD (left), not the tail");

    const QString wayOver(QString(AgentConst::kOutputCharLimit * 3, QLatin1Char('c')));
    check(BashRunner::truncateOutput(wayOver).length() == AgentConst::kOutputCharLimit,
          "bashrunner: 3x oversize also lands on the limit");
    check(BashRunner::truncateOutput(wayOver).length() <= AgentConst::kOutputCharLimit,
          "bashrunner: result never exceeds the limit (invariant)");

    QString src = QStringLiteral("keepme-tail");
    const QString srcCopy = src;
    BashRunner::truncateOutput(src);
    check(src == srcCopy, "bashrunner: truncateOutput takes text by value, caller's string intact");

    // ---- ⑨ finalizeOutput：超时分支必须在读缓冲之前 ----
    // nullptr 进程 + timedOut=true：函数体若先碰 process 就当场段错误（ctest 判红），
    // 这正是本条要的护栏——「kill 后残留输出计入结果会把超时伪装成正常结束」不可回退。
    check(BashRunner::finalizeOutput(nullptr, true) == AgentConst::kBashTimeoutError,
          "bashrunner: timed-out branch returns kBashTimeoutError without touching the process");
    check(BashRunner::finalizeOutput(nullptr, true).startsWith(QStringLiteral("Error: ")),
          "bashrunner: timeout text carries the 'Error: ' failure prefix");
    check(BashRunner::finalizeOutput(nullptr, true)
              .contains(QStringLiteral("%1s").arg(AgentConst::kBashTimeoutMs / 1000)),
          "bashrunner: timeout text derives from kBashTimeoutMs (no drift from the ms constant)");

    // ---- ⑩ finalizeOutput：正常分支走 truncateOutput ----
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QStringLiteral("cmd.exe"),
               {QStringLiteral("/c"), QStringLiteral("echo"),
                QStringLiteral("BASHRUNNER_PROBE")});
    if (!proc.waitForFinished(8000)) {
        TestHarness::skip("bashrunner: SKIP process group (cmd.exe unavailable)\n");
    } else {
        const QString out = BashRunner::finalizeOutput(&proc, false);
        check(out.contains(QStringLiteral("BASHRUNNER_PROBE")),
              "bashrunner: normal branch returns the process stdout");
        check(!out.startsWith(QStringLiteral("Error: ")),
              "bashrunner: normal branch is not reported as a failure");
        check(out != BashRunner::finalizeOutput(nullptr, true),
              "bashrunner: the two branches are mutually exclusive");

        QProcess quiet;
        quiet.setProcessChannelMode(QProcess::MergedChannels);
        quiet.start(QStringLiteral("cmd.exe"),
                    {QStringLiteral("/c"), QStringLiteral("exit")});
        if (quiet.waitForFinished(8000)) {
            check(BashRunner::finalizeOutput(&quiet, false)
                          .compare(QStringLiteral("(no output)"))
                      == 0,
                  "bashrunner: silent process falls back to '(no output)'");
        }
    }

    return TestHarness::failCount() - before;
}
