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

#include <algorithm> // s13：leasesPointingAt 的 owner 字典序输出（std::sort）

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

TaskStore::TaskStore(std::function<QString()> sessionRootSink, std::function<QString()> workDirSink)
    : m_sessionRootSink(std::move(sessionRootSink)), m_workDirSink(std::move(workDirSink))
{
}
// s13 注：workDirSink 带默认值 nullptr（向后兼容铁律①）——AgentLoop.cpp 单 lambda 构造点
// 零改动可编译；P3 装配根补传宿主工作目录，供租约 cwd 回落链使用。

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
    // id/subject/description/status/owner/timestamp/blockedBy/worktree 顺序输出（偏差登记见 jsonCompactLiteral 注释；
    // worktree 为 s13 新增末键，lcc 34775c8 dataclass 声明序同款，空串落 null——向后兼容铁律④）
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
        lines << QStringLiteral("  \"blockedBy\": [],"); // s13：worktree 成为末键，本行补尾逗号
    } else {
        QStringList items;
        items.reserve(task.blockedBy.size());
        for (const QString &dep : task.blockedBy)
            items << QStringLiteral("    %1").arg(jsonStringLiteral(dep));
        lines << QStringLiteral("  \"blockedBy\": [");
        lines << items.join(QStringLiteral(",\n"));
        lines << QStringLiteral("  ],"); // s13：同上，尾逗号
    }
    lines << QStringLiteral("  \"worktree\": %1")
                 .arg(task.worktree.isEmpty() ? QStringLiteral("null") : jsonStringLiteral(task.worktree));
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
        // lite 对存量旧任务文件（6 键、无 timestamp）容错取 0.0、不判 Invalid。
        // worktree（lcc s13 34775c8，dataclass 末字段默认 None）同款容错：缺键视为 null
        //（向后兼容铁律③——旧任务文件照常可读）。键数公式随之泛化：
        // 六个必存键 + timestamp/worktree 两个各自可选的键；多一个杂键即判 Invalid（保持
        // lite 原"多键从严"纪律）；timestamp 键存在时类型必须为数值、worktree 键存在时必须为
        // null|string，否则归入同族 Invalid。
        const bool hasTs = obj.contains(QStringLiteral("timestamp"));
        const bool hasWt = obj.contains(QStringLiteral("worktree"));
        shapeOk = (obj.size() == 6 + (hasTs ? 1 : 0) + (hasWt ? 1 : 0))
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
            && (!hasTs || obj.value(QStringLiteral("timestamp")).isDouble())
            && (!hasWt || obj.value(QStringLiteral("worktree")).isNull()
                || obj.value(QStringLiteral("worktree")).isString());
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
            // worktree：缺键或 null → 空串（= 未绑定，在主工作目录干活；偏差⑦单态化，
            // 判真语义 !isEmpty() 与 lcc `if task.worktree` 一致）；有键取 string
            parsed.worktree = obj.value(QStringLiteral("worktree")).toString();
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

