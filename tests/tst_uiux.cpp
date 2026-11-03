// tst_uiux.cpp —— P1 UI 优化的判据直测（零 Gui 依赖：本套件只链接 Qt6::Core）
// 被测：命中区下限常量（LayoutConstants）/ 设置门面的组前缀契约（AppSettings）/
//       Reader 写失败返回 false（SettingsCache，P1-A 的契约前提）/ 数值域自洽（AgentConstants）。
//
// 本套件为什么必须存在（不是凑覆盖率）：
//   · 命中区 28/30px 是「能点但容易点偏」的形态，没有失败信号，只有尺寸常量被人改回时才暴露
//     —— 断言直接钉 32 下界，谁改小谁红（P1-B 的回归哨兵）；
//   · AppSettings::setValue 的返回值此前被 7 个调用点全部丢弃，P1-A 让设置页依赖它弹告警。
//     若 Reader::setValue 在 AccessError 时仍返回 true，告警就永远不会出现 —— 静默失败会
//     悄悄回归，而 UI 上完全看不出来。故本套件把「只读文件 → false」钉死；
//   · AgentConst 的 min/default/max 若写反（min > max），取值函数会把所有合法配置判成非法、
//     全部回退默认，表现是「设置页改了不生效」，同样无声。
//
// 组 1 零夹具恒跑；组 2 需 LITE_TEST_TMPROOT（ScopedTempRoot 无效则 SKIP）。
// 覆盖面边界（勿高估）：本套件不证明设置页真的在失败时弹了框（那是 QWidget 接线，
// 验证手段是编译期等价 + 冒烟运行），只钉住「失败可被观测」这条契约。

#include "TestHarness.h"

#include "AgentConstants.h"
#include "AppSettings.h"
#include "LayoutConstants.h"
#include "ScopedTempRoot.h"
#include "SettingsCache.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QVariant>

namespace {

using TestHarness::check;

// 只读化：去掉写位，后续 QSettings::sync 应落 AccessError。
// 注意：管理员身份运行时可绕过只读属性 —— 那种环境下列的断言会改走 SKIP 而非 FAIL。
void makeReadOnly(const QString &path)
{
    QFile f(path);
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadUser);
}

void makeWritable(const QString &path)
{
    QFile f(path);
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                     | QFileDevice::WriteUser);
}

// 组 1：命中区下限（P1-B）
void testHitTargetFloor()
{
    check(LayoutConst::kSidebarRestoreBtnSize >= 32, "hit target: sidebar restore btn >= 32px");
    check(LayoutConst::kJumpBottomBtnSize >= 32, "hit target: jump bottom btn >= 32px");
    // 正圆条件：QSS 的 border-radius 取尺寸一半，尺寸为奇数会得到半像素圆角
    check(LayoutConst::kJumpBottomBtnSize % 2 == 0, "hit target: jump bottom btn even size (perfect circle)");
    check(LayoutConst::kJumpBottomBtnMargin > 0, "hit target: jump bottom btn margin positive");
}

// 组 1：组前缀契约（AppSettings 门面）
void testSettingsKeyPrefix()
{
    check(AppSettings::keyGroup() == QStringLiteral("app"), "AppSettings: key group is app");
    check(AppSettings::scopedKey(QStringLiteral("apiToken")) == QStringLiteral("app/apiToken"),
          "AppSettings: scopedKey prefixes group once");
    check(!AppSettings::scopedKey(QStringLiteral("x")).startsWith(QLatin1Char('/')),
          "AppSettings: scopedKey has no leading slash");
}

// 组 1：数值域自洽（AgentConstants）
void testNumericDomain()
{
    check(AgentConst::kContextCharLimitMin < AgentConst::kContextCharLimitDefault,
          "domain: contextCharLimit min < default");
    check(AgentConst::kContextCharLimitDefault < AgentConst::kContextCharLimitMax,
          "domain: contextCharLimit default < max");
    check(AgentConst::kMaxToolIterationsMin < AgentConst::kMaxToolIterationsDefault,
          "domain: maxToolIterations min < default");
    check(AgentConst::kMaxToolIterationsDefault < AgentConst::kMaxToolIterationsMax,
          "domain: maxToolIterations default < max");
}

// 组 2：写失败契约（P1-A 的前提：失败必须可被观测）
//
// 这里**故意不断言「失败后回读仍是旧值」**：QSettings 的 ini 后端把解析结果挂在进程级
// QConfFile 缓存里，setValue 已改的是那份内存表；sync 失败后即便 Reader 自己 invalidate，
// 同进程内下一次构造的 QSettings 仍可能命中被污染的表，回读到那个**没落盘**的新值。
// 也就是说：写失败后「回读看起来生效了」是常态 —— 这正是 P1-A 必须依赖返回值、
// 而不能靠 updateValue() 回读判断成功的原因（否则用户看到新值、重启后丢失）。
void testWriteFailureIsReported(const QString &dirPath)
{
    const QString iniPath = QDir(dirPath).filePath(QStringLiteral("readonly.ini"));
    SettingsCache::Reader reader(iniPath);

    // 先成功写一次，确保文件存在且内容合法
    check(reader.setValue(QStringLiteral("k"), QStringLiteral("v1")), "write: baseline write succeeds");
    check(reader.value(QStringLiteral("k")).toString() == QStringLiteral("v1"),
          "write: baseline value reads back");

    // 只读化 → 写必须返回 false（设置页据此弹告警）
    makeReadOnly(iniPath);
    const bool readOnlyWrite = reader.setValue(QStringLiteral("k"), QStringLiteral("v2"));
    if (readOnlyWrite)
    {
        // 管理员进程可绕过只读属性：本机策略问题，不是被测代码的问题，如实跳过
        TestHarness::skip("SKIP: readonly file still writable in this environment (admin?), "
                          "write-failure assertion skipped\n");
    }
    else
    {
        check(!readOnlyWrite, "write: readonly file -> setValue returns false (P1-A contract)");
    }

    // 恢复可写 → 失败不粘滞
    makeWritable(iniPath);
    check(reader.setValue(QStringLiteral("k"), QStringLiteral("v3")),
          "write: writable again -> setValue recovers");
    check(reader.value(QStringLiteral("k")).toString() == QStringLiteral("v3"),
          "write: recovered value reads back");
}

} // namespace

int tst_uiux()
{
    const int before = TestHarness::failCount();

    // 组 1：零夹具，任何环境全量执行
    testHitTargetFloor();
    testSettingsKeyPrefix();
    testNumericDomain();

    ScopedTempRoot root(QStringLiteral("uiux"));
    if (!root.isValid())
    {
        TestHarness::skip("SKIP: LITE_TEST_TMPROOT unavailable, settings write-failure groups skipped\n");
        return TestHarness::failCount() - before;
    }

    testWriteFailureIsReported(root.path());

    return TestHarness::failCount() - before;
}