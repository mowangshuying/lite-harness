#include "WorktreeManager.h"

#include "AgentConstants.h" // 隔离目录名单源（kWorktreesDirName）
#include "AgentPathGuard.h" // isWithinPath 共享包含守护（gate① §3-6；名字正则**不**共享——M7）

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

// ============================================================================
// lcc s13 34775c8 worktree_manager.py 移植（Lane B）。git worktree 隔离：一任务一
// 独立 checkout，队友并行不互踩。纯同步内核（零线程、无信号槽、QtCore-only，
// QProcess::waitForFinished 同步等待、禁嵌事件循环——waitForFinished 不泵应用事件
// 循环，同步 createWorktree 路径内无可重入窗口，串行由「主线程同步执行」天然保证）。
// D8 异步壳归 P3；Gate③ MINOR-4 前半已落地（P4 Lane A 第7条）：同名并发拒止
// in-flight QSet 以防御性形态进内核（同步路径必空、Lane B 异步壳启用即承重）；
// 异步形态本身（MINOR-4 剩余）仍归 Lane B。
// 铁律：git 失败**绝不**自动清理（任何回滚冲动 = 二次销毁风险，repo-wipe 同族），
// remove 成功后分支也永不删除。
// ============================================================================

namespace {

// Gate③ 第7条：createWorktree 在途名字表的 RAII 出口——十连门多 early return，
// 逐点手写 remove 易漏，析构统一清（键保持入口 insert 时的原文形态）。
struct CreateInFlightGuard
{
    QSet<QString> *set;
    QString name;
    ~CreateInFlightGuard()
    {
        if (set)
            set->remove(name);
    }
};

// lcc _run_git timeout=30（秒）；lite 毫秒口径同值（fix-3 专属⑥ parity）
constexpr int kGitTimeoutMs = 30000;

// lcc run_git :91-92 对外截断上限（字符）
constexpr int kGitReportMaxChars = 5000;

// python repr() 的轻形近拟：单引号包裹，含单引号时改双引号（lcc :65 {name!r} 文案锚）。
// 不处理反斜杠/转义族——name 已过正则，必无引号外的 repr 特殊字符。
QString pyReprLite(const QString &text)
{
    if (text.contains(QLatin1Char('\'')))
        return QLatin1Char('"') + text + QLatin1Char('"');
    return QLatin1Char('\'') + text + QLatin1Char('\'');
}

// ---- FIND-C（Gate②）比较口径单源 ----------------------------------------------------
// 裁决：worktree **路径/名字**比较一律大小写不敏感（Windows 文件系统语义——同一目录
// 可有 d:/ 与 D:/ 两种拼法，git porcelain 输出与本地 sink 拼法漂移是常态，敏感比较=假
// miss）。键**原样存储不折叠**（parseRegistry 的 QMap 键、byName 键均为 cleanPath 原文，
// 大小写折叠只发生在比较点，防口径混用与「键已归一」的注释失真——原注释的失实即
// FIND-C 根因）。lcc 为 python 大小写敏感 + resolve() 规范键，lite 登记偏差：
// 词法键 + 比较点 CI（QFileSystem 语义优先于 lcc 字面 parity）。
// 敏感比较的豁免族（逐条钉死）：任务 id / owner 是令牌非路径；branch/ref 名是 git
// 引用语义（lcc _registered_entry :157 敏感比较，parity 保留）。

// 路径/名字段的 CI 等值比较（单源）
bool eqCi(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// isWithinPath 的 CI 版：共享头 AgentPathGuard 不动（越界写域禁改），消费侧 toLower
// 折叠后走同一词法判定 = 等价的大小写不敏感包含。等值返回 true 的既有语义不变，
// 「path == root 也拒」仍由调用点 eqCi 显式把守（worktreePath 门③）。
bool withinCi(const QString &child, const QString &parent)
{
    return AgentPathGuard::isWithinPath(child.toLower(), parent.toLower());
}

// 注册表/名单映射的 CI 查找（线性扫；规模=单仓 worktree 数，开销可忽略）。
// 命中返回条目指针（指向 map 内部，const 引用生命周期内有效），未命中 nullptr。
const WorktreeManager::Entry *findCi(const QMap<QString, WorktreeManager::Entry> &entries,
                                     const QString &key)
{
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (eqCi(it.key(), key))
            return &it.value();
    }
    return nullptr;
}

} // namespace

WorktreeManager::WorktreeManager(TaskStore *store,
                                 std::function<QString()> sessionRootSink,
                                 std::function<QString()> workDirSink)
    : m_store(store),
      m_sessionRootSink(std::move(sessionRootSink)),
      m_workDirSink(std::move(workDirSink))
{
}

// ---------------------------------------------------------------------------
// 名字与路径（lcc validate_worktree_name / _worktree_path）
// ---------------------------------------------------------------------------