bool TaskStore::claimTaskUnleased(const QString &taskId, const QString &owner, QString *result,
                                  QString *error) const
{
    // lcc s10 claim_task :176-190（s10 无租约语义）：业务性失败（状态不符/被阻塞）是返回文本而非异常 → 走 *result
    // s13 改名登记（铁律②）：lcc s10 原内核更名 claimTask→claimTaskUnleased，行为与文案逐字不变；
    // s13 的带租约 claimTask（六门）另见本文件尾部 s13 扩展段。
    // 成功文案 'Claimed <id> <subject>' 无括号——s10 口径，与 s13 带租约版 'Claimed <id> (<subject>)' 有意不同（勿"统一"）。
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

bool TaskStore::completeTaskUnleased(const QString &taskId, const QString &owner, QString *result,
                                     QString *error) const
{
    // lcc s10 complete_task :194-222（s10 无租约/无计划门语义）
    // s13 改名登记（铁律②）：lcc s10 原内核更名 completeTask→completeTaskUnleased，行为与文案逐字不变；
    // s13 的带租约 completeTask（planGateCheck 否决 + 租约自愈 + 故意不释放）另见本文件尾部 s13 扩展段。
    // owner 失配文案 'Task %1 is owned by %2, not %3' 无 s13 后缀 '; cannot complete'——s10 口径，勿"统一"。
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
        return QStringLiteral("Error: ") + error; // lcc 裸抛 → lite 折叠为错误文本；'Error: ' 前缀对齐全仓失败文案族（B1 成败判定单源）
    qDebug().noquote() << QStringLiteral("[task] create %1").arg(task.subject); // lcc run_create_task print
    return QStringLiteral("Created %1: %2").arg(task.id, task.subject);
}

QString TaskStore::runUpdateTask(const QJsonObject &args) const
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    const QJsonValue addValue = args.value(QStringLiteral("addBlockedBy"));
    if (!addValue.isArray()) {
        // lcc :95 在 load 之前的 isinstance(list) 检查，文案在 lcc 原文基础上加 'Error: ' 前缀
        //（B1 成败判定单源）；缺参 → 同文案（族一致）
        return QStringLiteral("Error: addBlockedBy must be a list of task IDs");
    }
    Task updated;
    QString error;
    if (!updateTaskDependencies(taskId, addValue.toArray(), &updated, &error))
        return QStringLiteral("Error: ") + error;
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
        return QStringLiteral("Error: ") + error;
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
        return QStringLiteral("Error: ") + error;
    return taskToJsonText(task); // lcc get_task：json.dumps(asdict, indent=2) 透传
}

QString TaskStore::runClaimTask(const QJsonObject &args) const
{
    // 铁律（向后兼容）：单代理工具的 claim 走 s10 无租约内核，行为与事故前逐字一致；
    // s13 带租约通道是另一对 runClaimTaskLeased/runCompleteTaskLeased（P3 团队工具接线用）。
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!claimTaskUnleased(taskId, QStringLiteral("agent"), &result, &error)) // owner 硬编码 lcc run 层 'agent'
        return QStringLiteral("Error: ") + error;
    return result;
}

QString TaskStore::runCompleteTask(const QJsonObject &args) const
{
    // 铁律（向后兼容）：同 runClaimTask，走 s10 无租约内核，文案逐字不变。
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!completeTaskUnleased(taskId, QStringLiteral("agent"), &result, &error))
        return QStringLiteral("Error: ") + error;
    return result;
}

// ============================================================================
// s13 Lane A：租约台账 + worktree 绑定（lcc s13 34775c8 task_manager.py 移植）
// claim 六门 :312-349 / complete（故意不释放租约）:354-396 /
// release_completed_assignment :400-414 / release_teammate_assignment :419-431 /
// advance_assignment_version :136-142 / _task_cwd :147-156 / _owner_in_progress :159-161。
// 偏差登记全集见 TaskStore.h 类头（D9：lcc :96-131 的 fcntl/msvcrt 跨进程文件锁服务于多进程
// 人类 CLI，lite 单宿主进程不移植；D7：台账纯内存，进程重启即作废=fail-closed）。
// ============================================================================

bool TaskStore::setWorktree(const QString &taskId, const QString &worktreeName, QString *error) const
{
    // s13 worktree 绑定的宿主侧入口（P2 WorktreeManager 创建成功路径将来调用；解绑=clearWorktree）。
    // 有意偏离 lcc：lcc 把绑定动作内嵌于 create_worktree/claim 流程，lite 暴露独立内核 API 供 P2/P3 接线。
    // 不设状态门（C3 钉桩：in_progress/completed 均可绑）——worktreeName 合法性（正则命名约束、
    // 在册校验）是 P2 职责（lcc worktree_manager.py:304 存 name），本处只负责持久化（load→set→save）。
    // gate① M1：形参原误名 path——本字段存的是 worktree 名字，name→路径推导归 P2::worktreePath。
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    task.worktree = worktreeName;
    return saveTask(task, error);
}

bool TaskStore::clearWorktree(const QString &taskId, QString *error) const
{
    // 解绑即置 null（lcc 语义 worktree=None ≡ 在宿主主工作目录干活；lite 单态形=空串，偏差⑦前半）。
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    task.worktree.clear();
    return saveTask(task, error);
}

