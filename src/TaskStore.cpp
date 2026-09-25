#include "TaskStore.h"

#include "AgentConstants.h" // 中间目录名单源（kTaskDirName，与 CompactManager 等拼接方共用）

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QDebug>

namespace {

// ---- lcc s10 任务图文本层工具（python 语义近似，各处偏差登记）----
// json.dumps 单值紧凑字面量近似：控制字符 Qt 用 \u00XX（python 用 \n 等简称）、
// 非 ASCII Qt 原样 UTF-8（python 默认 ensure_ascii=True 转 \uXXXX）、null/true 与 None/True
// 拼写差异——语义等价可再解析，仅观感偏差。
// 注意：切片剥外层方括号必须在 QByteArray 字节空间进行后再 fromUtf8，
// 反之（先转 QString 再按字节数 mid）在非 ASCII 内容下会因 UTF-8 字节数 >
// UTF-16 码元数而超发 count，尾部 ']' 泄入返回值（Gate4 BLOCKER-1）。
QString jsonCompactLiteral(const QJsonValue &value)
{
    QJsonArray wrapper;
    wrapper.append(value);
    const QByteArray json = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString jsonStringLiteral(const QString &value)
{
    return jsonCompactLiteral(QJsonValue(value));
}

// python repr(str) 复刻：优先单引号；含单引号且不含双引号时用双引号（免转义）；
// 转义反斜杠/\n/\r/\t 与生效引号，其余 <0x20 用 \xNN。
// 偏差：同时含两种引号时 python 仍用单引号并转义 \'，本实现改用双引号
// （任务 ID/subject 实际极少含引号，仅影响错误消息观感）
QString pythonStrRepr(const QString &value)
{
    const bool preferDouble = value.contains(QLatin1Char('\'')) && !value.contains(QLatin1Char('"'));
    const QChar quote = preferDouble ? QLatin1Char('"') : QLatin1Char('\'');
    QString out;
    out += quote;
    for (const QChar &c : value) {
        if (c == QLatin1Char('\\'))
            out += QStringLiteral("\\\\");
        else if (c == QLatin1Char('\n'))
            out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\r'))
            out += QStringLiteral("\\r");
        else if (c == QLatin1Char('\t'))
            out += QStringLiteral("\\t");
        else if (c == quote) {
            out += QLatin1Char('\\'); // 生效引号前加反斜杠
            out += c;
        }
        else if (c.unicode() < 0x20)
            out += QStringLiteral("\\x%1").arg(static_cast<int>(c.unicode()), 2, 16, QLatin1Char('0'));
        else
            out += c;
    }
    out += quote;
    return out;
}

// python 列表 f-string 插值形态（如 f"Blocked by: {dependencies}"）：['a', 'b']
QString pythonListRepr(const QStringList &values)
{
    QStringList parts;
    parts.reserve(values.size());
    for (const QString &v : values)
        parts.append(pythonStrRepr(v));
    return QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]");
}

// lcc TASK_ID_PATTERN 的 fullmatch 等价：锚定匹配 + 捕获段恰覆盖全串
//（显式长度校验规避 PCRE $ 允许末尾换行的怪癖，与 python fullmatch 严格一致）
bool isTaskIdFull(const QString &taskId)
{
    static const QRegularExpression re(QStringLiteral("^task_[0-9a-f]{8}$"));
    const QRegularExpressionMatch m = re.match(taskId);
    return m.hasMatch() && m.capturedStart(0) == 0 && m.capturedLength(0) == taskId.size();
}

} // namespace

// ============================================================================
// lcc s10 任务图（TaskManager 移植；重构第三轮自 AgentLoop 内联段拆为独立类文件，
// 原“SkillManager 档不建类文件”裁决随本块与主循环零耦合的事实解除——见 TaskStore.h 拆分动因）
// 存储 <会话根>/.task/task_<hex8>.json，一任务一文件，每操作直读盘无缓存；
//（lite 有意偏差：lcc 放 workDir 直下，lite 收进 .lite-harness 中间目录，见 taskRootDir）
// 内核 bool + 错误出参保持 lcc 抛错语义，六个 run_* 处理器把一切失败折叠为错误字符串
// 直接作为工具输出（lcc 裸抛崩主循环，lite 对齐 executeTool“一切失败皆字符串”纪律——登记偏差；
// 错误字符串不加 'Error:' 前缀，内核文案逐字即工具输出）。
// 控制台 print → qDebug().noquote()（s04 承接 lcc 控制台输出的移植先例）。
// ============================================================================

