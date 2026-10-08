// 沙箱文件工具（本地 IO，同步执行）：safePathIn 逃逸判定 + read_file / write_file / edit_file / glob。
// 全部为静态实现，宿主与子代理各传各的 workDir（见 AgentLoop::baseFileToolHandlers）。

#include "AgentLoop.h"

#include "AgentConstants.h"
#include "BashRunner.h"
#include "LineEnding.h" // 行尾归一单源：edit_file 两级匹配 / write_file 覆盖保真（read_file 交还 LF 文本）

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>

namespace {
// glob 模式转正则（供 runGlob 使用）：
// "**" 匹配任意层级路径；"**/" 允许零层或多层目录；"*" 匹配单层内任意字符（不跨 '/'）；
// "?" 匹配单个非 '/' 字符；其余字符按字面量转义
QRegularExpression globToRegex(const QString &pattern)
{
    QString rx;
    const int size = pattern.size();
    for (int i = 0; i < size; ++i)
    {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('*'))
        {
            if (i + 1 < size && pattern.at(i + 1) == QLatin1Char('*'))
            {
                if (i + 2 < size && pattern.at(i + 2) == QLatin1Char('/'))
                {
                    rx += QStringLiteral("(?:.*/)?"); // "**/" → 零或多层目录
                    i += 2;
                }
                else
                {
                    rx += QStringLiteral(".*"); // 末尾 "**" → 任意剩余（含 '/'）
                    ++i;
                }
            }
            else
            {
                rx += QStringLiteral("[^/]*"); // "*" → 单层通配
            }
        }
        else if (c == QLatin1Char('?'))
        {
            rx += QStringLiteral("[^/]");
        }
        else
        {
            rx += QRegularExpression::escape(QString(c));
        }
    }

    QRegularExpression re(QRegularExpression::anchoredPattern(rx));
    re.setPatternOptions(QRegularExpression::CaseInsensitiveOption); // Windows 文件系统大小写不敏感
    return re;
}
} // namespace

QString AgentLoop::safePathIn(const QString &workDir, const QString &p, QString *error)
{
    // 相对路径按工作区解析，绝对路径直接使用；cleanPath 归一化 "../" 与分隔符（Windows 反斜杠转正斜杠）
    const QString joined = QDir::isAbsolutePath(p)
        ? QDir::cleanPath(p)
        : QDir::cleanPath(workDir + QLatin1Char('/') + p);

    // 存在部分尽量取 canonical path（消解符号链接/大小写真实形态）；
    // 文件尚不存在时（write 场景）规范化已存在的父目录后接原文件名
    const QFileInfo info(joined);
    QString absPath;
    if (info.exists())
    {
        const QString canonical = info.canonicalFilePath();
        absPath = canonical.isEmpty() ? joined : canonical;
    }
    else
    {
        const QString canonicalParent = QFileInfo(info.dir().absolutePath()).canonicalFilePath();
        absPath = canonicalParent.isEmpty()
            ? joined
            : QDir::cleanPath(canonicalParent + QLatin1Char('/') + info.fileName());
    }

    // 前缀比较带目录分隔符边界（防 "D:/work" 误判 "D:/work-evil"），Windows 下大小写不敏感
    const QString root = QDir::cleanPath(workDir);
    if (absPath.compare(root, Qt::CaseInsensitive) != 0
        && !absPath.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
    {
        if (error)
            *error = QStringLiteral("Error: Path escapes workspace: %1").arg(p);
        return QString();
    }
    return absPath;
}

QString AgentLoop::runReadFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
    if (abs.isEmpty())
        return err;

    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error:%1").arg(file.errorString());

    // 读前字节预检（挂起审计防御加固）：原实现同步 readAll，LLM 指到数百 MB 文件时
    // 主线程冻结秒级且截断发生在读入之后。零线程纪律下改限量读：超限只读开头
    // kReadFileMaxBytes 字节，返回尾部附中文截断说明；行区间 limit 逻辑保持原行为
    // （对已读入的头部内容照常生效）。
    const bool sizeTruncated = QFileInfo(abs).size() > AgentConst::kReadFileMaxBytes;
    const QByteArray raw = sizeTruncated ? file.read(AgentConst::kReadFileMaxBytes)
                                         : file.readAll();

    // UTF-8 按行读取；QTextStream 行为对齐 Python splitlines（末尾换行不产生空行）
    QString text = QString::fromUtf8(raw); // 非 const：QTextStream 需要 QString*
    QStringList lines;
    QTextStream ts(&text);
    for (QString line = ts.readLine(); !line.isNull(); line = ts.readLine())
        lines.append(line);

    const int limit = args.value(QStringLiteral("limit")).toInt();
    if (limit > 0 && limit < lines.size())
    {
        const int more = lines.size() - limit;
        lines = lines.mid(0, limit);
        lines.append(QStringLiteral("... (%1 more lines)").arg(more));
    }

    // 截断与空输出兜底与 bash 同口径（lcc [:50000] + 空则默认文案），单源于 BashRunner
    QString result = BashRunner::truncateOutput(lines.join(QLatin1Char('\n')));
    if (sizeTruncated)
    {
        // 字节级限量读说明后置追加：若先拼进正文可能被 50000 字符截断吃掉，警告必须可见
        result += QStringLiteral("\n[警告：文件超过 %1 字节，仅读取开头部分]")
                      .arg(AgentConst::kReadFileMaxBytes);
    }
    return result;
}