bool WorktreeManager::isValidWorktreeName(const QString &name)
{
    // 独立单源（gate① M7/§3-5）：**禁**与 AgentPathGuard::isValidAgentName 共享——
    // 字符集不同（此处允许 '.'，首字符必须字母数字，负向先行断言禁内嵌 ".."）。
    // 合一偏差登记：lcc 是两层防线（fullmatch ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ 后
    // 再独查 '.'/'..'/含'..'），互不信任；本正则把第二层折进先行断言，"." 被首字符
    // 规则拒绝、"a..b" 被先行断言拒绝，判定结果逐例等价。python fullmatch 天然整串
    // 锚定，C++ 等价物 = capturedStart(0)==0 && capturedLength(0)==size 惯用法
    // （防 PCRE $ 的尾换行怪癖，TaskStore::isTaskIdFull 同款）。
    static const QRegularExpression re(QStringLiteral("^(?!.*\\.\\.)[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    const QRegularExpressionMatch m = re.match(name);
    return m.hasMatch() && m.capturedStart(0) == 0 && m.capturedLength(0) == name.size();
}

QString WorktreeManager::branchForWorktree(const QString &name)
{
    // lcc _worktree_branch :68-69 单源。remove 后分支永不删（fix-3 专属②）。
    return QStringLiteral("wt/") + name;
}

QString WorktreeManager::worktreesRootDir() const
{
    // lcc env.py worktreesDirPath = workspace/.lcc/worktrees；lite 收敛进会话根下
    // .worktrees 叶子（D2 登记偏差，同 .mailboxes 先例），单源常量防拼法漂移。
    return QDir(m_sessionRootSink()).filePath(AgentConst::kWorktreesDirName);
}

bool WorktreeManager::worktreePath(const QString &name, QString *path, QString *error) const
{
    // 三关 fail-closed（照 lcc _worktree_path :47-65，宁报错不猜）。比较口径 FIND-C：
    // 路径比较一律 CI（eqCi/withinCi 单源，Windows 语义）；m1（Gate① minor + Gate②
    // 前置③）已落地——**目标已存在**时经 isWithinPathCanonical 复校，防 junction/符号
    // 链接把 .worktrees（或其子项）重定向出会话根；**尚未创建**时保持词法
    // （canonicalFilePath 对不存在路径返回空串，无可解析对象——与 send 首帧目录同理；
    // lcc :59/:62 的 Path.resolve(strict=False) 能解析不存在路径，lite Qt 口径做不到，
    // 登记偏差：不存在段留词法、存在段 canonical 复校，已存在目标的 resolve parity 恢复）。
    // ⓪ 名字校验（lcc create/remove 首行 validate_worktree_name，折叠文案 :150-152 锚）。
    //   偏差登记：lcc 两层校验的两种 ValueError 文案，lite 合一正则后统一回第一条
    //   （".." 类失配在 lcc 走第二条文案——文本差异，判定等价，测试按此登记断言）。
    if (!isValidWorktreeName(name)) {
        *error = QStringLiteral(
            "worktree name must be 1-64 letters, digits, dots, underscores, or dashes, "
            "and start with a letter or digit");
        return false;
    }
    // ① 隔离根必须在工作目录内（lcc :52-55）。sink 空串按非法状态 fail-closed——
    //   python 侧空段会抛错，lite 惰性 sink 返回空 = 宿主未装配，同样拒（登记偏差）。
    //   withinCi：两侧均来自宿主字符串，盘符/拼写大小写差是同类越狱判定的假阴性来源，
    //   口径统一走 FIND-C 单源。
    const QString cleanWorkDir = m_workDirSink ? QDir::cleanPath(m_workDirSink()) : QString();
    const QString cleanRoot = m_sessionRootSink().isEmpty()
                                  ? QString()
                                  : QDir::cleanPath(worktreesRootDir());
    if (cleanWorkDir.isEmpty() || cleanRoot.isEmpty() ||
        !withinCi(cleanRoot, cleanWorkDir)) {
        *error = QStringLiteral("Worktrees root escapes the working directory");
        return false;
    }
    // ② 目录路径必须仍在隔离根内（防前缀合法后缀越狱；名字已禁 '/' 与 ".."，此为纵深防御）
    //   （lexical 由 cleanRoot 直接拼接派生，本无关大小写错配，仍走 withinCi 归一口径防漂移）
    const QString lexical = QDir::cleanPath(cleanRoot + QLatin1Char('/') + name);
    if (!withinCi(lexical, cleanRoot)) {
        *error = QStringLiteral("Worktree path escapes directory: %1").arg(pyReprLite(name));
        return false;
    }
    // ③ lcc :58-65 易漏关：**path == root 本身也拒**（隔离根永远不得当一个 worktree）。
    //   AgentPathGuard::isWithinPath 等值返回 true，故在调用点另判 !=（头注释同款提醒）。
    if (eqCi(lexical, cleanRoot)) {
        *error = QStringLiteral("Worktree path escapes directory: %1").arg(pyReprLite(name));
        return false;
    }
    // m1 复校（仅目标已存在时）：把 child 以 canonicalFilePath（OS 拼写）喂给 helper，
    // 与 helper 内部 canonicalParent（同为 OS 拼写）同源对齐，杜绝盘符大小写假拒；
    // 断链解析为空串 → isWithinPath 判失败 → fail-closed 拒。lcc 本处即 resolve 后比较
    // （:59/:62），此路恢复其 parity。
    const QFileInfo lexicalInfo(lexical);
    if (lexicalInfo.exists() &&
        !AgentPathGuard::isWithinPathCanonical(lexicalInfo.canonicalFilePath(), cleanRoot)) {
        *error = QStringLiteral("Worktree path escapes directory: %1").arg(pyReprLite(name));
        return false;
    }
    *path = lexical;
    return true;
}

// ---------------------------------------------------------------------------
// git 唯一出口（lcc _run_git / run_git parity，fix-3 专属⑥）
// ---------------------------------------------------------------------------

bool WorktreeManager::runGit(const QStringList &args, QString *output, const QString &cwd) const
{
    // lcc :68-92：列表参数无 shell；cwd 缺省 = workDirPath（lite：workDirSink，空则走
    // QProcess 继承 cwd——lcc 同构，但调用点先经路径门 fail-closed，空 workDir 实际不可达）。
    // text=True errors=replace（解码替换不炸）；timeout=30s。
    // 偏差登记：lcc 的 OSError/TimeoutExpired 折叠为 'OSError:<msg>'/'TimeoutExpired:<msg>'
    // 异常串；lite 无异常域，折叠为本类错误串风格（MessageBus 同口径），语义等价 = (false, 折叠串)。
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels); // stdout+stderr 并流（lcc :80 合流口径）
    const QString startDir = cwd.isEmpty()
                                 ? (m_workDirSink ? m_workDirSink() : QString())
                                 : cwd;
    if (!startDir.isEmpty())
        proc.setWorkingDirectory(startDir);
    proc.start(QStringLiteral("git"), args);
    if (!proc.waitForStarted(kGitTimeoutMs)) {
        *output = QStringLiteral("WorktreeManager: failed to start \"git\" (%1)")
                      .arg(proc.errorString());
        return false;
    }
    if (!proc.waitForFinished(kGitTimeoutMs)) {
        // 超时 → kill 计失败（lcc TimeoutExpired 分支；kill 后回收防孤儿进程持锁）
        proc.kill();
        proc.waitForFinished(5000);
        *output = QStringLiteral("WorktreeManager: git timed out after %1 ms").arg(kGitTimeoutMs);
        return false;
    }
    // errors="replace" 的 lite 等价：fromUtf8 对非法字节替换 U+FFFD，不炸流
    QString text = QString::fromUtf8(proc.readAll()).trimmed();
    const bool ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
    // 空输出哨兵 "(no output)" **逐字**——remove 第五门的「干净」判定依赖该串
    if (text.isEmpty())
        text = QStringLiteral("(no output)");
    *output = text;
    return ok;
}

QString WorktreeManager::truncateForReport(const QString &output)
{
    // lcc run_git :91-92 的对外截断形。偏差登记：python 静默切片 output[:5000]；
    // lite 有意追加 "...[truncated]" 标记——账目里被裁掉的输出不留痕，模型无从知道信息缺失。
    if (output.size() <= kGitReportMaxChars)
        return output;
    return output.left(kGitReportMaxChars) + QStringLiteral("...[truncated]");
}

// ---------------------------------------------------------------------------
// 注册表（lcc _parse_registry / registered_worktrees / _registered_entry）
// ---------------------------------------------------------------------------

QMap<QString, WorktreeManager::Entry> WorktreeManager::parseRegistry(QString *registryError) const
{
    // lcc :95-116。失败 = git 非零/起不来/超时 → 空表 + *registryError 折叠串，
    // **绝不**把「读不到」当「没有 worktree」（fix-3 专属① fail-closed）。
    QMap<QString, Entry> entries;
    QString output;
    const bool ok = runGit({QStringLiteral("worktree"), QStringLiteral("list"),
                            QStringLiteral("--porcelain")},
                           &output);
    if (!ok) {
        if (registryError)
            *registryError = QStringLiteral("cannot read Git worktree registry: ") + output;
        return entries;
    }

    // porcelain 文法：'worktree <路径原文>' / 'HEAD <hash>' / 'branch refs/heads/<全名>' /
    // 空行分隔。逐行 partition **首个**空格——路径含空格安全（lcc line.partition(' ') 同款）。
    // 空行 flush + 循环尾补 flush：等价 lcc splitlines()+[''] 哨兵形——记录结尾有无空行
    // 两种终止形都覆盖（trimmed 后 git 输出通常以单 \n 结尾，split 不产生尾空元素，靠尾 flush 收账）。
    Entry current;
    bool hasFields = false;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        // strip 行尾（CRLF 文件里 '\r' 残留会污染 partition 值——lcc text 模式天然不吃 \r，
        // lite 并流原始字节须自净；行首不动，porcelain 无前导缩进语义）
        QString line = lines.at(i);
        while (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.isEmpty()) {
            if (hasFields && !current.worktreePath.isEmpty()) {
                // 键 = cleanPath 归一（git Windows 输出正斜杠、本地拼法反斜杠，两侧归一后
                // 可比）。FIND-C（Gate②）纠注释失实：**键保留原文大小写、不折叠**——QMap
                // 键比较本来就是大小写敏感的，原注释宣称「大小写不敏感」并未生效。
                // Windows 不敏感语义统一落在**比较点**：eqCi/withinCi/findCi（见文件顶部
                // FIND-C 单源），全链路（②过滤/查找/⑩与 recheck 命中/解绑扫描）一律经其归一。
                // 登记偏差：lcc 键=Path.resolve() 规范形 + 敏感比较；lite=词法原文键 + 比较点 CI。
                entries.insert(QDir::cleanPath(current.worktreePath), current);
            }
            current = Entry();
            hasFields = false;
            continue;
        }
        const int sep = line.indexOf(QLatin1Char(' '));
        if (sep < 0)
            continue; // 非 `键 值` 形态（含空 key），忽略
        const QString key = line.left(sep);
        const QString value = line.mid(sep + 1);
        if (key == QStringLiteral("worktree")) {
            if (hasFields && !current.worktreePath.isEmpty())
                entries.insert(QDir::cleanPath(current.worktreePath), current); // 无空行分隔的容错 flush
            current = Entry();
            current.worktreePath = value;
            hasFields = true;
        } else if (key == QStringLiteral("HEAD")) {
            current.head = value;
            hasFields = true;
        } else if (key == QStringLiteral("branch")) {
            current.branch = value;
            hasFields = true;
        }
        // 'bare' / 'detached' / 'lock' 等旗标：不消费（lcc 同款忽略）
    }
    if (hasFields && !current.worktreePath.isEmpty())
        entries.insert(QDir::cleanPath(current.worktreePath), current);
    // 尾 flush：输出不以空行结尾时的最后一条记录（与空行哨兵形互补，两种终止形只可能命中其一）
    return entries;
}

