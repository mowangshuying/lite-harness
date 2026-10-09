#include "MessageBus.h"

#include "AgentConstants.h" // 邮箱目录名单源（kMailboxesDirName）
#include "AgentPathGuard.h"  // 名字正则与路径包含守护单源（gate① M7：从本文件匿名 ns 提出，供 Lane B/C/D 复用）

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

// ============================================================================
// lcc s13 34775c8 message_bus.py 移植。一人一个 .jsonl 邮箱，追加写 + 破坏性读取。
// 线程安全在 lcc 靠 RLock + Condition（多进程人类 CLI）；lite 单宿主进程、零线程，
// 本类全同步 IO，锁与 wait_for_messages（阻塞条件等待）不移植——唤醒由宿主 QTimer
// 轮询 hasPending 门铃实现（见 D1/D9）。路径校验逐字对齐 _path 三关。
// ============================================================================

MessageBus::MessageBus(std::function<QString()> sessionRootSink)
    : m_sessionRootSink(std::move(sessionRootSink))
{
}

QString MessageBus::mailboxesDir() const
{
    // lcc env.py mailboxesDirPath = workspace / ".lcc/mailboxes"；lite 收敛进会话根下
    // .mailboxes 叶子（.lite-harness 中间层由 sessionRootSink 提供），单源常量防拼法漂移。
    return QDir(m_sessionRootSink()).filePath(AgentConst::kMailboxesDirName);
}

QString MessageBus::lastError() const
{
    return m_lastError;
}

bool MessageBus::resolveMailboxPath(const QString &name, QString *path, QString *error) const
{
    // 三关 fail-closed（宁报错不猜，照 lcc _path :38-50）。
    // 名字正则与路径包含 helper 单源于 AgentPathGuard.h（gate① M7 提出，供 Lane B/C/D 复用）。
    // m1 已落地（Gate① minor 遗留 → Gate② 前置③ → 本轮 Gate③ P4 收口）：词法关
    // 保留（不存在目标无 canonical 可解析，lcc message_bus.py:38-47 Path.resolve
    // (strict=False) 同款两口径），目标存在时追加 canonical 复校——junction/符号链接
    // 把 .mailboxes 或邮箱文件指向根外时，纯词法 indexOf 放行而 OS 实际写出界外。
    // 同款接法照 WorktreeManager.cpp worktreePath 的 m1 先例（喂 canonicalFilePath
    // 作 child 匹配 helper 的 canonical parent，防盘符大小写假拒；断链 canonical
    // 为空 → helper fail-closed）。大小写口径不改（维持现词法大小写敏感，
    // WorktreeManager 的 CI 注册表是它自己的 FIND-C 裁决，不跨界）。
    // ① 收件人名正则（VALID_AGENT_NAME fullmatch）
    if (!AgentPathGuard::isValidAgentName(name)) {
        *error = QStringLiteral("MessageBus: invalid mailbox name '%1' (must match [A-Za-z0-9_-]{1,64})")
                     .arg(name);
        return false;
    }
    // ② 邮箱目录必须在会话根内
    const QString cleanRoot = QDir::cleanPath(m_sessionRootSink());
    const QString cleanDir = QDir::cleanPath(mailboxesDir());
    if (!AgentPathGuard::isWithinPath(cleanDir, cleanRoot)) {
        *error = QStringLiteral("MessageBus: mailbox directory escapes session root");
        return false;
    }
    // m1 复校：目录存在（send 曾 mkpath 过/外部预建）时按 canonical 实测口径再验一次
    const QFileInfo dirInfo(cleanDir);
    if (dirInfo.exists()
        && !AgentPathGuard::isWithinPathCanonical(dirInfo.canonicalFilePath(), cleanRoot)) {
        *error = QStringLiteral("MessageBus: mailbox directory escapes session root");
        return false;
    }
    // ③ 解析后的文件路径必须仍在邮箱根下
    const QString file = QDir::cleanPath(cleanDir + QLatin1Char('/') + name + QStringLiteral(".jsonl"));
    if (!AgentPathGuard::isWithinPath(file, cleanDir)) {
        *error = QStringLiteral("MessageBus: mailbox path escapes directory '%1'").arg(name);
        return false;
    }
    // m1 复校：邮箱文件已存在（doorbell/历史消息）时同上走 canonical（父级 cleanDir
    // 已由 helper 内部 canonical 化，若父级本身是指向界外的 junction，
    // isWithinPathCanonical 在②号关已被拦截——此关防的是文件级链接，纵深防御）
    const QFileInfo fileInfo(file);
    if (fileInfo.exists()
        && !AgentPathGuard::isWithinPathCanonical(fileInfo.canonicalFilePath(), cleanDir)) {
        *error = QStringLiteral("MessageBus: mailbox path escapes directory '%1'").arg(name);
        return false;
    }
    *path = file;
    return true;
}

