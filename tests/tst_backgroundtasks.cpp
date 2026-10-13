// tst_backgroundtasks.cpp —— s15 后台任务收割台账单测套件（对照 lcc s15 af26862 background_tasks_manager.py）
// 被测：src/BackgroundTasksManager —— start 编号自增 / shouldRunBackground 严格布尔门 /
//       recordResult 终态折叠 / hasPending 只查不清零 / collect 逐字模板与 200 截断
//
// 本套件零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。类是纯数据台账、
// 不建进程不建线程（零线程原则的登记端），直接值构造即用；BackgroundTasksManager.cpp
// 已按 CMakeLists 测试清单入编译（QtCore-only，第 6 个共享 TU）。
// 通知模板按 contains 标签组合断言（以 .cpp :67-74 实际模板为权威）——刻意不硬刻
// 整块文本：防缩进/字段顺序微调造成整块失配的假红，也防整串复制反而掩盖语义漂移。

#include "TestHarness.h"

#include "BackgroundTasksManager.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace {

// start()：id 形如 bg_0001 自增（m_counter 先 ++ 再零填充宽 4）
void testStartIdSequence()
{
    BackgroundTasksManager mgr;
    TestHarness::check(mgr.start(QStringLiteral("echo one"), QString()) == QStringLiteral("bg_0001"),
                       "首个任务 id 为 bg_0001（计数器起点 0、先自增后使用）");
    TestHarness::check(mgr.start(QStringLiteral("echo two"), QString()) == QStringLiteral("bg_0002"),
                       "第二个任务 id 为 bg_0002（同一台账内自增）");
}

// start()：空白命令直接拒绝，error 出参给 lcc 逐字文案；拒绝不占编号
void testStartEmptyCommand()
{
    BackgroundTasksManager mgr;
    QString error;
    TestHarness::check(mgr.start(QString(), QString(), &error).isEmpty(),
                       "空命令返回空 id（lcc ValueError 折叠为返回值）");
    TestHarness::check(error == QStringLiteral("Bash command cannot be empty"),
                       "error 出参逐字等于 lcc 文案（对照 ValueError 原文）");
    TestHarness::check(mgr.start(QStringLiteral("   "), QString(), &error).isEmpty(),
                       "纯空白同样按空命令拒绝（trimmed 判据）");
}

// shouldRunBackground()：bash 且 run_in_background 为严格 JSON 布尔 true——
// 字符串 "true"/数字 1/缺键/非 bash 一律不转后台（对齐 python "is True" 的静默前台）
void testShouldRunBackground()
{
    // 工具名用字面 wire 名：ToolNames::BASH 的值即 LLM 可见的线名，若常量改名此路转红
    // 恰是 wire 契约漂移的信号，应暴露而非回避
    QJsonObject on;
    on.insert(QStringLiteral("run_in_background"), true);
    TestHarness::check(BackgroundTasksManager::shouldRunBackground(QStringLiteral("bash"), on),
                       "bash + 严格布尔 true → 转后台");

    QJsonObject strTrue;
    strTrue.insert(QStringLiteral("run_in_background"), QStringLiteral("true"));
    TestHarness::check(!BackgroundTasksManager::shouldRunBackground(QStringLiteral("bash"), strTrue),
                       "字符串 \"true\" 不认（非 Bool 型即假）");

    QJsonObject numOne;
    numOne.insert(QStringLiteral("run_in_background"), 1);
    TestHarness::check(!BackgroundTasksManager::shouldRunBackground(QStringLiteral("bash"), numOne),
                       "数字 1 不认");

    QJsonObject missing;
    TestHarness::check(!BackgroundTasksManager::shouldRunBackground(QStringLiteral("bash"), missing),
                       "缺 run_in_background 键默认前台");

    QJsonObject otherTool;
    otherTool.insert(QStringLiteral("run_in_background"), true);
    TestHarness::check(
        !BackgroundTasksManager::shouldRunBackground(QStringLiteral("read_file"), otherTool),
        "非 bash 工具带 true 也不认（转后台判据为 bash 专属）");
}