QMap<QString, WorktreeManager::Entry> WorktreeManager::registeredWorktrees(QString *error) const
{
    // lcc registered_worktrees :118-141：过滤三关 = 根自身/越界条目剔除、分支不符剔除、
    // 目录缺失剔除；以**名字**为键返回（名字 → 期望分支 refs/heads/wt/<name> 由 branchForWorktree 单源）。
    QMap<QString, Entry> byName;
    QString registryError;
    const QMap<QString, Entry> entries = parseRegistry(&registryError);
    if (!registryError.isEmpty()) {
        // fix-3 专属①：不可读 ≠ 空集。置错、空表，调用方必须查错（fail-closed）。
        if (error)
            *error = registryError;
        return byName;
    }

    // 根的 cleanPath 词法形（与 parseRegistry 键同口径）；sink 非法（空会话根/空工作目录）
    // 时 cleanRoot 置空 → 下面词法关全拦 = 空集 fail-closed（偏保守，登记；worktreePath
    // 的三关在 create/remove 调用点把关，本查询侧只需根形做过滤，不重复名字校验）。
    const QString cleanRoot =
        m_sessionRootSink().isEmpty() || !m_workDirSink || m_workDirSink().isEmpty()
            ? QString()
            : QDir::cleanPath(worktreesRootDir());

    for (const Entry &entry : entries) {
        const QString entryPath = QDir::cleanPath(entry.worktreePath);
        // FIND-C（Gate②）：口径归一 eqCi/withinCi 单源。假漏靶心就在这两行——git
        // porcelain 输出的盘符大小写与 sink 拼法可能错位（d:/ vs D:/），原
        // AgentPathGuard::isWithinPath 词法大小写敏感 → 合法条目被当越界静默剔除，
        // 注册表视图整体塌空。根过滤与路径包含一律 Windows 不敏感语义。
        if (eqCi(entryPath, cleanRoot))
            continue; // 根自身不是 worktree（lcc :126）
        if (cleanRoot.isEmpty() || !withinCi(entryPath, cleanRoot))
            continue; // 越界条目剔除（lcc relative_to :126-128；cleanRoot 不可用 = 全拦 fail-closed）
        // 名字从路径反推（lcc path.name → str：目录段原名）
        const QString name = entryPath.section(QLatin1Char('/'), -1);
        // 分支不符剔除（lcc :129-133）——含主 checkout（branch 空 = 裸 HEAD/detached，天然不符）
        const QString expected = QStringLiteral("refs/heads/") + branchForWorktree(name);
        if (entry.branch != expected)
            continue;
        if (!QFileInfo(entry.worktreePath).isDir())
            continue; // 目录缺失剔除（lcc :134）
        byName.insert(name, entry);
    }
    if (error)
        error->clear();
    return byName;
}