bool MessageBus::send(const QString &from, const QString &to, const QString &content,
                      const QString &type, const QJsonObject &metadata)
{
    m_lastError.clear();

    QString path;
    if (!resolveMailboxPath(to, &path, &m_lastError))
        return false;

    // lcc send 每次 mkdir(parents=True, exist_ok=True)：检查便宜，换「任何时候可用」
    QDir().mkpath(mailboxesDir());

    // 六字段信封。ts = 浮点秒（D3，time.time() 口径）。
    // 偏差登记：lcc json.dumps(ensure_ascii=True) 输出纯 ASCII；Qt 原样 UTF-8——两侧均为合法
    // JSON、可互解，仅观感差异（Windows 再包一层保险在 lite 单宿主下无必要）。键序 Qt 排序输出，
    // 解析按键名，语义等价。
    QJsonObject envelope;
    envelope[QStringLiteral("from")] = from;
    envelope[QStringLiteral("to")] = to;
    envelope[QStringLiteral("content")] = content;
    envelope[QStringLiteral("type")] = type;
    envelope[QStringLiteral("ts")] = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    envelope[QStringLiteral("metadata")] = metadata;

    QByteArray line = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    // toJson 的尾换行随 Qt 版本而变，剥净后自补单 LF，确保「一行一封」（读侧按 '\n' 拆行、
    // trim 容忍 CRLF）。lcc Windows text 模式会写 CRLF，读侧 splitlines 亦容忍，无碍互操作。
    while (line.endsWith('\n') || line.endsWith('\r'))
        line.chop(1);
    line += '\n';

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_lastError = QStringLiteral("MessageBus: cannot open mailbox for append: %1 (%2)")
                          .arg(path, file.errorString());
        return false;
    }
    if (file.write(line) != line.size()) {
        m_lastError = QStringLiteral("MessageBus: mailbox append write failed: %1 (%2)")
                          .arg(path, file.errorString());
        file.close();
        return false;
    }
    file.close();
    return true;
}

bool MessageBus::hasPending(const QString &name) const
{
    m_lastError.clear();

    QString path;
    if (!resolveMailboxPath(name, &path, &m_lastError))
        return false;

    // lcc peek: exists && st_size>0（查尺寸防 0 字节崩溃残留活锁门铃）
    const QFileInfo info(path);
    return info.exists() && info.size() > 0;
}

QVector<BusMessage> MessageBus::drain(const QString &name)
{
    m_lastError.clear();
    QVector<BusMessage> out;

    QString path;
    if (!resolveMailboxPath(name, &path, &m_lastError))
        return out;

    QFile file(path);
    if (!file.exists())
        return out; // 无文件 = 无信（lcc 返回 []，非异常，不置 lastError）

    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("MessageBus: cannot open mailbox for read: %1 (%2)")
                          .arg(path, file.errorString());
        return out;
    }
    const QByteArray data = file.readAll();
    file.close();

    // 破坏性读取：读文件 + unlink 一步走（at-most-once，无 ack）。取走即清空整个 jsonl。
    // M8 编排者裁决=① fail-closed（对齐 lcc message_bus.py:52-59：unlink 失败异常上抛→本批
    // 不投递、信箱保留、下次重试）：remove 失败 → 置 lastError 并返回空批，已解析批丢弃、
    // 信箱原样保留，靠宿主心跳下一拍重试投递——协议层投递次数是引擎职责，至多一次由引擎
    // 兜底，不外包装给调用方。
    if (!file.remove()) {
        m_lastError = QStringLiteral("MessageBus: mailbox unlink failed: %1 (%2)")
                          .arg(path, file.errorString());
        return QVector<BusMessage>();
    }

    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            continue;

        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &perr);
        // D9-defensive（有意偏离 lcc：python json.loads 遇畸形行 raise → 卡死整个邮箱）。
        // 非法 JSON / 非对象 / 缺任一信封键，一律跳过该行，保合法行送达、防毒邮箱炸账。
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject envelope = doc.object();
        if (!envelope.contains(QStringLiteral("from")) || !envelope.contains(QStringLiteral("to")) ||
            !envelope.contains(QStringLiteral("content")) || !envelope.contains(QStringLiteral("type")) ||
            !envelope.contains(QStringLiteral("ts")) || !envelope.contains(QStringLiteral("metadata")))
            continue;

        BusMessage msg;
        msg.from = envelope.value(QStringLiteral("from")).toString();
        msg.to = envelope.value(QStringLiteral("to")).toString();
        msg.content = envelope.value(QStringLiteral("content")).toString();
        msg.type = envelope.value(QStringLiteral("type")).toString();
        msg.ts = envelope.value(QStringLiteral("ts")).toDouble();
        msg.metadata = envelope.value(QStringLiteral("metadata")).toObject();
        out.append(msg);
    }

    return out;
}
