// 项目指令文件（AGENTS.md）读盘侧：两级候选（工程根 → 存储根）命中即止、按 mtime+size
// 惰性缓存、交纯头 ProjectInstructions.h 出段文本，供注入块第 0 段消费。
// 判据（顺序/命中即止/判空/截断/签名）全在纯头（可 ctest 直测），本 TU 只做 IO 与接线。

#include "AgentLoop.h"

#include "LitePaths.h"        // 候选路径派生单源：agentsFileFor / configRoot
#include "ProjectInstructions.h" // 判据单源（纯头，QtCore-only）
#include "AgentConstants.h"   // 字符预算单点取值 kProjectInstructionsCharBudget
#include "LiteLog.h"

#include <QFile>
#include <QFileInfo>

QString AgentLoop::projectInstructions() const
{
    // 候选顺序单源在纯头：工程根在前、存储根在后，互斥回退（不是叠加）。
    // configRoot 走 LitePaths 生产口——本函数只在会话回合里被调（run() 召回续延），
    // 此刻 QCoreApplication 实例必在，不会踩该口「无实例即炸」的引导期约束。
    const QStringList candidates =
        ProjectInstructions::candidatesFor(m_workDir, LitePaths::configRoot());

    const QString path = ProjectInstructions::selectExisting(
        candidates, [](const QString &candidate) {
            const QFileInfo info(candidate);
            return info.isFile() && info.isReadable();
        });
    if (path.isEmpty()) {
        // 两级都不存在：清缓存并返回空串 → 注入块不产生第 0 段，产物与改造前逐字节一致
        // （「零 AGENTS.md 会话零 overhead」纪律，同「零技能+零记忆不产生注入块」）。
        m_instructionsSignature.clear();
        m_instructionsSection.clear();
        return QString();
    }

    // 签名 = 路径 + mtime(ms) + 字节数（单源纯头拼接）。会话中途编辑 AGENTS.md 不需要
    // 显式失效钩子：下一回合签名不符即重读。路径也在签名里，换工作目录/换命中层级
    // 同样自动失效——刻意的超集，只靠 mtime 会漏掉「同秒改完且改回原大小」这一类。
    const QFileInfo info(path);
    const QString signature = ProjectInstructions::signatureFor(
        path, info.lastModified().toMSecsSinceEpoch(), info.size());
    if (signature == m_instructionsSignature)
        return m_instructionsSection;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 读失败（占用/权限）：返回空串但**不落签名缓存**，下一回合重试。
        // 落缓存就等于把一次瞬时失败钉到文件被改动为止——静默失效比多读一次贵得多。
        // 偏差登记：skills 侧单文件读失败是静默跳过，本处留 LOG_WARN 痕迹——AGENTS.md
        // 是用户显式放在项目根的指令文件，读不到属可观测降级，不该无声。
        LOG_WARN << "[instructions] cannot open " << path;
        return QString();
    }
    const QString text = QString::fromUtf8(file.readAll());

    // 命中来源留痕：两级回退是「静默择一」，用户不看日志就无从知道到底加载了哪一份
    // （项目根那份存在时全局那份被完全忽略，这条不对称必须有观测面）。
    LOG_INFO << "[instructions] loaded " << path << " (" << text.size() << " chars, budget "
             << AgentConst::kProjectInstructionsCharBudget << ")";

    // 空正文同样落签名缓存：命中文件为空是稳定状态（不是瞬时失败），不该每回合重读。
    m_instructionsSection = ProjectInstructions::formatSection(
        path, text, AgentConst::kProjectInstructionsCharBudget);
    m_instructionsSignature = signature;
    return m_instructionsSection;
}