TaskStore::TaskStore(std::function<QString()> sessionRootSink)
    : m_sessionRootSink(std::move(sessionRootSink))
{
}

QString TaskStore::taskRootDir() const
{
    // lcc env.py:19 taskDirPath = workDirPath / ".task"（第四隐藏目录）；
    // lite 有意偏差：收进 .lite-harness 中间目录（会话隔离后为 sessions/<id>/），不在用户项目根撒目录。
    // .lite-harness 中间层由宿主 sessionDataRoot（sessionRootSink）统一提供，本处仅拼叶子段 .task
    // （目录名单源于 AgentConst::kTaskDirName，与 CompactManager 等拼接方共用）
    return QDir(m_sessionRootSink()).filePath(AgentConst::kTaskDirName);
}

bool TaskStore::taskFilePath(const QString &taskId, QString *path, QString *error) const
{
    // lcc _path :37-45：ID 非 str（JSON 层已约束为字符串）或未过 fullmatch →
    // ValueError(f"Invalid task ID:{task_id!r}")（冒号后无空格，快照逐字）
    const QString root = taskRootDir();
    // lcc _root :28-34 目录逃逸防御：lite 中会话根为归一化绝对路径、".task" 为固定段，
    // 该检查恒通过——防御死路径按 lcc 保留（状态文件 s10 裁决）
    const QString dataRoot = QDir::cleanPath(m_sessionRootSink());
    if (root != dataRoot + QLatin1Char('/') + AgentConst::kTaskDirName) {
        if (error)
            *error = QStringLiteral("TaskManager escapes the workspace");
        return false;
    }
    if (!isTaskIdFull(taskId)) {
        if (error)
            *error = QStringLiteral("Invalid task ID:%1").arg(pythonStrRepr(taskId));
        return false;
    }
    const QString candidate =
        QDir::cleanPath(root + QLatin1Char('/') + taskId + QStringLiteral(".json"));
    // ID 字符集已被正则约束，此检查恒通过——lcc _path :43-44 resolve 逃逸防御的等价死路径
    if (candidate != root && !candidate.startsWith(root + QLatin1Char('/'))) {
        if (error)
            *error = QStringLiteral("Invalid task ID:%1").arg(pythonStrRepr(taskId));
        return false;
    }
    if (path)
        *path = candidate;
    return true;
}

bool TaskStore::taskExists(const QString &taskId, bool *exists, QString *error) const
{
    QString path;
    if (!taskFilePath(taskId, &path, error))
        return false;
    if (exists)
        *exists = QFileInfo(path).isFile(); // lcc exists = _path(id).is_file()
    return true;
}

QString TaskStore::taskToJsonText(const Task &task) const
{
    // lcc save/get_task：json.dumps(asdict(task), indent=2)（无尾换行）。
    // QJsonObject 序列化按键名字典序，与 Task 声明序不符 → 手工按
    // id/subject/description/status/owner/timestamp/blockedBy 顺序输出（偏差登记见 jsonCompactLiteral 注释）
    QStringList lines;
    lines << QStringLiteral("  \"id\": %1,").arg(jsonStringLiteral(task.id));
    lines << QStringLiteral("  \"subject\": %1,").arg(jsonStringLiteral(task.subject));
    lines << QStringLiteral("  \"description\": %1,").arg(jsonStringLiteral(task.description));
    lines << QStringLiteral("  \"status\": %1,").arg(jsonStringLiteral(task.status));
    lines << QStringLiteral("  \"owner\": %1,")
                 .arg(task.owned ? jsonStringLiteral(task.owner) : QStringLiteral("null"));
    // timestamp（lcc c3fe3f2 对齐）：JSON number。QString::number(ts,'f',6) 固定 6 位小数——与
    // lcc python json.dumps(float) 的最短 repr 观感有差异（登记为文本格式偏差：语义等价、可解析回同值）
    lines << QStringLiteral("  \"timestamp\": %1,").arg(QString::number(task.timestamp, 'f', 6));
    if (task.blockedBy.isEmpty()) {
        lines << QStringLiteral("  \"blockedBy\": []");
    } else {
        QStringList items;
        items.reserve(task.blockedBy.size());
        for (const QString &dep : task.blockedBy)
            items << QStringLiteral("    %1").arg(jsonStringLiteral(dep));
        lines << QStringLiteral("  \"blockedBy\": [");
        lines << items.join(QStringLiteral(",\n"));
        lines << QStringLiteral("  ]");
    }
    lines << QStringLiteral("}");
    return QStringLiteral("{\n") + lines.join(QLatin1Char('\n'));
}

