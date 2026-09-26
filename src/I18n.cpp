#include "I18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QSettings>
#include <QTranslator>
#include <QWidget>

#include <FluConfigUtils.h>

namespace {

// 语言取值（与 FluentUI getLanguage/setLanguage 的字符串约定一致）
const QString kLangZh = QStringLiteral("zh-CN");
const QString kLangEn = QStringLiteral("en-US");

// translator 生命周期单点持有：applyLanguage 每次「remove + delete 旧对象 → new +
// install 新对象」。重装是 Qt 广播 QEvent::LanguageChange 到全 widget 树的唯一途径
// （QTranslator 装载即触发；FluentUI 无语言信号可订阅），常驻组件的 changeEvent
// 重译链路依赖这一机制兜底「同进程二次调用 applyLanguage」的演进场景。
QList<QTranslator *> g_translators;

/// 装载并安装一个 .qm；资源缺失时告警并回退源文本（中文），返回是否成功
bool installTranslator(const QString &resourcePath)
{
    auto *translator = new QTranslator(qApp);
    if (!translator->load(resourcePath)) {
        qWarning().noquote() << "[i18n] translator not loaded:" << resourcePath;
        delete translator;
        return false;
    }
    qApp->installTranslator(translator);
    g_translators.append(translator);
    qDebug().noquote() << "[i18n] loaded translator:" << resourcePath;
    return true;
}

} // namespace

namespace I18n {

QString language()
{
    // 默认构造 QSettings：org/app 已在 App.cpp 全局设定（LiteHarness/LiteHarness）
    const QString stored = QSettings().value(QStringLiteral("language")).toString();
    return stored == kLangEn ? kLangEn : kLangZh; // 非法/缺省一律回退 zh-CN
}

void setLanguage(const QString &lang)
{
    QSettings().setValue(QStringLiteral("language"), lang);
    // 镜像同步 FluentUI 内部读值。其存储为 CWD 相对的 ../config/config.ini（分发场景
    // 可能漂移），故仅作一致性同步、不作权威来源；且该写入不发任何信号。
    FluConfigUtils::getUtils()->setLanguage(lang);
}

void applyLanguage(const QString &lang)
{
    // 先卸旧再装新：见 g_translators 注释（重装即 LanguageChange 广播的触发机制）
    for (auto *translator : std::as_const(g_translators)) {
        qApp->removeTranslator(translator);
        delete translator;
    }
    g_translators.clear();

    if (lang == kLangEn) {
        // 自产英文词条（qt_add_translations 内嵌，路径已在 CMake 侧核实）。
        // 阶段二填译文前 unfinished 条目回退中文源文本，属预期。
        installTranslator(QStringLiteral(":/i18n/lite-harness_en_US.qm"));
    } else {
        // zh-CN：装 Qt 官方中文 qm 使标准对话框（QFileDialog/QMessageBox 等按钮）
        // 落中文——qtbase_zh_CN.qm 已拷入库并经 qt_add_resources 内嵌；
        // Controls.zh-CN.qm 由 FluentUI 静态库自带资源提供（:/i18n/ 同路径），
        // 直接复用官方中文，无需拷贝。
        installTranslator(QStringLiteral(":/i18n/qtbase_zh_CN.qm"));
        installTranslator(QStringLiteral(":/i18n/Controls.zh-CN.qm"));
    }
}

bool requestRestart(QWidget *parent)
{
    QWidget *window = parent ? parent->window() : nullptr;
    if (!window)
        return false;

    // 复用主窗口既有退出守卫（closeEvent 询问运行中会话）：用户取消 → close 被
    // ignore() → close() 返回 false → 不重启，语言设置已存、下次正常启动生效。
    if (!window->close())
        return false;

    // gallery 惯例：931 = 语言切换自重启。新进程在此处拉起（而非 main 收口处），
    // main 对 rc==931 只透传返回值、严禁重复 startDetached。
    // arguments() 首元素是程序名，须剔除；当前程序未消费任何 CLI 参数，
    // 透传余下参数是为未来「带工作目录启动」场景留路。
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QCoreApplication::arguments().mid(1));

    // 末窗关闭已同步触发 quit()（exitCode=0 并投递 Quit 事件），但 exit() 先同步
    // 改写 exitCode、事件循环处理 Quit 时才读取，故此处 931 覆盖 0，exec() 返回 931。
    qApp->exit(931);
    return true;
}

} // namespace I18n