// formatBashResult()：timedOut 或退出码 0 → 原样；否则 Error 前缀（前后台共用文案族）
void testFormatBashResult()
{
    TestHarness::check(
        BackgroundTasksManager::formatBashResult(QStringLiteral("all good"), 0, false)
            == QStringLiteral("all good"),
        "退出码 0 原样返回");
    TestHarness::check(
        BackgroundTasksManager::formatBashResult(QStringLiteral("partial output"), -1, true)
            == QStringLiteral("partial output"),
        "timedOut 原样返回（对应 lcc exit_code None 分支）");
    TestHarness::check(
        BackgroundTasksManager::formatBashResult(QStringLiteral("boom"), 1, false)
            == QStringLiteral("Error: command exited with status 1:\nboom"),
        "非零退出码逐字 Error 前缀（该前缀是 isToolFailure 的 Error: 承重判据）");
}

// recordResult()：completed 当且仅当 exitCode==0 且未超时，否则 failed；未知 id 静默无操作
void testRecordStatus()
{
    BackgroundTasksManager mgr;
    const QString okId = mgr.start(QStringLiteral("cmd ok"), QString());
    const QString failId = mgr.start(QStringLiteral("cmd fail"), QString());
    const QString timeoutId = mgr.start(QStringLiteral("cmd timeout"), QString());

    mgr.recordResult(okId, QStringLiteral("all good"), 0, false);
    mgr.recordResult(failId, QStringLiteral("boom"), 1, false);
    mgr.recordResult(timeoutId, QStringLiteral("partial"), -1, true);
    const QStringList notes = mgr.collect();

    // value(i) 越界返回空串不崩：行为回归时是红的断言而非炸的进程
    TestHarness::check(notes.value(0).contains(QStringLiteral("<status>completed</status>")),
                       "退出码 0 且未超时 → completed");
    TestHarness::check(notes.value(1).contains(QStringLiteral("<status>failed</status>")),
                       "非零退出码 → failed");
    TestHarness::check(notes.value(2).contains(QStringLiteral("<status>failed</status>")),
                       "超时 → failed（timedOut 优先于退出码折叠）");
    TestHarness::check(
        notes.value(1).contains(QStringLiteral("Error: command exited with status 1:")),
        "失败任务的 summary 携带 formatBashResult 的 Error 前缀文本");

    // 未知 id（lcc run() 的 get 为空早退：任务已被收割）不得凭空入册
    mgr.recordResult(QStringLiteral("bg_9999"), QStringLiteral("ghost"), 0, false);
    TestHarness::check(!mgr.hasPending(),
                       "未知 id 记账为无操作（不产生待收条目、不崩溃）");
}

// hasPending()：只查询、不清零，收割唯一归 collect()（lcc s15 af26862 has_pending :132-135）
void testHasPendingLifecycle()
{
    BackgroundTasksManager mgr;
    TestHarness::check(!mgr.hasPending(), "新台账无待收");
    const QString id = mgr.start(QStringLiteral("echo ping"), QString());
    TestHarness::check(!mgr.hasPending(),
                       "已登记但未终态不算待收（running 不触发唤醒，对照 lcc 终态判据）");
    mgr.recordResult(id, QStringLiteral("pong"), 0, false);
    TestHarness::check(mgr.hasPending(), "终态落账一条即有待收");
    TestHarness::check(mgr.hasPending(),
                       "连续第二次查询仍为真——只查不清零（清零唯一发生在 collect）");
    (void)mgr.collect();
    TestHarness::check(!mgr.hasPending(), "collect 收割后转假");
}