bool TaskStore::resolveTaskCwd(const Task &task, QString *cwd, QString *error) const
{
    // lcc _task_cwd :147-156 的 lite 收敛形（偏差⑦后半：worktree_validator 折进 resolver 错误通道）。
    // gate① M4 早退（lcc :147 `if task.worktree:` 真值判断形）：未绑定任务不进 resolver，
    // 直接走回落链——「未绑定不起子进程」是热路径性能语义（P2 的 resolver 背后是 git 命令，
    // 每个未绑任务都调用 = 每 claim 白跑一次 rev-parse）。实参给快照（P2 解析须看 worktree 名字）。
    if (m_cwdResolver && !task.worktree.isEmpty()) {
        QString resolverError;
        const QString resolved = m_cwdResolver(makeSnapshot(task), &resolverError);
        if (!resolverError.isEmpty()) {
            // fail-closed：resolver 置错 = 不可解（如 worktree 绑定破损），错误原文上交调用方
            if (error)
                *error = resolverError;
            return false;
        }
        if (!resolved.isEmpty()) {
            if (cwd)
                *cwd = resolved;
            return true;
        }
        // 返回空串 = 本回调对该任务不解析（≡ lcc `if task.worktree` 真值判断不成立），走回落链
    }
    // 回落链：workDirSink（宿主工作目录，P3 补传）→ sessionRootSink（未传时的测试/独立形态，铁律①）
    if (cwd)
        *cwd = m_workDirSink ? m_workDirSink() : m_sessionRootSink();
    return true;
}

bool TaskStore::ownerInProgressTask(const QString &owner, Task *task, QString *error) const
{
    // lcc _owner_in_progress :159-161：取磁盘上第一条属于该 owner 的 in_progress 任务。
    // 三态返回（见头文件注释）：调用方必须区分“未找到”与“台账不可读”，后者须 fail-closed。
    QVector<Task> all;
    if (!listTasks(&all, error))
        return false; // 台账不可读 → *error 非空（lcc list() 崩溃上抛的 lite 折叠形）
    for (const Task &candidate : all) {
        if (candidate.status == QStringLiteral("in_progress") && candidate.owned
            && candidate.owner == owner) {
            if (task)
                *task = candidate;
            return true;
        }
    }
    if (error)
        error->clear(); // 未找到 ≠ 失败：清 error 区分三态
    return false;
}

int TaskStore::advanceAssignmentVersion(const QString &owner, const QString &taskId)
{
    // lcc advance_assignment_version :136-142：换工即 +1，令陈旧计划审批失效（P2 防 TOCTOU 消费）。
    // 偏差⑤：advanced 回调带 taskId（lcc 只传 owner，lite 宿主免二次反查）。
    const int version = m_assignmentVersions.value(owner, 0) + 1;
    m_assignmentVersions[owner] = version;
    if (m_onAssignmentAdvanced)
        m_onAssignmentAdvanced(owner, taskId);
    return version;
}

std::optional<TaskStore::Lease> TaskStore::leaseFor(const QString &owner) const
{
    const auto it = m_assignments.constFind(owner);
    if (it == m_assignments.constEnd())
        return std::nullopt;
    return *it;
}

int TaskStore::assignmentVersion(const QString &owner) const
{
    return m_assignmentVersions.value(owner, 0); // 无台账记录 = 0
}