bool WorktreeManager::isWorktreeRegistered(const QString &name) const
{
    // lcc is_valid_worktree :176-179：空名 = 未绑 = 合法真值；否则查在册表。
    // 注册表不可读 → false（fail-closed，lcc 经 registered_worktrees 吞错回空集同款保守形）。
    if (name.isEmpty())
        return true;
    QString error;
    // FIND-C：name = 路径叶段（同一目录在 Windows 下不分大小写），键查找走 findCi
    // 不敏感扫描（键仍按原样存储）。登记偏差：lcc :178 python `in dict` 大小写敏感。
    return findCi(registeredWorktrees(&error), name) != nullptr;
}

bool WorktreeManager::registeredEntry(const QString &name, Entry *entry, QString *error) const
{
    // lcc _registered_entry :143-166 逐字文案。
    // FIND-A（Gate② minor）：原形 `entry &&` 短路在 entry==nullptr 时静默旁路分支
    // 校验（lcc :159-165 是无条件检查，旁路即放行未验分支的条目 = 安全破口）。
    // 现实两调用点（removeWorktree/resolveWorktreeCwd）恒传非空，公开 API 不可达
    // ——属编程错误。择显式防御 fail-closed：拒绝并给内部缺陷标记串（不为此发明
    // lcc 第二套文案面；不可达路径，注释登记不设测试）。
    if (!entry) {
        if (error)
            *error = QStringLiteral("worktree '%1': internal lookup defect (null entry output)").arg(name);
        return false;
    }
    QString path;
    if (!worktreePath(name, &path, error))
        return false; // lcc :144-146：路径三关错原样上抛（含 worktreePath 内 m1 复校）
    QString registryError;
    const QMap<QString, Entry> entries = parseRegistry(&registryError);
    if (!registryError.isEmpty()) {
        *error = registryError; // lcc :147-149
        return false;
    }
    // FIND-C：原 constFind 走 QMap 键比较=大小写敏感（旧注释声称不敏感=失实，即
    // FIND-C 根因）。改为 findCi 不敏感扫描，键仍按原样存储。lcc 侧键是
    // Path.resolve() canonical 形、比对严格相等——lite 词法键+CI 扫描，偏差登记。
    const Entry *hit = findCi(entries, QDir::cleanPath(path));
    if (!hit) {
        // lcc :150-152：未注册
        *error = QStringLiteral("worktree '%1' is not registered with Git").arg(name);
        return false;
    }
    *entry = *hit;
    const QFileInfo pathInfo(path);
    if (!pathInfo.isDir()) {
        // FIND-B（lcc :154 not path.is_dir()，registered_worktrees :137 同口径）：
        // 原 QFileInfo::exists() 把「路径被普通文件伪装占据」误判为在世，放行到
        // 分支校验甚至返回 cwd；lcc 判 missing。一词之改，文案不变。
        *error = QStringLiteral("worktree '%1' is missing at %2").arg(name, path); // lcc :155-158
        return false;
    }
    // m1（Gate① minor + Gate② 前置③）canonical 复校：目录在世 → junction/符号链接
    // 重定向防护。child 传 canonicalFilePath()（OS 拼写）与 helper 内 canonicalParent
    // 同源对齐，杜绝盘符大小写假拒；破损链接解析为空串 → isWithinPath 必败 =
    // fail-closed。登记偏差：lcc :154-158 此段无再解析复查（其键天然 resolve 过），
    // lite 词法键体系在此补严。
    const QString cleanRoot =
        m_sessionRootSink().isEmpty() ? QString() : QDir::cleanPath(worktreesRootDir());
    if (cleanRoot.isEmpty() ||
        !AgentPathGuard::isWithinPathCanonical(pathInfo.canonicalFilePath(), cleanRoot)) {
        *error = QStringLiteral("Worktree path escapes directory: %1").arg(pyReprLite(name));
        return false;
    }
    const QString branch = branchForWorktree(name);
    // 分支/ref 名比较保持大小写敏感（git ref 语义，lcc :159-165 parity——FIND-C
    // 不敏感口径只覆盖路径/目录名段，ref 名不在此列）。
    if (hit->branch != QStringLiteral("refs/heads/") + branch) {
        // lcc :159-165（{branch!r} = 单引号包裹）
        *error = QStringLiteral("worktree '%1' is not registered on expected branch '%2'")
                     .arg(name, branch);
        return false;
    }
    return true;
}

