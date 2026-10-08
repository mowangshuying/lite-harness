#ifndef LITE_TESTS_SCOPED_TEMP_ROOT_H
#define LITE_TESTS_SCOPED_TEMP_ROOT_H

// ============================================================================
// 测试临时根目录 RAII 包装（header-only，仅 Qt6::Core）—— s13 P1 毁仓事故的根治件。
//
// 事故登记（port-s13 fix-2）：本机安全策略按「进程镜像位置」拦截仓库内 exe 向系统
// Temp 根写目录（CreateDirectoryW gle=5 ACCESS_DENIED），旧版 ScopedTempRoot 的
// applicationDirPath() 回退分支 mkpath 失败后，析构对空路径执行
// QDir("").removeRecursively() —— QDir("") 即当前工作目录（运行时恰为仓库根），
// 整仓被永久删除。教训：任何以「路径可能为空」的字符串构造 QDir 再递归删除，
// 等价于对 cwd 做 rm -rf。
//
// 编排者钉死的契约（本类为其唯一实现，逐条遵守）：
//   · 唯一路径来源 = 环境变量 LITE_TEST_TMPROOT；未设置/为空 → 无效态，不删不建；
//     测试套件对无效态的约定是打印 SKIP 并 return 0（计数为 0 失败，不误报）。
//   · 子目录命名 lite-harness-tst-<suiteTag>-<pid>-<递增序号>，只建在 root 之下。
//   · 析构三重守卫（缺一即只告警、绝不删除）：m_path 非空、m_path 以
//     m_cleanRoot + "/lite-harness-tst-" 为前缀、m_cleanRoot 以 "/tmp-test" 结尾
//     （大小写不敏感）。守卫的意义：即便未来有人改坏构造逻辑，删除目标也
//     只可能是 <以 /tmp-test 结尾的根>/lite-harness-tst-* 形态的目录。
//
// 禁止事项清单（事故根因，永久生效）：
//   1. 永不对可能为空的路径构造 QDir（QDir("") == cwd，是本次事故的直接引爆点）；
//   2. 不使用 QCoreApplication::applicationDirPath() 或任何 exe 位置回退——
//      「主路径失败 → 双路/多路回退」设计整体废除（回退路径把删除目标指向了
//      不受测试控制的位置，正是事故形态；可移植性由「CI 干净环境恒可设
//      LITE_TEST_TMPROOT、本机跑不过就 SKIP」保障，不为个别机器策略牺牲）；
//   3. 不依赖 cwd（构造与析构均只用显式记录的绝对路径）；
//   4. 只删除本对象亲手 mkpath 创建的 lite-harness-tst-* 子目录，绝不触碰 root 本身。
//
// 用法：ctest/本机运行前把 LITE_TEST_TMPROOT 指向以 \tmp-test 结尾的目录
// （如 build\tmp-test）。CI 里由 workflow 注入；未注入时所有套件 SKIP，零删除风险。
// ============================================================================

#include <QDir>
#include <QString>

#include <cstdio>

#if defined(Q_OS_WINDOWS)
#include <process.h> // _getpid（Windows 专用；本仓仅 Windows 平台，见 AGENTS.md 构建节）
#else
#include <unistd.h> // getpid（理论可移植性保留，构建矩阵实际不含非 Windows）
#endif