bool TaskStore::claimTask(const QString &taskId, const QString &owner, QString *result, QString *error)
{
    // lcc s13 claim_task :312-349：六门依 lcc 顺序逐门检查；业务性失败经 *result 返回可判定文本
    //（return true，lcc 以 return 而非 raise 表达），读盘/校验失败经 *error（return false，run 层折叠）。
    Task task;
    if (!loadTask(taskId, &task, error)) // 门①：任务存在且可读可解析
        return false;
    if (task.status != QStringLiteral("pending")) {
        *result = QStringLiteral("Task %1 is %2, cannot claim").arg(taskId, task.status);
        return true;
    }
    if (task.owned) { // 门②：无主
        *result = QStringLiteral("Task %1 is already owned by %2").arg(taskId, task.owner);
        return true;
    }
    if (m_assignments.contains(owner)) { // 门③：内存租约互斥（同一 owner 一个回合只干一件活）
        *result = QStringLiteral(
            "Owner %1 must finish the current work turn for %2 before claiming another task")
                      .arg(owner, m_assignments.value(owner).taskId);
        return true;
    }
    // 门④：磁盘上无属于该 owner 的 in_progress 任务（跨重启/崩溃遗留检测——内存账清空后仍拦）
    Task current;
    QString inProgError;
    if (ownerInProgressTask(owner, &current, &inProgError)) {
        *result = QStringLiteral("Owner %1 must complete %2 before claiming another task")
                      .arg(owner, current.id);
        return true;
    }
    if (!inProgError.isEmpty()) {
        // 内核契约（本文件通篇纪律：bool=false 必带 error 文本，run 层 'Error: ' 折叠才有可判定输出；
        // lcc 同点位 list() 抛 ValueError 经 run_claim_task 折叠原文——error 通道透传是规格行为，
        // 重建重放时漏了这行，属重放缺陷非行为变更，事故前实测版此处即透传）
        if (error)
            *error = inProgError;
        return false; // fail-closed：台账不可读 ≠ 无在途（不得误判为可认领）
    }
    // 门⑤：blockedBy 依赖全部 completed（坏/缺失依赖计为未完——与 s10 同款容错方向，勿统一）
    const QStringList deps = incompleteDependencies(task);
    if (!deps.isEmpty()) {
        *result = QStringLiteral("Blocked by: %1").arg(pythonListRepr(deps));
        return true;
    }
    // 门⑥：cwd 可解（lcc _task_cwd；未绑定走 M4 早退直接回落；已绑定 resolver 置错即拒，
    // 任务保持 pending 不写盘、不建租约）
    QString cwd;
    QString cwdError;
    if (!resolveTaskCwd(task, &cwd, &cwdError)) {
        *result = QStringLiteral("Cannot claim %1: %2").arg(taskId, cwdError);
        return true;
    }
    task.owned = true;
    task.owner = owner;
    task.status = QStringLiteral("in_progress");
    if (!saveTask(task, error))
        return false;
    Lease lease;
    lease.taskId = taskId;
    lease.cwd = cwd;
    m_assignments.insert(owner, lease);
    advanceAssignmentVersion(owner, taskId);
    qDebug().noquote()
        << QStringLiteral("[task] claim %1 -> in_progress (lease owner: %2)").arg(task.subject, owner);
    // 偏差③承重契约：'Claimed ' 前缀被 lcc 跨模块 startswith 判成功（spawn/pull 通道）；
    // 本版本带括号是 s13 形（'Claimed <id> (<subject>)'），与遗留 s10 无括号形有意不同，勿"统一"。
    *result = QStringLiteral("Claimed %1 (%2)").arg(task.id, task.subject);
    return true;
}

bool TaskStore::completeTask(const QString &taskId, const QString &owner, QString *result, QString *error)
{
    // lcc s13 complete_task :354-396。门序照 lcc：状态 → owner → 计划门 → 租约自愈 → 记账。
    // 铁律（lcc :354 注释语义）：完成**故意不释放租约、不递增版本**——同回合后续工具仍需租约 cwd 路由，
    // 释放只发生在两个回合边界 API（releaseCompletedAssignment / releaseTeammateAssignment）。
    Task task;
    if (!loadTask(taskId, &task, error))
        return false;
    if (task.status != QStringLiteral("in_progress")) {
        *result = QStringLiteral("Task %1 is %2, cannot complete").arg(taskId, task.status);
        return true;
    }
    if (!task.owned || task.owner != owner) {
        // lcc s13 :358 文案带后缀 '; cannot complete'（s10 遗留版无——有意不同，勿统一）；
        // 未认领时 python None 呈现 'None'（与遗留版同款修正：Task.owner 类属性笔误 → 实例值）
        *result = QStringLiteral("Task %1 is owned by %2, not %3; cannot complete")
                      .arg(taskId, task.owned ? task.owner : QStringLiteral("None"), owner);
        return true;
    }
    if (m_planGateCheck) { // 计划审批否决（lcc plan_gate_check；偏差⑥·M5：(owner, taskId) 双值判定）
        QString reason;
        if (!m_planGateCheck(owner, taskId, &reason)) {
            *result = reason; // 可为空串=静默否决（lcc 空串拒口径同款）；任务保持 in_progress 不动
            return true;
        }
    }
    if (!m_assignments.contains(owner) || m_assignments.value(owner).taskId != taskId) {
        // 租约自愈（lcc :370-375）：台账缺失或指向他任务（如遗留通道认领的任务走本内核完成）时重建。
        // 有意偏离 lcc：自愈不递增版本、不触发 advanced 回调——版本推进=真实换工，自愈不算（P2 防 TOCTOU 语义纯净）。
        QString healedCwd;
        QString cwdError;
        if (!resolveTaskCwd(task, &healedCwd, &cwdError)) {
            *result = QStringLiteral("Task %1 cannot complete: %2").arg(taskId, cwdError);
            return true; // 业务通道：可判定文本；任务保持 in_progress、租约保持缺失
        }
        Lease lease;
        lease.taskId = taskId;
        lease.cwd = healedCwd;
        m_assignments[owner] = lease;
    }
    // ready_before：完成前已“可开始”的 pending 带依赖任务 id 集合（lcc :381-386，can_start 逐个重载）
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
    // unblocked：完成前不可开始、现在可开始的 → 收集 subject（lcc :391-395）
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
    return true; // 铁律：此处不 remove 租约、不 advanceAssignmentVersion
}