QString WorktreeManager::foldError(const QString &message)
{
    // lcc create/remove 各门的 'Error: {exc}' / 'Error: {error}' 包装形
    return QStringLiteral("Error: ") + message;
}

QString WorktreeManager::pyRepr(const QString &text)
{
    return pyReprLite(text); // create 的 {name!r} 文案用（供本类各折叠串）
}

// ---------------------------------------------------------------------------
// create（lcc create_worktree :234-312，十连门顺序逐字）
// ---------------------------------------------------------------------------

QString WorktreeManager::createWorktree(const QString &name, const QString &taskId)
{
    if (!m_store)
        return foldError(QStringLiteral("WorktreeManager: task store not wired")); // 装配未毕 fail-closed（偏差登记：lcc 无此态）

    // 门⓪（lcc :236-239 validate + _worktree_path 合一折叠，'Error: {exc}' 形）
    QString path;
    QString pathError;
    if (!worktreePath(name, &path, &pathError))
        return foldError(pathError);

    // Gate③ 第7条（in-flight 防御，lcc 异步壳 in-flight 集的同步内核预埋）：
    // 名字非法已被门⓪挡掉，此处起同名并发拒止。同步路径下入口必空（无重入窗口），
    // 断言性常假——但 Lane B 异步壳（MINOR-4：git 子进程挪到事件循环外/或嵌套轮询）
    // 一旦启用即是承重门，故 RAII 登记。键 CS 口径（lcc 原语义）；CI 双重注册风险
    // 由 gate④ eqCi 兜（见头文件注）。
    if (m_createInFlight.contains(name))
        return foldError(
            QStringLiteral("Worktree '%1' creation is already in progress").arg(name));
    m_createInFlight.insert(name);
    const CreateInFlightGuard inFlightGuard{&m_createInFlight, name};

    const QString branch = branchForWorktree(name);

    // 门①（lcc :241-245 exists → 'Task {id} not found'）。
    // taskExists 是 TaskStore 私有核，公开面只有快照清单（M3 唯一跨模块视图）→ 用
    // listTaskSnapshots 扫一次；台账不可读 → fail-closed 折叠（登记偏差：lcc 的 load
    // 崩溃域无此形，lite 把 IO 灾难折成可读错误；破损任务文件在 lcc 会裸抛未捕获异常）。
    QVector<TaskStore::TaskSnapshot> snapshots;
    QString storeError;
    if (!m_store->listTaskSnapshots(&snapshots, &storeError))
        return foldError(QStringLiteral("cannot read task ledger: ") + storeError);
    const TaskStore::TaskSnapshot *found = nullptr;
    for (const TaskStore::TaskSnapshot &snapshot : snapshots) {
        if (snapshot.id == taskId) {
            found = &snapshot;
            break;
        }
    }
    if (!found) // 不在清单 = 不存在（含扫后消失的竞态窗口）——lcc 门①同文案 fail-closed
        return foldError(QStringLiteral("Task %1 not found").arg(taskId));

    // 门②（lcc :247-252：pending 且无主；owner 空串 ≡ lcc None）
    if (found->status != QStringLiteral("pending") || !found->owner.isEmpty())
        return foldError(QStringLiteral("Task %1 must be pending and unowned").arg(taskId));

    // 门③（lcc :253-258）
    if (!found->worktree.isEmpty())
        return foldError(QStringLiteral("Task %1 already uses worktree '%2'")
                             .arg(taskId, found->worktree));

    // 门④（lcc :259-264：名字未被**别的**任务占用——扫快照，M3 唯一视图）。
    // FIND-C（Gate②）：worktree 名 = 目录路径的叶段，Windows 文件系统大小写不敏感——
    // 「W1」与「w1」指向同一目录，若按 lcc（python 大小写敏感）逐字比较，大小写错位可
    // 造成双重绑定同一目录、互相踩工作区。故本比较走 eqCi（登记偏差：lcc :259 CS；
    // snapshot.id != taskId 属任务 id token 族，维持 CS 口径不变）。
    for (const TaskStore::TaskSnapshot &snapshot : snapshots) {
        if (snapshot.id != taskId && eqCi(snapshot.worktree, name))
            return foldError(QStringLiteral("Worktree '%1' is already bound to another task").arg(name));
    }

    // 门⑤（lcc :265-267：目标路径不存在——已存在目录不做任何清理，看一眼就走）。
    // exists() 交 OS 判定，Windows OS 天然大小写不敏感，无字符串口径问题；m1 的
    // junction/canonical 复校已由门⓪ worktreePath（返回 path 前）覆盖，此处不再重复。
    if (QFileInfo::exists(path))
        return foldError(QStringLiteral("Worktree path already exists: %1").arg(path));

    // 门⑥（lcc :268-276：workDir 必须是仓库 toplevel）。
    // 比对偏差登记：lcc 两侧 resolve()（canonical）相等；lite 词法 cleanPath + Windows
    // 大小写不敏感（git 输出正斜杠/盘符大小写与本地拼法常不同形）。FIND-C（Gate②）：
    // 归一单源经 eqCi，注释里「canonical 复校归 P3」已由 m1 在 worktreePath 落地。
    const QString cleanWorkDir = QDir::cleanPath(m_workDirSink ? m_workDirSink() : QString());
    QString toplevel;
    const bool toplevelOk = runGit({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")},
                                   &toplevel);
    if (!toplevelOk || !eqCi(QDir::cleanPath(toplevel), cleanWorkDir))
        return foldError(QStringLiteral("Working directory must be the root of a Git repository"));

    // 门⑦（lcc :277-282：分支名合法性交 git 自己裁决 check-ref-format --branch）
    QString branchCheck;
    if (!runGit({QStringLiteral("check-ref-format"), QStringLiteral("--branch"), branch},
                &branchCheck))
        return foldError(QStringLiteral("Invalid worktree branch '%1': %2").arg(branch, branchCheck));

    // 门⑧（lcc :283-288：show-ref --verify --quiet，rc0=存在→拒；rc 非 0 一律按不存在
    //   ——lcc `exists,_ = run_git(...)` 只取 ok 的逐字口径。非仓库等异常已被门⑥拦住，
    //   即便漏网，门⑨注册表不可读亦 fail-closed 兜底，无放行风险）
    QString showRefOut;
    const bool branchExists = runGit({QStringLiteral("show-ref"), QStringLiteral("--verify"),
                                      QStringLiteral("--quiet"),
                                      QStringLiteral("refs/heads/") + branch},
                                     &showRefOut);
    Q_UNUSED(showRefOut); // --quiet 语义：rc1 无输出，本串不消费
    if (branchExists)
        return foldError(QStringLiteral("Branch '%1' already exists").arg(branch));

    // 门⑨（lcc :289-293：注册表可读性——不可读**拒绝**而非当「无 worktree」，fix-3 专属①）
    QString registryError;
    const QMap<QString, Entry> entries = parseRegistry(&registryError);
    if (!registryError.isEmpty())
        return foldError(registryError);

    // 门⑩（lcc :294-298：路径未注册）。FIND-C（Gate②）：QMap::contains 键比较大小写
    // **敏感**（原实现即假 miss 风险：git 输出盘符大小写与本地拼法不同形时，同一路径
    // 会被判「未注册」而放行双注册）→ 改 findCi 大小写不敏感扫描（键仍原文存储）。
    if (findCi(entries, QDir::cleanPath(path)) != nullptr)
        return foldError(QStringLiteral("Worktree path is already registered: %1").arg(path));

    // ---- 真实操作（lcc :299-312）----
    QDir().mkpath(QFileInfo(path).dir().path()); // 父目录 = .worktrees 根（lcc mkdir parents=True exist_ok=True）

    QString addOut;
    const bool addOk = runGit({QStringLiteral("worktree"), QStringLiteral("add"),
                               QStringLiteral("-b"), branch, path, QStringLiteral("HEAD")},
                              &addOut);
    if (!addOk) {
        // **绝不自动清理**（lane 前言铁律；TOCTOU② 重查现场分类残留，lcc :300-310 逐字）。
        // 重查三件现场：注册表可读性、该路径是否已注册、分支是否已建、目录是否已现。
        QString recheckError;
        const QMap<QString, Entry> recheck = parseRegistry(&recheckError);
        QStringList artifacts;
        const bool dirAppeared = QFileInfo::exists(path);
        // FIND-C（Gate②）：残留重查与门⑩同口径走 findCi（两侧口径必须一致，否则
        // 「门⑩放行但重查报未注册/反之」会让 Partial operation 文案失真）。
        const bool registeredAgain =
            recheckError.isEmpty() && findCi(recheck, QDir::cleanPath(path)) != nullptr;
        QString recheckBranch;
        const bool branchAppeared = runGit({QStringLiteral("show-ref"), QStringLiteral("--verify"),
                                            QStringLiteral("--quiet"),
                                            QStringLiteral("refs/heads/") + branch},
                                           &recheckBranch);
        if (dirAppeared)
            artifacts.append(QStringLiteral("checkout path %1").arg(pyRepr(path)));
        if (registeredAgain)
            artifacts.append(QStringLiteral("registered Git worktree"));
        if (branchAppeared)
            artifacts.append(QStringLiteral("branch %1").arg(pyRepr(branch)));

        if (!artifacts.isEmpty()) {
            return QStringLiteral(
                       "Partial operation: git worktree add reported an error after leaving %1. "
                       "Task %2 remains unbound and no Git data was deleted. "
                       "Run `git worktree list`, inspect %3 and %4, then keep or remove those "
                       "artifacts manually after preserving any work. Git error: %5")
                .arg(artifacts.join(QStringLiteral(", ")), taskId, pyRepr(path), pyRepr(branch),
                     truncateForReport(addOut));
        }
        return QStringLiteral("Git error: %1").arg(truncateForReport(addOut));
    }

    // 成功链收口：绑定回写（存**名字**非路径——gate① M1，lcc :304 task.worktree = name）。
    QString bindError;
    if (!m_store->setWorktree(taskId, name, &bindError)) {
        return QStringLiteral(
                   "Partial success: Worktree '%1' was created at %2 on branch '%3', "
                   "but task binding failed: %4. Git data was retained for manual recovery.")
            .arg(name, path, branch, bindError);
    }
    return QStringLiteral("Created worktree '%1' at %2 for task %3").arg(name, path, taskId);
}

// ---------------------------------------------------------------------------
// remove（lcc remove_worktree :323-385，五连门；**内核 API 非工具**，fix-3 专属④）
// ---------------------------------------------------------------------------

bool WorktreeManager::removeWorktree(const QString &name, bool discardChanges, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = foldError(message);
        return false;
    };
    if (error)
        error->clear();

    if (!m_store)
        return fail(QStringLiteral("WorktreeManager: task store not wired"));

    // 门⓪/①（lcc :325-333：validate + _registered_entry——注册条目合法性一把核：
    // 名字/路径三关、注册表可读、在册、目录存在、分支==期望）
    QString path;
    QString pathError;
    if (!worktreePath(name, &path, &pathError))
        return fail(pathError);
    Entry entry;
    QString entryError;
    if (!registeredEntry(name, &entry, &entryError))
        return fail(entryError);

    // 门②③（lcc :334-347：有任务绑定；绑定的活跃任务（非 completed）一律拦）
    QVector<TaskStore::TaskSnapshot> snapshots;
    QString storeError;
    if (!m_store->listTaskSnapshots(&snapshots, &storeError))
        return fail(QStringLiteral("cannot read task ledger: ") + storeError);
    QStringList boundIds;
    QStringList activeIds;
    for (const TaskStore::TaskSnapshot &snapshot : snapshots) {
        // FIND-C：绑定名比较走 eqCi 单一口径（与 create 门④对称）——name=路径叶段，
        // Windows 下 W1/w1 同目录，解绑扫描若 CS 会漏掉大小写错位绑定、留孤儿绑定账。
        if (eqCi(snapshot.worktree, name)) {
            boundIds.append(snapshot.id);
            if (snapshot.status != QStringLiteral("completed"))
                activeIds.append(snapshot.id);
        }
    }
    if (boundIds.isEmpty())
        return fail(QStringLiteral("Worktree '%1' is not bound to a task").arg(name));
    if (!activeIds.isEmpty())
        return fail(QStringLiteral("Worktree '%1' is bound to active task %2; complete it before removal")
                        .arg(name, activeIds.first())); // lcc active[0]：快照扫序首条

    // 门④（lcc :348-353：活跃租约仍指向该路径 → 拒。owners 已按 TaskStore 字典序）。
    // 口径登记：leasesPointingAt 词法 cleanPath + 大小写不敏感；junction/符号链接伪装
    // 归 m1/P3 消费点复校（isWithinPathCanonical 预留件），本 lane 词法偏保守=宁拒不误放。
    QStringList owners;
    if (m_store->leasesPointingAt(path, &owners) && !owners.isEmpty())
        return fail(QStringLiteral("Worktree '%1' is still in use by %2; wait for the turn to end")
                        .arg(name, owners.join(QStringLiteral(", "))));

    // 门⑤（lcc :354-368：工作区干净——**命令失败按脏处理**（:369-374 同款保守）。
    // discardChanges=true 只豁免本门并追加 --force，①~④寸步不让）
    QString status;
    const bool statusOk = runGit({QStringLiteral("status"), QStringLiteral("--porcelain"),
                                  QStringLiteral("--ignored")},
                                 &status, path);
    if (!statusOk)
        return fail(QStringLiteral("Cannot verify worktree '%1' status: %2").arg(name, status));
    if (status != QStringLiteral("(no output)") && !discardChanges) {
        int changed = 0;
        const QStringList statusLines = status.split(QLatin1Char('\n'));
        for (const QString &line : statusLines) {
            if (!line.trimmed().isEmpty())
                ++changed;
        }
        return fail(QStringLiteral("Worktree '%1' has %2 uncommitted change(s); "
                                   "preserve or discard them manually")
                        .arg(name).arg(changed));
    }

    // ---- 真实移除（lcc :369-377；成功后分支永不删除 = 不跑任何 branch -D）----
    QStringList removeArgs{QStringLiteral("worktree"), QStringLiteral("remove")};
    if (discardChanges)
        removeArgs.append(QStringLiteral("--force"));
    removeArgs.append(path);
    QString removeOut;
    if (!runGit(removeArgs, &removeOut))
        return fail(QStringLiteral("Git error: %1").arg(truncateForReport(removeOut)));

    // 解绑（lcc :378-390：逐绑定任务 worktree=None；写回失败 = Partial success 保留现场）
    QString unbindError;
    for (const QString &taskId : boundIds) {
        QString clearError;
        if (!m_store->clearWorktree(taskId, &clearError) && unbindError.isEmpty())
            unbindError = QStringLiteral("Task %1: %2").arg(taskId, clearError);
    }
    if (!unbindError.isEmpty()) {
        return fail(QStringLiteral(
                        "Partial success: Worktree '%1' was removed and branch '%2' retained, "
                        "but task unbinding failed: %3. Manual recovery is required.")
                        .arg(name, branchForWorktree(name), unbindError));
    }
    if (error)
        *error = QStringLiteral("Worktree '%1' removed; branch '%2' retained")
                     .arg(name, branchForWorktree(name)); // 成功文案走 *error 交还（内核 API 无独立返回值）
    return true;
}