// collect()：一次性的 <task_notification> 渲染（模板对照 lcc s15 af26862 :116-123，
// 字段名 summary、旧名 result 禁回归）
void testCollectTemplate()
{
    BackgroundTasksManager mgr;
    const QString id = mgr.start(QStringLiteral("echo hi"), QStringLiteral("tu-1"));
    mgr.recordResult(id, QStringLiteral("hello world"), 0, false);
    const QStringList notes = mgr.collect();

    TestHarness::check(notes.size() == 1, "单任务收割一条");
    const QString note = notes.value(0);
    TestHarness::check(note.contains(QStringLiteral("<task_notification>")), "通知开标签逐字");
    TestHarness::check(note.contains(QStringLiteral("</task_notification>")), "通知闭标签逐字");
    TestHarness::check(note.contains(QStringLiteral("<task_id>bg_0001</task_id>")), "task_id 字段");
    TestHarness::check(note.contains(QStringLiteral("<status>completed</status>")), "status 字段");
    TestHarness::check(note.contains(QStringLiteral("<command>echo hi</command>")), "command 字段");
    TestHarness::check(note.contains(QStringLiteral("<summary>hello world</summary>")),
                       "summary 字段（lcc s15 af26862 的字段名，对照 :116-123）");
    TestHarness::check(!note.contains(QStringLiteral("<result>")),
                       "旧字段名 <result> 不出现（s15 改名 summary，防回退漂移）");
    TestHarness::check(mgr.collect().isEmpty(), "第二次收割为空（一次性消费）");
}

// collect()：summary 截前 200 字符且无「已截断」标记（lcc §11-3 已知坑，沿 lcc 不修——
// 尾部关键报错会被切掉，本组断言的正是这个行为本身）；收割顺序为记账 FIFO
void testCollectTruncationAndOrder()
{
    BackgroundTasksManager mgr;

    // ① 超长截断：200×'A' + 标记串 = 209 字符，left(200) 后标记必消失
    const QString longId = mgr.start(QStringLiteral("long cmd"), QString());
    QString longOutput(200, QLatin1Char('A'));
    longOutput += QStringLiteral("CUTMARKER");
    mgr.recordResult(longId, longOutput, 0, false);
    const QString longNote = mgr.collect().value(0);
    TestHarness::check(longNote.contains(QString(200, QLatin1Char('A'))),
                       "summary 保留前 200 字符");
    TestHarness::check(!longNote.contains(QStringLiteral("CUTMARKER")),
                       "第 201 字符起内容被切掉且无截断标记（left(200)，lcc §11-3）");

    // ② 恰好 200 字符完整保留（left(200) 对 ≤200 恒等，截断门不含糊）
    const QString edgeId = mgr.start(QStringLiteral("edge cmd"), QString());
    mgr.recordResult(edgeId, QString(200, QLatin1Char('B')), 0, false);
    TestHarness::check(mgr.collect().value(0).contains(QString(200, QLatin1Char('B'))),
                       "恰 200 字符结果一字不减");

    // ③ 同批多条：收割序跟记账序（m_ready FIFO），与登记序无关
    const QString first = mgr.start(QStringLiteral("one"), QString());   // bg_0003
    const QString second = mgr.start(QStringLiteral("two"), QString());  // bg_0004
    mgr.recordResult(second, QStringLiteral("late"), 0, false);          // 先记 second
    mgr.recordResult(first, QStringLiteral("early"), 0, false);
    const QStringList notes = mgr.collect();
    TestHarness::check(notes.size() == 2, "同批两条终态收割两条");
    TestHarness::check(notes.value(0).contains(QStringLiteral("<task_id>bg_0004</task_id>")),
                       "首条是记账序更前的 bg_0004（m_ready FIFO，非登记序）");
}

} // namespace

int tst_backgroundtasks()
{
    const int before = TestHarness::failCount();
    testStartIdSequence();           // 2
    testStartEmptyCommand();         // 3
    testShouldRunBackground();       // 5
    testFormatBashResult();          // 3
    testRecordStatus();              // 5
    testHasPendingLifecycle();       // 5
    testCollectTemplate();           // 9
    testCollectTruncationAndOrder(); // 5
    return TestHarness::failCount() - before;
}