bool TaskStore::releaseCompletedAssignment(const QString &owner, QString *error)
{
    // lcc release_completed_assignment :400-414（回合边界，Lead 侧同款）：租约指向的任务确已
    // completed 且属于该 owner 才释放；否则一律不动台账（幂等：重复释放=“无租约”分支）。
    // 偏差④：lcc :411 释放时也 advance_assignment_version + 触发 advanced 回调，
    // lite 裁决只在 claim 换工时递增（一轮 claim→complete→release→reclaim 递增两次会扰乱
    // P2 陈旧审批判定），已登记（TaskStore.h 类头偏差④）。
    if (!m_assignments.contains(owner)) {
        if (error)
            *error = QStringLiteral("Owner %1 has no active assignment").arg(owner);
        return false;
    }
    const QString leasedTaskId = m_assignments.value(owner).taskId;
    Task task;
    if (!loadTask(leasedTaskId, &task, error))
        return false; // fail-closed：租约任务不可读则保留租约，交调用方决策
    if (task.status != QStringLiteral("completed") || !task.owned || task.owner != owner) {
        if (error)
            *error = QStringLiteral("Owner %1 assignment for task %2 is not completed; nothing released")
                         .arg(owner, leasedTaskId);
        return false; // 未达释放条件：租约保持（同回合 continue 干下一件活的语义归 claim 门③保障）
    }
    m_assignments.remove(owner);
    if (m_onAssignmentReleased)
        m_onAssignmentReleased(owner);
    return true;
}

bool TaskStore::releaseTeammateAssignment(const QString &owner, QString *error)
{
    // lcc release_teammate_assignment :419-431（队友死亡/退出清理）：遗留 in_progress 降级
    // pending/清 owner；内存台账清理**无条件**执行（lcc try/finally 语义——死 owner 若不无条件
    // 清租约，其名字被永久占用）。ghost owner（无租约无任务）同样走完成功路径且仍触发 released
    // 回调（lcc 无条件 finally 同款，幂等清理）。
    QString diskError;
    Task current;
    QString probeError;
    if (ownerInProgressTask(owner, &current, &probeError)) {
        current.status = QStringLiteral("pending");
        current.owned = false;
        current.owner.clear();
        QString saveError;
        if (!saveTask(current, &saveError))
            diskError = saveError; // 磁盘清理失败：内存清理照跑，最终如实报 false
    } else if (!probeError.isEmpty()) {
        diskError = probeError; // 台账不可读：同样放行内存清理，如实报 false
    }
    // finally 段（无条件）：清租约 + 通知（lcc :428-431——即使降级抛异常也要走完）。
    // 偏差④（gate① M6 复注）：lcc :429 此处也 advance_assignment_version，lite 两个释放点统一
    // 不递增、不触发 advanced——fix-4 的 plan gate 复位与 work_version 陈旧推进只挂
    // onAssignmentReleased（本函数与 releaseCompletedAssignment 的末尾通知即是唯一挂点）。
    m_assignments.remove(owner);
    if (m_onAssignmentReleased)
        m_onAssignmentReleased(owner);
    if (error)
        *error = diskError; // true 时恒空串；false 时=磁盘失败原因
    return diskError.isEmpty();
}