QString AgentLoop::runWriteFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    const QString content = args.value(QStringLiteral("content")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
    if (abs.isEmpty())
        return err;

    // 自动创建父目录（对齐 mkdir(parents=True)）
    if (!QDir().mkpath(QFileInfo(abs).dir().absolutePath()))
        return QStringLiteral("Error:cannot create directory:%1").arg(QFileInfo(abs).dir().absolutePath());

    // 行尾保真：覆盖已存在文件时沿用其主导行尾（模型给的 content 天然是 LF，直写会把整个
    // CRLF 文件翻转成 LF，产出全文件 diff 噪声）；新建文件按 content 原样落盘，不臆造行尾。
    // 只读开头 kEndingProbeBytes 字节做统计样本，不为探测行尾把大文件整体读进内存；
    // 已知边界：开头窗口内一个换行都没有（超长单行文件）时判为 LF，属可接受偏差。
    QString rendered = content;
    if (QFile::exists(abs))
    {
        QFile probe(abs);
        if (probe.open(QIODevice::ReadOnly))
        {
            const QString head = QString::fromUtf8(probe.read(AgentConst::kEndingProbeBytes));
            probe.close();
            rendered = LineEnding::apply(content, LineEnding::dominant(head));
        }
    }

    // 原子写防止中途失败毁目标文件（旧 QFile Truncate 在磁盘满/崩溃时留下半截内容），
    // 对齐同文件 persistHistory 的 QSaveFile 纪律；失败时 cancelWriting 丢弃临时文件不伤目标
    QSaveFile file(abs);
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("Error: %1").arg(file.errorString()); // lcc run_write 带空格（G3）
    const QByteArray bytes = rendered.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        const QString reason = file.errorString();
        file.cancelWriting(); // 丢弃临时文件并关闭句柄，目标路径不受影响
        return QStringLiteral("Error: %1").arg(reason);
    }
    return QStringLiteral("Wrote %1 bytes to %2").arg(bytes.size()).arg(path);
}

