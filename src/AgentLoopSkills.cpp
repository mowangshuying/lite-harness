// 技能（lcc s07 SkillManager 语义内联）：scanSkills 重建技能表、skillsCatalog 供 system prompt、
// load_skill 返回全文。技能目录始终跨会话共享 <workDir>/.lite-harness/skills。

#include "AgentLoop.h"


#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

void AgentLoop::scanSkills()
{
    // 对应 lcc scan_skills()：整表重建；skills 目录缺失静默为空（对应 python return）
    m_skills.clear();

    // lcc env.py: workDir/"skills"；lite 有意偏差：收进 .lite-harness 中间目录，不在用户项目根撒目录
    const QString skillsDir = QDir(m_workDir).filePath(QStringLiteral(".lite-harness/skills"));
    if (!QFileInfo(skillsDir).isDir())
        return;

    // 越界防护根：解析后的清单必须仍位于 skills 目录内（lcc resolve().is_relative_to(root) 等价；
    // canonicalFilePath 已归一化，root 为空表示目录不可解析）
    const QString root = QFileInfo(skillsDir).canonicalFilePath();
    if (root.isEmpty())
        return;

    // 对应 sorted(glob("*/SKILL.md"))：按目录名升序遍历。微小偏差：python glob 跳过 "." 开头的
    // 隐藏目录，QDir::entryList 会包含——lite 有意超集，不作特判
    QStringList dirs = QDir(skillsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    dirs.sort(); // 码点升序 ≈ python sorted()（Windows 下 nt 文件系统 python 以 casefold 为键，差异极微）

    // 空白切分正则（对应 python str.split() 的连续空白切分）
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));

    for (const QString &dirName : dirs)
    {
        const QString manifestPath = QDir(skillsDir).filePath(dirName + QStringLiteral("/SKILL.md"));
        const QFileInfo manifestInfo(manifestPath);
        if (!manifestInfo.isFile()) // 对应 not is_file() → continue（QFileInfo::isFile 跟随符号链接，语义一致）
            continue;
        const QString resolved = manifestInfo.canonicalFilePath(); // 对应 resolve()
        if (resolved.isEmpty() || (resolved != root && !resolved.startsWith(root + QLatin1Char('/'))))
            continue; // 逃逸出 skills 根（符号链接指向外部等）→ 跳过

        QFile file(resolved);
        if (!file.open(QIODevice::ReadOnly))
            continue; // 偏差：lcc 单文件读失败会抛异常中止整次扫描；lite 跳过该条继续（更稳健的有意超集）
        const QString content = QString::fromUtf8(file.readAll()); // 对应 read_text(encoding="utf-8")

        // ---- frontmatter 定位（lcc parse_frontmatter 的 splitlines(keepends) 偏移量移植）----
        // 行终止符为 \n 或 \r，\r\n 视作一个；"行内容"截到首个终止符，即等价 rstrip("\r\n") 后的比较
        auto lineEnd = [&content](int from) {
            int i = from;
            while (i < content.size() && content.at(i) != QLatin1Char('\n') && content.at(i) != QLatin1Char('\r'))
                ++i;
            return i;
        };
        auto skipEol = [&content](int end) {
            if (end < content.size() && content.at(end) == QLatin1Char('\r')
                && end + 1 < content.size() && content.at(end + 1) == QLatin1Char('\n'))
                return 2; // \r\n
            return end < content.size() ? 1 : 0; // 单个 \n / \r / 文件尾
        };

        QString metaName;
        QString metaDesc;
        QString body;
        const int firstEnd = lineEnd(0);
        const bool hasFrontmatter =
            !content.isEmpty() && content.left(firstEnd) == QLatin1String("---"); // 首行 rstrip 后恰为 "---"
        int closingStart = -1; // 第二条 "---" 行起点（-1 = 不存在闭合行 → 整体视为正文）
        int closingEnd = -1;   // 其行尾（不含行终止符）
        if (hasFrontmatter)
        {
            int pos = firstEnd + skipEol(firstEnd);
            while (pos < content.size())
            {
                const int end = lineEnd(pos);
                if (content.mid(pos, end - pos) == QLatin1String("---"))
                {
                    closingStart = pos;
                    closingEnd = end;
                    break;
                }
                pos = end + skipEol(end);
            }
        }

        if (hasFrontmatter && closingStart >= 0)
        {
            const int metaStart = firstEnd + skipEol(firstEnd);
            const QString metaRegion = content.mid(metaStart, closingStart - metaStart);
            body = content.mid(closingEnd + skipEol(closingEnd)).trimmed(); // 对应 remainder.strip()

            // ---- 极简 YAML 解析（登记偏差：lite 无 PyYAML 且禁止引入第三方库）----
            // 仅识别顶格单行 `name:` / `description:` 平面标量（冒号后须有空格/制表或行尾，
            // 值可选去除外层成对引号后 trim）；其余键、缩进、多行结构一律忽略，
            // 效果 ≈ lcc yaml.safe_load 失败/非 dict 时回落 {} 走默认值的分支
            auto stripQuotes = [](QString v) {
                v = v.trimmed(); // 对应 str(...).strip()
                if (v.size() >= 2
                    && ((v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                        || (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))))
                    v = v.mid(1, v.size() - 2).trimmed(); // 偏差：不处理 YAML 转义，仅去外层成对引号
                return v;
            };
            const QStringList metaLines = metaRegion.split(QLatin1Char('\n'));
            for (QString rawLine : metaLines)
            {
                while (rawLine.endsWith(QLatin1Char('\r')))
                    rawLine.chop(1); // splitlines 语义：剥去 \r\n 残留
                QString *target = nullptr;
                int valueStart = 0;
                if (rawLine.startsWith(QLatin1String("name:")))
                {
                    target = &metaName;
                    valueStart = 5; // strlen("name:")
                }
                else if (rawLine.startsWith(QLatin1String("description:")))
                {
                    target = &metaDesc;
                    valueStart = 12; // strlen("description:")
                }
                else
                {
                    continue;
                }
                if (rawLine.size() > valueStart && rawLine.at(valueStart) != QLatin1Char(' ')
                    && rawLine.at(valueStart) != QLatin1Char('\t'))
                    continue; // "name:x" 非 YAML 平面标量映射键 → 整行忽略
                *target = stripQuotes(rawLine.mid(valueStart)); // 重复键后者覆盖前者（≈ PyYAML last-wins）
            }
        }
        else
        {
            // 首行/闭合行任一缺失 → 无 frontmatter：meta 为空、正文取原文
            // （对应 lcc return {}, text —— 此路径 lcc 不 strip，逐字保留）
            body = content;
        }

        // name 缺省 = 技能目录名（对应 manifest.parent.name）
        const QString name = metaName.isEmpty() ? dirName : metaName;
        // description 缺省 = 正文首行（对应 body.split("\n", 1)[0]；空正文 → 空串行，语义一致）
        const QString rawDesc =
            metaDesc.isEmpty() ? body.split(QLatin1Char('\n'), Qt::KeepEmptyParts).first() : metaDesc;
        // 清洗（对应 " ".join(str(desc).lstrip("# ").split())）：剥离开头的 '#'/' ' 字符，
        // 再按连续空白切分、以单个空格重连
        int lead = 0;
        while (lead < rawDesc.size() && (rawDesc.at(lead) == QLatin1Char('#') || rawDesc.at(lead) == QLatin1Char(' ')))
            ++lead;
        const QString description =
            rawDesc.mid(lead).split(wsRe, Qt::SkipEmptyParts).join(QLatin1Char(' '));

        // content 保存整份文件原文（含 frontmatter，对应 "content": text）
        // 对应 python dict 赋值语义：同名后扫覆盖值但保留原插入位置（catalog 依首次出现顺序）
        bool replaced = false;
        for (Skill &existing : m_skills)
        {
            if (existing.name == name)
            {
                existing = Skill{name, description, content};
                replaced = true;
                break;
            }
        }
        if (!replaced)
            m_skills.append(Skill{name, description, content});
    }
}

QString AgentLoop::skillsCatalog() const
{
    // 对应 lcc catalog()：空 → "(no skills found)"；否则逐行 "- {name}: {description}" 以 \n 连接
    if (m_skills.isEmpty())
        return QStringLiteral("(no skills found)");
    QStringList lines;
    lines.reserve(m_skills.size());
    for (const Skill &skill : m_skills)
        lines.append(QStringLiteral("- %1: %2").arg(skill.name, skill.description));
    return lines.join(QLatin1Char('\n'));
}

QString AgentLoop::runLoadSkill(const QJsonObject &args) const
{
    // 对应 lcc run_load_skill → skill_manager.load(name)：命中返回整份原文，未命中返回错误文本
    // 偏差：lcc 缺 name 参数直接 TypeError 崩溃；此处回落空串返回 Unknown 错误
    // （与 todo_write 缺参的 Graceful 处理同风格）
    const QString name = args.value(QStringLiteral("name")).toString();
    for (const Skill &skill : m_skills)
    {
        if (skill.name == name)
            return skill.content;
    }
    return QStringLiteral("Error: Unknown skill '%1'").arg(name);
}