void TaskStore::setPlanGateCheck(
    std::function<bool(const QString &owner, const QString &taskId, QString *reason)> gate)
{
    m_planGateCheck = std::move(gate);
}

void TaskStore::setOnAssignmentAdvanced(std::function<void(const QString &owner, const QString &taskId)> cb)
{
    m_onAssignmentAdvanced = std::move(cb);
}

void TaskStore::setOnAssignmentReleased(std::function<void(const QString &owner)> cb)
{
    m_onAssignmentReleased = std::move(cb);
}

void TaskStore::setCwdResolver(
    std::function<QString(const TaskSnapshot &task, QString *error)> resolver)
{
    m_cwdResolver = std::move(resolver);
}

QString TaskStore::runClaimTaskLeased(const QJsonObject &args, const QString &owner)
{
    // P3 团队侧接线入口：Lead 传保留键 "agent"（类头说明），队友传邮箱名。
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!claimTask(taskId, owner, &result, &error))
        return QStringLiteral("Error: ") + error;
    return result; // 成功时以 'Claimed ' 开头——承重契约（偏差③），下游 startswith 判成功
}

QString TaskStore::runCompleteTaskLeased(const QJsonObject &args, const QString &owner)
{
    const QString taskId = args.value(QStringLiteral("task_id")).toString();
    QString result;
    QString error;
    if (!completeTask(taskId, owner, &result, &error))
        return QStringLiteral("Error: ") + error;
    return result;
}

// ============================================================================
// s13 Lane A（gate① 修复轮 R2 补齐）：跨模块快照视图 + 租约查询（lcc s13 34775c8
// task_manager.py list() :267-274 / scan_unclaimed_tasks :435-446；
// worktree_manager.py assignment_cwd :190-212 / remove 门④ :348-349）
// ============================================================================

TaskStore::TaskSnapshot TaskStore::makeSnapshot(const Task &task)
{
    // Task（私有嵌套）→ TaskSnapshot（公共跨模块形）的字段裁剪单源：
    // owned=false 折叠为空串 owner（lcc None 同款，偏差⑦单态口径——消费方判真用 !isEmpty()）
    TaskSnapshot snapshot;
    snapshot.id = task.id;
    snapshot.subject = task.subject;
    snapshot.status = task.status;
    snapshot.owner = task.owned ? task.owner : QString();
    snapshot.worktree = task.worktree;
    return snapshot;
}

bool TaskStore::listTaskSnapshots(QVector<TaskSnapshot> *snapshots, QString *error) const
{
    // gate① M3：程序化全量导出。错误口径对齐 lcc task_manager.py list() :267-274 现行为——
    // 内容损坏文件 load 崩直接上抛（lite 经 *error + false 折叠），不跳过；
    // 「文件名不合 ID 正则的脏文件跳过」沿用 listTasks 既有登记的 lite 防御偏差，同口径。
    QVector<Task> all;
    if (!listTasks(&all, error))
        return false;
    snapshots->clear();
    snapshots->reserve(all.size());
    for (const Task &task : all)
        snapshots->append(makeSnapshot(task));
    return true;
}

bool TaskStore::leasesPointingAt(const QString &dirPath, QStringList *owners) const
{
    // gate① M3：lcc worktree remove 门④（worktree_manager.py :348-349
    // `Path(a["cwd"]).resolve() == path.resolve()`）的 lite 词法形——两侧 QDir::cleanPath
    // 归一后比较；canonical 复校（符号链接/junction 绕行防御）由 fix-3 在消费点补做，
    // 此处不跑 git/QProcess（本类零外部进程纪律）。
    // 大小写不敏感比较（Windows 路径语义）：宁可多命中=多拒绝销毁，也不漏拦——fail-safe 方向。
    const QString normalized = QDir::cleanPath(dirPath);
    QStringList hits;
    for (auto it = m_assignments.constBegin(); it != m_assignments.constEnd(); ++it) {
        if (QDir::cleanPath(it->cwd).compare(normalized, Qt::CaseInsensitive) == 0)
            hits.append(it.key());
    }
    hits.sort(); // QHash 遍历序不定 → 字典序输出保可钉桩
    if (owners)
        *owners = hits;
    return !hits.isEmpty();
}