bool TaskStore::loadTask(const QString &taskId, Task *task, QString *error) const
{
    QString path;
    if (!taskFilePath(taskId, &path, error))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // lcc read_text 抛 FileNotFoundError（消息含完整路径）；lite 归一为同族错误串（登记偏差）
        if (error)
            *error = QStringLiteral("Task file not found: %1").arg(path);
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    Task parsed;
    bool shapeOk = doc.isObject();
    const QJsonObject obj = shapeOk ? doc.object() : QJsonObject();
    if (shapeOk) {
        // lcc Task(**data)：缺键/多键 → TypeError；非字符串字段 python 不校验类型。
        // lite 从严：文本字段必须为 string、owner 为 null|string、blockedBy 为字符串数组，
        // 否则统一归 'Invalid task file contents' 族（登记偏差）。
        // timestamp（lcc c3fe3f2）兼容性有意偏差：lcc Task(**data) 缺 timestamp → 崩，
        // lite 对存量旧任务文件（6 键、无 timestamp）容错取 0.0、不判 Invalid；键数放宽为 6 或 7：
        // 6 键（存量）必无 timestamp，7 键必含 timestamp——多一个杂键即判 Invalid（保持
        // lite 原"多键从严"纪律）；timestamp 键存在时类型必须为数值，否则归入同族 Invalid。
        const bool hasTs = obj.contains(QStringLiteral("timestamp"));
        shapeOk = ((obj.size() == 6 && !hasTs) || (obj.size() == 7 && hasTs))
            && obj.contains(QStringLiteral("id"))
            && obj.contains(QStringLiteral("subject")) && obj.contains(QStringLiteral("description"))
            && obj.contains(QStringLiteral("status")) && obj.contains(QStringLiteral("owner"))
            && obj.contains(QStringLiteral("blockedBy"))
            && obj.value(QStringLiteral("id")).isString()
            && obj.value(QStringLiteral("subject")).isString()
            && obj.value(QStringLiteral("description")).isString()
            && obj.value(QStringLiteral("status")).isString()
            && (obj.value(QStringLiteral("owner")).isNull()
                || obj.value(QStringLiteral("owner")).isString())
            && (!hasTs || obj.value(QStringLiteral("timestamp")).isDouble());
        const QJsonArray deps = obj.value(QStringLiteral("blockedBy")).toArray();
        if (shapeOk && !obj.value(QStringLiteral("blockedBy")).isArray())
            shapeOk = false;
        for (const QJsonValue &dep : deps) {
            if (!dep.isString()) {
                shapeOk = false;
                break;
            }
        }
        if (shapeOk) {
            parsed.id = obj.value(QStringLiteral("id")).toString();
            parsed.subject = obj.value(QStringLiteral("subject")).toString();
            parsed.description = obj.value(QStringLiteral("description")).toString();
            parsed.status = obj.value(QStringLiteral("status")).toString();
            const QJsonValue owner = obj.value(QStringLiteral("owner"));
            parsed.owned = !owner.isNull();
            parsed.owner = owner.toString(); // null → 空串（owned=false 时不呈现）
            // timestamp：缺键（存量旧文件）→ 0.0（登记为对 lcc 崩语义的有意偏差）；有键取 double（JSON 数值）
            parsed.timestamp = hasTs ? obj.value(QStringLiteral("timestamp")).toDouble() : 0.0;
            for (const QJsonValue &dep : deps)
                parsed.blockedBy.append(dep.toString());
        }
    }
    if (!shapeOk) {
        if (error)
            *error = QStringLiteral("Invalid task file contents: %1").arg(taskId);
        return false;
    }
    if (parsed.id != taskId) {
        if (error)
            *error = QStringLiteral("Task file ID does not match %1").arg(taskId); // 冒号后有空格，快照逐字
        return false;
    }
    if (parsed.status != QStringLiteral("pending") && parsed.status != QStringLiteral("in_progress")
        && parsed.status != QStringLiteral("completed")) {
        if (error)
            *error = QStringLiteral("Invalid task status:%1").arg(parsed.status); // 冒号后无空格，快照逐字
        return false;
    }
    if (task)
        *task = parsed;
    return true;
}