QString AgentLoop::runEditFileIn(const QString &workDir, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString();
    // lcc s10 破坏性改名跟随（tools_manager.py EDIT_FILE schema）：old_text/new_text → old_string/new_string，
    // schema properties、required 与本处读键三处同步；handler 行为文案不变
    const QString oldString = args.value(QStringLiteral("old_string")).toString();
    const QString newString = args.value(QStringLiteral("new_string")).toString();
    QString err;
    const QString abs = safePathIn(workDir, path, &err);
    if (abs.isEmpty())
        return err;

    // 空 old_string 拒绝：indexOf("") 恒返回 0，原语义会把 new_string 静默前插到文件开头
    // （模型漏填参数即毁文件头部），属明显误用，先行短路不读盘
    if (oldString.isEmpty())
        return QStringLiteral("Error: old_string 为空，拒绝编辑（会在 %1 开头静默插入 new_string）").arg(path);

    // 体量防线（对齐 read_file 的 kReadFileMaxBytes 纪律）：整文件读入 + 多次整串拷贝在
    // 零线程下会冻结主线程，超限直接拒绝，不尝试部分读写
    const qint64 fileSize = QFileInfo(abs).size();
    if (fileSize > AgentConst::kEditFileMaxBytes)
        return QStringLiteral("Error: %1 超过 edit_file 体量上限（%2 > %3 字节），拒绝编辑")
            .arg(path)
            .arg(fileSize)
            .arg(AgentConst::kEditFileMaxBytes);

    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Error:%1").arg(file.errorString());
    const QByteArray raw = file.readAll();

    // BOM 显式摘出（实测：QString::fromUtf8 会吃掉前导 UTF-8 BOM，解码结果首字符即正文）。
    // 不摘出会有两个后果：① 编码守卫拿含 BOM 的 raw 与已去 BOM 的 toUtf8() 比较恒不等，
    // 带 BOM 的合法源文件被误判为「非 UTF-8」而拒绝编辑；② 写回时 BOM 被静默删除——对以
    // BOM 标注编码的源文件（中文 Windows / MSVC 常见）属越界修改。故此处摘出、写回时原样补回，
    // 守卫按去 BOM 后的正文字节比较。
    static const QByteArray kUtf8Bom = QByteArray("\xef\xbb\xbf", 3);
    const bool hadBom = raw.startsWith(kUtf8Bom);
    const QByteArray body = hadBom ? raw.mid(kUtf8Bom.size()) : raw;
    const QString text = QString::fromUtf8(body);

    // 编码防线：fromUtf8 把非法字节解码成 U+FFFD，写回即永久损坏原文件且全程无告警
    // （中文 Windows 上的 GBK 源文件属现实场景）。往返字节不等价 → 判定非 UTF-8，拒绝编辑
    // 并回可判定错误（B1 约定），不静默毁文件。
    if (text.toUtf8() != body)
    {
        file.close();
        return QStringLiteral("Error: %1 不是合法 UTF-8，拒绝编辑（写回会把非法字节永久替换为 U+FFFD）")
            .arg(path);
    }

    // 只替换第一处（对齐 str.replace(old, new, 1)）；行尾两级匹配单源于 LineEnding.h——
    // read_file 交还模型的是 LF 归一化文本，直接拿原文匹配会让多行 old_string 在 CRLF
    // 文件上必然失配，而 LF new_string 原样插入又会混入裸 LF
    bool matched = false;
    int matchCount = 0;
    const QString edited = LineEnding::replaceOnce(text, oldString, newString, &matched, &matchCount);
    if (!matched)
    {
        file.close();
        return QStringLiteral("Error: text not found in %1").arg(path);
    }
    // 歧义拒绝（**有意偏离** lcc str.replace(old, new, 1) 的静默替换第一处）：命中多处时
    // 改哪一处取决于文件里恰好先出现哪个，模型无从判断，静默改错位置比失败更危险
    // （曾按 lcc 语义替换第一处）。回可判定错误并要求补上下文使其唯一，交模型自行重试。
    if (matchCount > 1)
    {
        file.close();
        return QStringLiteral("Error: old_string 在 %1 中命中 %2 处，拒绝编辑（请补充上下文使其唯一）")
            .arg(path)
            .arg(matchCount);
    }

    // 保持"读全文→替换→写回"语义，仅写回改原子（QSaveFile）：防止写回中途失败毁原文件；
    // 先 close 读句柄，避免 Windows 下 commit 的重命名被自身打开句柄阻塞
    file.close();
    // BOM 原样补回（见读入处注释）：编辑只应改动匹配区间，不得顺带删掉文件的编码标记
    const QByteArray bytes = hadBom ? (kUtf8Bom + edited.toUtf8()) : edited.toUtf8();
    QSaveFile out(abs);
    if (!out.open(QIODevice::WriteOnly))
        return QStringLiteral("Error:%1").arg(out.errorString());
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        const QString reason = out.errorString();
        out.cancelWriting(); // 丢弃临时文件，不伤目标
        return QStringLiteral("Error:%1").arg(reason);
    }
    return QStringLiteral("Edited %1").arg(path);
}