bool TaskStore::assignmentCwd(const QString &owner, QString *cwd, QString *error)
{
    // lcc worktree_manager.py assignment_cwd :190-212 热路径的 lite 形（gate① M2）：
    // 「内存租约当缓存、磁盘当真相」。分支序与 lcc 逐一对应。
    const auto it = m_assignments.constFind(owner);
    if (it == m_assignments.constEnd()) {
        if (owner == QStringLiteral("agent")) {
            // ① lcc :193-194：Lead 无租约 = 在主工作目录干活，回落链同款（workDirSink→sessionRootSink）
            if (cwd)
                *cwd = m_workDirSink ? m_workDirSink() : m_sessionRootSink();
            return true;
        }
        // ② lcc :196：队友无租约 = 无处路由，fail-closed（lcc raise，lite error 通道形）
        if (error)
            *error = QStringLiteral("No active assignment for %1").arg(owner);
        return false;
    }
    const Lease lease = *it;
    // ③ 现读盘校验（lcc :197-200）：租约任务不可读 → fail-closed 上抛
    Task task;
    if (!loadTask(lease.taskId, &task, error))
        return false;
    const bool statusActive = task.status == QStringLiteral("in_progress")
                              || task.status == QStringLiteral("completed");
    if (!statusActive || !task.owned || task.owner != owner) {
        // lcc :199-200：completed 放行配合「回合边界才退租」（同回合工具仍要 cwd 路由）；
        // 其余状态/归属不符 = 台账过期，任务已被外部改写
        if (error)
            *error = QStringLiteral("Assignment for %1 is no longer active").arg(owner);
        return false;
    }
    // lcc :201-207：绑定态经 task_worktree_cwd 解析、破损即错（lite 偏差⑦收敛进 resolver 的
    // *error 通道，"Worktree '<name>' binding is broken for task <id>" 文本由 fix-3 置入）；
    // 未绑定走 M4 早退回落。resolveTaskCwd 两态全覆盖，无需在此另判。
    QString resolved;
    QString resolveError;
    if (!resolveTaskCwd(task, &resolved, &resolveError)) {
        if (error)
            *error = resolveError;
        return false;
    }
    if (resolved != lease.cwd) {
        // lcc :210-211：计算值 ≠ 台账值 → 自愈回写。有意语义钉死（防 fix-4 误推）：
        // 不 advance_assignment_version、不触发 onAssignmentAdvanced——版本推进唯一挂点
        // 是 claim 换工（advanceAssignmentVersion），陈旧推进唯一挂点是 onAssignmentReleased。
        Lease healed = lease;
        healed.taskId = task.id;
        healed.cwd = resolved;
        m_assignments[owner] = healed;
    }
    if (cwd)
        *cwd = resolved;
    return true;
}

bool TaskStore::scanUnclaimedTasks(QVector<TaskSnapshot> *tasks, QString *error) const
{
    // lcc task_manager.py scan_unclaimed_tasks :435-446（移植自 s3 L1417-1428 的 lcc 注）：
    // 纯侦察只读——pending 且无主且依赖就绪且 cwd 可解 → 候选快照清单。不改任何状态，
    // 认领是下一步的事（届时走 claimTask 六门复验）。lcc 的 can_start(task.id) 逐条重载在
    // lite 用已读 task 直接算 incompleteDependencies（口径等价——canStart 体内即此调用）。
    tasks->clear();
    QVector<Task> all;
    if (!listTasks(&all, error))
        return false;
    for (const Task &task : all) {
        if (task.status != QStringLiteral("pending") || task.owned)
            continue;
        if (!incompleteDependencies(task).isEmpty())
            continue;
        QString cwd;
        QString cwdError;
        if (!resolveTaskCwd(task, &cwd, &cwdError))
            continue; // lcc `if not error` 同款：worktree 破损任务不进候选（侦察静默跳过，不报错）
        tasks->append(makeSnapshot(task));
    }
    return true;
}