bool TaskStore::saveTask(const Task &task, QString *error) const
{
    QString path;
    if (!taskFilePath(task.id, &path, error))
        return false;
    QDir().mkpath(taskRootDir()); // lcc _path(create_root=True) → _root(create=True) mkdir parents
    // 状态文件原子覆写（QSaveFile）：防止半截 JSON 毁掉任务图，对齐 persistHistory 纪律；
    // 失败时 cancelWriting 丢弃临时文件不伤目标
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        // lcc write_text 抛 OSError；lite 归一错误串族（登记偏差）
        if (error)
            *error = QStringLiteral("Task file write failed: %1").arg(path);
        return false;
    }
    const QByteArray bytes = taskToJsonText(task).toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("Task file write failed: %1").arg(path);
        return false;
    }
    return true;
}

bool TaskStore::createTask(const QString &subject, const QString &description, Task *task,
                           QString *error) const
{
    const QString trimmed = subject.trimmed(); // lcc :53：subject.strip() 后校验并存储；description 不 strip
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Task subject cannot be empty");
        return false;
    }
    QDir().mkpath(taskRootDir()); // lcc _root(create=True)
    // secrets.token_hex(4) ≡ 8 位小写十六进制；QRandomGenerator 32bit 恰好 8 hex
    for (int attempt = 0; attempt < 100; ++attempt) {
        const QString id = QStringLiteral("task_%1")
                               .arg(static_cast<quint32>(QRandomGenerator::global()->generate()), 8,
                                    16, QLatin1Char('0'));
        QString path;
        if (!taskFilePath(id, &path, error))
            return false; // 随机 ID 恒过正则，理论不可达
        if (QFile::exists(path))
            continue; // 撞名重试 ≡ 原 NewOnly 语义（QSaveFile 无独占创建；GUI 线程串行循环，无并发竞态）
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            // 第六轮审计 C4：open 失败非撞名（权限/磁盘类），continue 会空转百次后报
            // 误导性的"无法分配唯一 ID"——立即返回真实原因，与本文件写盘失败文案族同式
            if (error)
                *error = QStringLiteral("Task file write failed: %1 (%2)").arg(path, file.errorString());
            return false;
        }
        Task created;
        created.id = id;
        created.subject = trimmed;
        created.description = description;
        created.status = QStringLiteral("pending");
        created.owned = false; // python owner=None
        // 创建时间戳（lcc c3fe3f2 对齐 python datetime.now().timestamp()）：epoch 秒 double；
        // toMSecsSinceEpoch() 为 qint64，除以 1000.0 提升为 double，与 struct Task 字段类型一致
        created.timestamp = QDateTime::currentDateTime().toMSecsSinceEpoch() / 1000.0;
        const QByteArray bytes = taskToJsonText(created).toUtf8();
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            file.cancelWriting(); // 丢弃临时文件，目标路径从未被污染
            if (error)
                *error = QStringLiteral("Task file write failed: %1").arg(path);
            return false;
        }
        if (task)
            *task = created;
        return true;
    }
    if (error)
        *error = QStringLiteral("Could not allocate a unique task ID"); // 100 次穷尽（lcc RuntimeError 族）
    return false;
}

