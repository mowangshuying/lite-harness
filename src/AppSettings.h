#pragma once

// 设置存储单源头：全仓用户配置一律落 exe 同目录 settings.ini（用户裁决：弃用注册表，
// 禁再引入默认构造 QSettings——org/app 注册表路径已成历史）。
// 键清单：defaultWorkDir / sidebarVisible / language / contextCharLimit / maxToolIterations
// / apiBaseUrl / apiToken / modelOptions / defaultModel / maxRetries / fallbackModelId
// / goalEvaluatorModelId / stopHookBlockCap / permissionApproval
// / fluConfigDir / fluConfigFile / fluConfigFormat（FluentUI 配置落点三段定位，见 App.cpp 注入点）。
// apiToken 明文存 ini 属用户裁决（设置页配置取代环境变量，本机 exe 同目录文件，属主可见可改）。
// 手工篡改/非法值由各消费点自带校验回退默认（既有语义，本头不做聚合校验）。
//
// 读写两条路都收敛到 SettingsCache::Reader（§9.10 问题②修复），本头只剩「路径单源 + 转发」：
//   读 = value()：缓存解析产物，按文件身份 (exists, mtime, size) 惰性重载；手改 ini 立即生效。
//   写 = setValue()：局部 QSettings 写后即 sync 落盘（原「局部对象析构即 sync」语义不变），
//        随后 invalidate()——为什么必须显式失效见 SettingsCache.h 不变量③。
// 原 `ini()`（按值返回新鲜 QSettings 供各处自取）已摘除：它让「每次调用现构造一个」看起来
// 是正当用法，而 QSettings 在 Qt6 不可拷贝（Q_DISABLE_COPY），缓存实例根本没法按值交出去——
// 保留这个逃生口等于保留问题②的复发通道。需要 QSettings 全量能力（group/导出/删键）时，
// 优先给 Reader 加方法并配测试，而不是把 ini() 加回来。
// 特别注意：**「删键」不能靠传 invalid QVariant**——IniFormat 会把它写成 `key=@Invalid()`
// 而不是删除该行（qsettings.cpp:373-377 + QSettings::setValue 无 remove 分支）。真要删键得给
// Reader 加显式 remove()。设置页的「空串=清除」用的是空串而非 invalid，读回是有效空串，
// 与「键不存在」可区分（tst_settingscache 组 4 钉住这两态）。
#include <QCoreApplication>
#include <QFileInfo> // bootstrapValue：从 argv[0] 反推 exe 目录
#include <QString>
#include <QVariant>

#include "LitePaths.h"    // 存储根单源：settings.ini 落 <exeDir>/../config/，与索引/会话数据同根
#include "SettingsCache.h" // QSettings 由本头内部使用（Reader 装载/写回），此处不再直接构造

namespace AppSettings {

// 配置文件绝对路径单源（Reader 构造期取一次，杜绝两处拼法漂移）
inline QString filePath()
{
    // 存储根单源见 LitePaths.h：settings.ini 从 exe 同目录搬到 exe 同级的 config/ 之下。
    // 改造前它钉 <exe 目录>/settings.ini、会话索引钉 <进程 CWD>/.lite-harness/index.json、
    // 会话数据钉 <workDir>/.lite-harness/sessions/<id>——三套锚点切开一个逻辑存储区。
    // 注意：config/ 在打包产物里被 CMake 的 staging 清理剥掉、由运行期 mkpath 生成，
    // 而本头的 Reader 不建目录（QSettings 写不存在的目录会静默 AccessError），
    // 故 App.cpp 必须在任何写之前先 mkpath 存储根。
    return LitePaths::settingsFilePath();
}

// 进程级唯一读写门面。函数内 static（C++17 inline 函数跨 TU 唯一实体，同 TestHarness 计数口径）；
// 路径在这里钉死，此后不再重取。
// **断言不是装饰**：无 QCoreApplication 实例时 applicationDirPath() 返回空串并 qWarning
// （qcoreapplication.cpp:2380-2386），filePath() 就退化成 "/settings.ini"——Windows 下解析成
// 「当前驱动器根」，而这条错路径会被 static 永久钉死，写侧还会在那儿落一个真文件。今天没有
// 调用点能触发（App.cpp 先建 QApplication；测试二进制里没人调 AppSettings），但 tst_worktree
// 证明测试进程里 app 实例是跑到第 4 套才出现的——将来若有套件在它之前读配置，必须当场炸，
// 而不是静默绑错路径并跨套件互相污染。
inline const SettingsCache::Reader &reader()
{
    Q_ASSERT_X(QCoreApplication::instance() != nullptr,
               "AppSettings::reader",
               "settings.ini path is derived from applicationDirPath(); call this only after a "
               "QCoreApplication instance exists");
    static const SettingsCache::Reader instance(filePath());
    return instance;
}

// 引导期取值（P1-11 唯一合法用途）：高 DPI 取整策略**必须在 QGuiApplication 构造前
// 落定**——Qt 实证构造后调用只 qWarning 且对已算好的缩放毫无作用
// （qguiapplication.cpp:4009），而 reader() 依赖 applicationDirPath()，后者要求实例
// 已存在（本头 Q_ASSERT 正是拦这条）。两条约束互斥，故开一条不依赖实例的引导通道：
// exe 目录改从 argv[0] 反推（Qt 的 applicationDirPath 亦是 exe 所在目录，同源不同路）。
// 只读、不写、不走静态缓存（构造期即弃）——**除「构造前必须落定」的键以外，
// 其余一律走 value()**，否则等于把本头的单源纪律拆成两条并行路径。
inline QVariant bootstrapValue(const char *argv0, const QString &key, const QVariant &fallback)
{
    if (argv0 == nullptr)
        return fallback;
    // 引导期同样走 LitePaths 派生，只是锚点从 applicationDirPath() 换成 argv[0] 反推的 exe 目录
    // ——两条拼法必须同源，否则「引导读 <exe>/settings.ini、运行读 <exe>/../config/settings.ini」
    // 会把同一个配置文件劈成两个文件，dpiRounding 改了不生效且无声。
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv0)).absolutePath();
    const QString root = LitePaths::configRootFor(exeDir);
    if (root.isEmpty())
        return fallback; // 反推不出 exe 目录：取缺省，绝不猜一个根去落盘
    const SettingsCache::Reader bootstrap(LitePaths::settingsFilePathFor(root));
    return bootstrap.value(key, fallback);
}

// 热读单点：命中缓存零 QSettings 构造（仍有一次 stat，见 SettingsCache.h 不变量①的代价说明），
// 文件被手改则下一次取值即重解析——README「其余手改 ini 即可」的语义由此保住，勿改成 TTL
// 或永久 static 缓存。
inline QVariant value(const QString &key, const QVariant &fallback = QVariant())
{
    return reader().value(key, fallback);
}

// 写单点：转发给 Reader，落盘与失效成对发生（SettingsCache.h 不变量③）——逻辑只有一份，
// 测试直测的就是生产跑的那一份。返回值 = 是否成功落盘；现有调用点沿用「静默失败」的既有语义
// 不检查它（与改动前一致），留着是为了让这一格将来能接上设置页的告警而不必再改签名。
inline bool setValue(const QString &key, const QVariant &value)
{
    return reader().setValue(key, value);
}

} // namespace AppSettings