class ScopedTempRoot
{
public:
    // 在 LITE_TEST_TMPROOT 下创建 <suiteTag>-<pid>-<seq> 专属子目录。
    // 环境变量缺失/为空/mkpath 失败 → isValid()==false 且 path() 返回空串。
    explicit ScopedTempRoot(const QString &suiteTag)
    {
        const QString root = qEnvironmentVariable("LITE_TEST_TMPROOT");
        if (root.isEmpty())
            return; // 未配置：保持无效态（套件方应 SKIP），不建不删
        // 归一化记录 clean 后的 root 与 m_cleanRoot：析构守卫据此比对前缀与 /tmp-test 尾缀。
        // cleanPath 同时消化结尾斜杠与 . / .. 段，令前缀判断不受书写形态影响。
        m_cleanRoot = QDir::cleanPath(root);
        if (m_cleanRoot.isEmpty() || m_cleanRoot == QStringLiteral("."))
            return; // clean 后仍空/相对当前目录（如 root 写成 ""、"."、cwd 相对串）——
                    // 这类值一旦流入 QDir(path).removeRecursively() 就是事故形态，直接判无效。
        static quint64 s_seq = 0; // 类内定义（天然 inline）的函数级 static：跨 TU 唯一实体，无需定义文件
#if defined(Q_OS_WINDOWS)
        const qint64 pid = _getpid();
#else
        const qint64 pid = getpid();
#endif
        const QString name = QStringLiteral("lite-harness-tst-%1-%2-%3")
                                 .arg(suiteTag)
                                 .arg(pid)
                                 .arg(++s_seq);
        // 刻意用字符串拼接 '/' 而非 QDir::filePath()：Windows 下 filePath 产出反斜杠
        // 分隔符（"<root>\lite-harness-tst-..."），会令析构的前缀守卫（比较
        // m_cleanRoot + "/lite-harness-tst-"）恒不成立、目录永不回收——那只是泄漏；
        // 但更重要的是把「守卫用的分隔符」与「Qt 平台默认分隔符」解绑，
        // 守卫字符串单源归本类所有，不受 Qt 内部拼接行为变化牵连。
        // QDir 对正斜杠路径在 Windows 上同样可正常 mkpath/removeRecursively。
        m_path = m_cleanRoot + QLatin1Char('/') + name;
        if (!QDir().mkpath(m_path))
        {
            m_path.clear(); // mkpath 失败：回落到无效态，析构无操作
            return;
        }
        m_valid = true;
    }

    ~ScopedTempRoot()
    {
        if (!m_valid)
            return; // 无效态析构什么都不做（连字符串都不构造 QDir，杜绝事故路径）
        // 三重守卫：删除目标必须形如 <以 /tmp-test 结尾的根>/lite-harness-tst-*
        const bool pathShapeOk = !m_path.isEmpty()
            && m_path.startsWith(m_cleanRoot + QStringLiteral("/lite-harness-tst-"));
        const bool rootShapeOk = m_cleanRoot.endsWith(QStringLiteral("/tmp-test"), Qt::CaseInsensitive);
        if (!pathShapeOk || !rootShapeOk)
        {
            // 守卫不满足 = 本对象状态被改坏或环境异常：宁可泄漏一个目录，绝不删除任何
            // 不明路径。泄漏可人工清理，误删不可逆（毁仓事故的教训即「失败时少动作为妙」）。
            std::fprintf(stderr,
                         "ScopedTempRoot LEAK WARNING: guard rejected delete for '%s' "
                         "(root='%s'); directory left behind, NOT deleted\n",
                         m_path.toUtf8().constData(), m_cleanRoot.toUtf8().constData());
            return;
        }
        QDir dir(m_path); // 此处 m_path 已过守卫：非空 + 绝对 + 前缀形态正确
        if (!dir.removeRecursively())
        {
            std::fprintf(stderr,
                         "ScopedTempRoot CLEANUP WARNING: partial failure removing '%s' "
                         "(locked files left behind)\n",
                         m_path.toUtf8().constData());
        }
    }

    ScopedTempRoot(const ScopedTempRoot &) = delete;
    ScopedTempRoot &operator=(const ScopedTempRoot &) = delete;
    ScopedTempRoot(ScopedTempRoot &&) = delete;
    ScopedTempRoot &operator=(ScopedTempRoot &&) = delete;

    bool isValid() const { return m_valid; }
    QString path() const { return m_valid ? m_path : QString(); }

private:
    QString m_cleanRoot; // cleanPath 后的 LITE_TEST_TMPROOT（守卫比对基准）
    QString m_path;      // 本对象专属子目录绝对路径（无效态为空串）
    bool m_valid = false;
};

#endif // LITE_TESTS_SCOPED_TEMP_ROOT_H