bool TaskStore::dependsOn(const QString &startId, const QString &targetId, bool *depends,
                          QString *error) const
{
    // lcc _depends_on :76-93：栈式 DFS（pop 尾部 ≡ takeLast）；load 失败 lcc 不捕获会崩，
    // lite 按状态文件 :109 裁决容错跳过（与 incompleteDependencies 的容错方向相反——特性原样复刻勿统一）
    Q_UNUSED(error);
    QStringList stack;
    stack.append(startId);
    QSet<QString> visited;
    while (!stack.isEmpty()) {
        const QString current = stack.takeLast();
        if (current == targetId) {
            *depends = true;
            return true;
        }
        if (visited.contains(current))
            continue;
        visited.insert(current);
        Task node;
        QString loadError;
        if (!loadTask(current, &node, &loadError)) {
            qWarning().noquote() << QStringLiteral("[task] cycle check skipped unloadable task: %1")
                                        .arg(loadError);
            continue; // 容错：该节点出边视为不可达
        }
        stack.append(node.blockedBy);
    }
    *depends = false;
    return true;
}

bool TaskStore::updateTaskDependencies(const QString &taskId, const QJsonArray &addBlockedBy,
                                        Task *updated, QString *error) const
{
    // lcc update_dependencies :94-118（addBlockedBy 是否数组由 runUpdateTask 先行校验，
    // 对应 :95 在 load 之前的 isinstance(list) 检查与文案顺序）
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    if (task.status != QStringLiteral("pending") || task.owned) {
        if (error)
            *error = QStringLiteral("Task %1 dependencies can only be updated while pending and unowned")
                         .arg(taskId);
        return false;
    }
    // dict.fromkeys 去重保序（首次出现序）；python 非可哈希元素（list/dict）→ TypeError，
    // lite 归入 Invalid task ID 错误族（登记偏差）
    QVector<QJsonValue> deps;
    QSet<QString> seen;
    for (const QJsonValue &item : addBlockedBy) {
        const QString key = item.isString() ? item.toString() : jsonCompactLiteral(item);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        deps.append(item);
    }
    // 校验循环（逐字保持 lcc 顺序：自依赖 → 存在性 → 环检测）
    for (const QJsonValue &item : deps) {
        if (!item.isString()) {
            if (error)
                *error = QStringLiteral("Invalid task ID:%1").arg(jsonCompactLiteral(item));
            return false;
        }
        const QString dep = item.toString();
        if (dep == taskId) {
            if (error)
                *error = QStringLiteral("Task cannot depend on itself");
            return false;
        }
        bool exists = false;
        if (!taskExists(dep, &exists, error))
            return false; // 非法格式依赖 → Invalid task ID 文案（lcc exists→_path 抛错等价，非 Dependency not found）
        if (!exists) {
            if (error)
                *error = QStringLiteral("Dependency not found:%1").arg(dep); // 冒号后无空格，快照逐字
            return false;
        }
        if (!task.blockedBy.contains(dep)) {
            bool cyclic = false;
            if (!dependsOn(dep, taskId, &cyclic, error))
                return false;
            if (cyclic) {
                if (error)
                    *error = QStringLiteral("Dependency cycle detected: %1 - > %2").arg(taskId, dep);
                // 文案 "- >" 中 lcc 原生的多余空格为逐字红线，勿修正（tools/task_manager :115）
                return false;
            }
        }
    }
    // 追环检测通过后统一追加（仅未存在的），再落盘——lcc 两循环结构原样
    for (const QJsonValue &item : deps) {
        const QString dep = item.toString();
        if (!task.blockedBy.contains(dep))
            task.blockedBy.append(dep);
    }
    if (!saveTask(task, error))
        return false;
    if (updated)
        *updated = task;
    return true;
}

QStringList TaskStore::incompleteDependencies(const Task &task) const
{
    // lcc incomplete_dependencies :160-169：缺失/坏依赖文件计为未完成（except 分支 append）——
    // 与 dependsOn 的容错方向相反，lcc 特性原样复刻，勿统一
    QStringList unfinished;
    for (const QString &dep : task.blockedBy) {
        Task depTask;
        QString loadError;
        if (!loadTask(dep, &depTask, &loadError)) {
            unfinished.append(dep);
            continue;
        }
        if (depTask.status != QStringLiteral("completed"))
            unfinished.append(dep);
    }
    return unfinished;
}

bool TaskStore::canStart(const QString &taskId, bool *startable, QString *error) const
{
    // lcc can_start :171-172
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    *startable = incompleteDependencies(task).isEmpty();
    return true;
}