QString AgentLoop::runGlobIn(const QString &workDir, const QJsonObject &args)
{
    // 统一分隔符风格（模型可能给出反斜杠模式）
    QString pattern = args.value(QStringLiteral("pattern")).toString();
    pattern.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QRegularExpression re = globToRegex(pattern);
    if (!re.isValid())
        return QStringLiteral("Error:%1").arg(re.errorString());

    // 以工作区为根递归遍历，按相对路径匹配；结果过滤 safePathIn 逃逸项（如符号链接指向外部）。
    // 有界化改造（GUI 线程同步遍历，全仓零线程约定不改线程模型）：原 QDirIterator(Subdirectories)
    // 无条目上限且会进入 .git/build 等巨型目录，大仓库直接冻结 UI。现按显式目录队列 BFS：
    // 剪枝 kGlobPruneDirNames 命中的整目录、遍历条目与收集命中各设硬上限，超限即停止并附提示。
    // 取舍登记：上限触发时收集集是"遍历序前 N 条命中"，最终展示（排序+前 200）不再保证是
    // 全集中字典序前 200；未触限时集合与结果序和原实现完全一致（BFS/DFS 序差被末尾 sort 抹平）。
    QStringList collected;
    QSet<QString> seen;
    qsizetype scanned = 0; // 已遍历条目数（目录+文件合计，与原 iterator 逐项产出粒度一致）
    bool truncated = false;

    QStringList dirQueue; // 显式待遍历目录队列（头指针消费、尾部追加子目录——BFS）
    dirQueue.append(workDir);
    for (qsizetype head = 0; head < dirQueue.size() && !truncated; ++head)
    {
        const QDir dir(dirQueue.at(head));
        // AllEntries|NoDotAndDotDot：目录+文件同表返回，含隐藏项——与原 QDirIterator 纳入口径一致
        const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo &info : entries)
        {
            if (++scanned > AgentConst::kGlobScanEntryLimit)
            {
                truncated = true;
                break;
            }
            if (info.isDir())
            {
                // 剪枝：VCS/构建/依赖/缓存目录整棵跳过（Windows 文件系统大小写不敏感，比较同样不敏感）
                if (!AgentConst::kGlobPruneDirNames.contains(info.fileName(), Qt::CaseInsensitive))
                    dirQueue.append(info.absoluteFilePath());
                continue;
            }
            const QString rel = QDir(workDir).relativeFilePath(info.absoluteFilePath());
            if (!re.match(rel).hasMatch())
                continue;
            QString err;
            if (safePathIn(workDir, rel, &err).isEmpty())
                continue;
            if (!seen.contains(rel))
            {
                seen.insert(rel);
                collected.append(rel);
                if (collected.size() >= AgentConst::kGlobCollectLimit)
                {
                    truncated = true;
                    break;
                }
            }
        }
    }

    QStringList shown;
    if (collected.isEmpty())
    {
        shown.append(QStringLiteral("(no matches)"));
    }
    else
    {
        collected.sort();
        shown = collected.mid(0, AgentConst::kGlobDisplayLimit); // 输出前 kGlobDisplayLimit 条
        if (collected.size() > AgentConst::kGlobDisplayLimit)
            shown.append(QStringLiteral("...(more matches omitted; narrow the pattern)"));
    }
    if (truncated)
        shown.append(QStringLiteral("...(truncated: scan limits reached; narrow the pattern)"));
    return shown.join(QLatin1Char('\n'));
}