// ---------------------------------------------------------------------------
// cwd 解析（P3 接线 TaskStore::setCwdResolver 的目标形态，gate① M4/M1）
// ---------------------------------------------------------------------------

QString WorktreeManager::resolveWorktreeCwd(const TaskStore::TaskSnapshot &task, QString *error) const
{
    // M4 早退兜底：未绑定任务返回空串且**零 git 进程**（TaskStore::resolveTaskCwd :750
    // 已保证空绑定不进 resolver，此处再兜一层——热路径每 claim 白跑 porcelain 不可接受）。
    // 空串 = 「本回调对该任务不解析」，TaskStore 走回落链（workDirSink→sessionRootSink）。
    // 出参契约：任何成功/不解析路径都须清空 *error（调用方复用同一 error 变量时，
    // 陈旧值冒充新错 = 假故障；单测「空绑定不置错」实证捕获过此瑕疵）。
    if (task.worktree.isEmpty()) {
        if (error)
            error->clear();
        return QString();
    }

    // 绑定破损 → 置 *error（TaskStore fail-closed 上交：nonempty *error = 不可解）。
    // 偏差登记：lcc task_worktree_cwd :184-190 破损时静默回 None（→ 回落到主目录干活）；
    // lite 按 gate① 裁决改 fail-closed——破损绑定悄悄回主目录 = 队友在错误目录写花，
    // 比失败更危险。文案 = 钉死串（M1），registeredEntry 的细分错并入该串
    //（TaskStore.h :161-162 预告的 "Worktree '<name>' binding is broken for task <id>"
    //  人类文案由 TaskStore 侧组装，本 resolver 只交协议错）。
    Entry entry;
    QString entryError;
    if (!registeredEntry(task.worktree, &entry, &entryError)) {
        if (error)
            *error = QStringLiteral("worktree '%1' is not available for task %2")
                         .arg(task.worktree, task.id);
        Q_UNUSED(entryError); // 细分原因（未注册/缺失/分支不符）不上交——钉死单文案防模型侧刮文本报错漂移（M3）
        return QString();
    }
    if (error)
        error->clear();
    return QDir::cleanPath(entry.worktreePath); // 注册表原文路径（git 写的形，含正斜杠）
}