bool TaskStore::listTasks(QVector<Task> *tasks, QString *error) const
{
    // lcc list :136-143：目录缺失 → 空表；sorted(glob) ≡ entryList QDir::Name 升序。
    // 偏差登记：文件名未过 ID 正则的脏文件 lcc 会 load 崩溃整表，lite 跳过（更稳）；
    // 正则通过但内容损坏的文件仍向上抛错（与 lcc ValueError 族一致，由 run_* 折叠）
    tasks->clear();
    const QString root = taskRootDir();
    if (!QFileInfo(root).isDir())
        return true;
    const QStringList names = QDir(root).entryList(QStringList() << QStringLiteral("task_*.json"),
                                                   QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QString stem = QString(name).left(name.size() - int(qstrlen(".json")));
        if (!isTaskIdFull(stem))
            continue; // 脏文件跳过（登记偏差）
        Task task;
        if (!loadTask(stem, &task, error))
            return false;
        tasks->append(task);
    }
    return true;
}

bool TaskStore::claimTask(const QString &taskId, const QString &owner, QString *result,
                          QString *error) const
{
    // lcc claim_task :176-190：业务性失败（状态不符/被阻塞）是返回文本而非异常 → 走 *result
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    if (task.status != QStringLiteral("pending")) {
        *result = QStringLiteral("Task %1 is %2, cannot claim").arg(taskId, task.status);
        return true;
    }
    const QStringList deps = incompleteDependencies(task);
    if (!deps.isEmpty()) {
        *result = QStringLiteral("Blocked by: %1").arg(pythonListRepr(deps)); // f-string 插值 python list ≡ repr 形态
        return true;
    }
    task.owned = true;
    task.owner = owner;
    task.status = QStringLiteral("in_progress");
    if (!saveTask(task, error))
        return false;
    qDebug().noquote() << QStringLiteral("[task] claim %1 -> in_progress (owner: %2)").arg(task.subject, owner);
    *result = QStringLiteral("Claimed %1 %2").arg(task.id, task.subject);
    return true;
}

bool TaskStore::completeTask(const QString &taskId, const QString &owner, QString *result,
                             QString *error) const
{
    // lcc complete_task :194-222
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    if (task.status != QStringLiteral("in_progress")) {
        *result = QStringLiteral("Task %1 is %2, cannot complete").arg(taskId, task.status);
        return true;
    }
    if (!task.owned || task.owner != owner) {
        // lcc :200 写的是 {Task.owner}（类属性访问，dataclass 无此类属性 → AttributeError 崩溃笔误）；
        // lite 修正为实例值 task.owner（状态文件 :111 裁决，登记偏差）；未认领时 python None 呈现 'None'
        *result = QStringLiteral("Task %1 is owned by %2, not %3")
                      .arg(taskId, task.owned ? task.owner : QStringLiteral("None"), owner);
        return true;
    }
    // ready_before：完成前已“可开始”的 pending 带依赖任务 id 集合（lcc :204-209，can_start 逐个重载）
    QSet<QString> readyBefore;
    QVector<Task> all;
    if (!listTasks(&all, error))
        return false;
    for (const Task &candidate : all) {
        if (candidate.status != QStringLiteral("pending") || candidate.blockedBy.isEmpty())
            continue;
        bool startable = false;
        if (!canStart(candidate.id, &startable, error))
            return false;
        if (startable)
            readyBefore.insert(candidate.id);
    }
    task.status = QStringLiteral("completed");
    if (!saveTask(task, error))
        return false;
    // unblocked：完成前不可开始、现在可开始的 → 收集 subject（快照 :214-220，状态文件 '{ids}' 为宽泛描述）
    QStringList unblocked;
    QVector<Task> after;
    if (!listTasks(&after, error))
        return false;
    for (const Task &candidate : after) {
        if (candidate.status != QStringLiteral("pending") || candidate.blockedBy.isEmpty()
            || readyBefore.contains(candidate.id))
            continue;
        bool startable = false;
        if (!canStart(candidate.id, &startable, error))
            return false;
        if (startable)
            unblocked.append(candidate.subject);
    }
    qDebug().noquote() << QStringLiteral("[task] complete %1").arg(task.subject);
    QString message = QStringLiteral("Completed %1 (%2)").arg(task.id, task.subject);
    if (!unblocked.isEmpty()) {
        message += QStringLiteral("\nUnblocked: %1").arg(unblocked.join(QStringLiteral(", ")));
        qDebug().noquote() << QStringLiteral("[task] unblocked %1").arg(unblocked.join(QStringLiteral(", ")));
    }
    *result = message;
    return true;
}

QString TaskStore::runCreateTask(const QJsonObject &args) const
{
    // 缺失参数在 JSON 边界优雅落到内核校验（subject 缺省 ''→ 'Task subject cannot be empty'，
    // 与 s05 todo 缺参族一致，登记偏差）
    const QString subject = args.value(QStringLiteral("subject")).toString();
    const QString description = args.value(QStringLiteral("description")).toString();
    Task task;
    QString error;
    if (!createTask(subject, description, &task, &error))
        return error; // lcc 裸抛 → lite 原样文案直返（不加 'Error:' 前缀，登记裁决）
    qDebug().noquote() << QStringLiteral("[task] create %1").arg(task.subject); // lcc run_create_task print
    return QStringLiteral("Created %1: %2").arg(task.id, task.subject);
}

QString TaskStore::runUpdateTask(const QJsonObject &args) const
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    const QJsonValue addValue = args.value(QStringLiteral("addBlockedBy"));
    if (!addValue.isArray()) {
        // lcc :95 在 load 之前的 isinstance(list) 检查，文案逐字；缺参 → 同文案（族一致）
        return QStringLiteral("addBlockedBy must be a list of task IDs");
    }
    Task updated;
    QString error;
    if (!updateTaskDependencies(taskId, addValue.toArray(), &updated, &error))
        return error;
    QString dependencies = updated.blockedBy.join(QStringLiteral(", "));
    if (dependencies.isEmpty())
        dependencies = QStringLiteral("(none)");
    qDebug().noquote() << QStringLiteral("[task] update %1 blockedBy: %2").arg(updated.subject, dependencies);
    return QStringLiteral("Updated %1 blockedBy: %2").arg(updated.id, dependencies);
}

QString TaskStore::runListTasks() const
{
    QVector<Task> tasks;
    QString error;
    if (!listTasks(&tasks, &error))
        return error;
    if (tasks.isEmpty())
        return QStringLiteral("No tasks. Use create_task to add some.");
    QStringList lines;
    lines.reserve(tasks.size());
    for (const Task &task : tasks) {
        QString marker;
        if (task.status == QStringLiteral("pending"))
            marker = QStringLiteral("[ ]");
        else if (task.status == QStringLiteral("in_progress"))
            marker = QStringLiteral("[>]");
        else if (task.status == QStringLiteral("completed"))
            marker = QStringLiteral("[x]");
        else
            marker = QStringLiteral("[?]"); // .get(status, "[?]") 缺省形态（load 已约束枚举，lcc 同为死路径保留）
        const QString owner = task.owner.isEmpty()
            ? QString()
            : QStringLiteral(" [%1]").arg(task.owner); // python 真值判断：空串 owner 亦不显示
        const QString dependencies = task.blockedBy.isEmpty()
            ? QString()
            : QStringLiteral(" (blockedBy: %1)").arg(task.blockedBy.join(QStringLiteral(", ")));
        lines << QStringLiteral("%1 %2: %3 [%4]%5%6")
                     .arg(marker, task.id, task.subject, task.status, owner, dependencies);
    }
    return lines.join(QLatin1Char('\n'));
}

QString TaskStore::runGetTask(const QJsonObject &args) const
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    Task task;
    QString error;
    if (!loadTask(taskId, &task, &error))
        return error;
    return taskToJsonText(task); // lcc get_task：json.dumps(asdict, indent=2) 透传
}

QString TaskStore::runClaimTask(const QJsonObject &args) const
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!claimTask(taskId, QStringLiteral("agent"), &result, &error)) // owner 硬编码 lcc run 层 'agent'
        return error;
    return result;
}

QString TaskStore::runCompleteTask(const QJsonObject &args) const
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!completeTask(taskId, QStringLiteral("agent"), &result, &error))
        return error;
    return result;
}
