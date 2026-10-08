#include "MessageBus.h"

#include "AgentConstants.h" // 邮箱目录名单源（kMailboxesDirName）

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

// ============================================================================
// lcc s13 34775c8 message_bus.py 移植。一人一个 .jsonl 邮箱，追加写 + 破坏性读取。
// 线程安全在 lcc 靠 RLock + Condition（多进程人类 CLI）；lite 单宿主进程、零线程，
// 本类全同步 IO，锁与 wait_for_messages（阻塞条件等待）不移植——唤醒由宿主 QTimer
// 轮询 hasPending 门铃实现（见 D1/D9）。路径校验逐字对齐 _path 三关。
// ============================================================================

namespace {

// lcc VALID_AGENT_NAME（message_bus.py:14）：fullmatch 防 "abc/../evil" 前缀合法后缀越狱。
// 字符集不含 '.' 与 '/'，故 '.'/'..'/'a/b' 一律在①关即拒。
bool isValidAgentName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    const QRegularExpressionMatch m = re.match(name);
    // python fullmatch 等价：锚定命中 + 捕获段恰覆盖全串（规避 PCRE $ 允许末尾换行的怪癖，
    // 与 isTaskIdFull 同款纪律）
    return m.hasMatch() && m.capturedStart(0) == 0 && m.capturedLength(0) == name.size();
}

// python Path.is_relative_to 的词法等价：入参均已 QDir::cleanPath 归一（正斜杠、无冗余段）。
// 用前缀比较而非 canonicalFilePath——后者要求文件存在且解析符号链接，而 send 时邮箱/目录尚未
// 落地。名字正则已禁路径分隔符，sessionRoot 为宿主可信值，词法包含校验足以拦越狱（登记偏差：
// lcc resolve() 另做符号链接归一，此处仅词法，纵深防御仍覆盖任务书三重校验）。
bool isWithinPath(const QString &child, const QString &parent)
{
    if (child == parent)
        return true;
    if (parent.endsWith(QLatin1Char('/')))
        return child.startsWith(parent);
    return child.startsWith(parent + QLatin1Char('/'));
}

} // namespace

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
    // 三关 fail-closed（宁报错不猜，照 lcc _path :38-50）
    // ① 收件人名正则（VALID_AGENT_NAME fullmatch）
    if (!isValidAgentName(name)) {
        *error = QStringLiteral("MessageBus: invalid mailbox name '%1' (must match [A-Za-z0-9_-]{1,64})")
                     .arg(name);
        return false;
    }
    // ② 邮箱目录必须在会话根内
    const QString cleanRoot = QDir::cleanPath(m_sessionRootSink());
    const QString cleanDir = QDir::cleanPath(mailboxesDir());
    if (!isWithinPath(cleanDir, cleanRoot)) {
        *error = QStringLiteral("MessageBus: mailbox directory escapes session root");
        return false;
    }
    // ③ 解析后的文件路径必须仍在邮箱根下
    const QString file = QDir::cleanPath(cleanDir + QLatin1Char('/') + name + QStringLiteral(".jsonl"));
    if (!isWithinPath(file, cleanDir)) {
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
    if (!file.remove()) {
        // 偏差：lcc unlink 失败会 raise；lite 已把消息取到内存（投递事实成立），仅在 lastError
        // 记账，不回滚。正常临时目录可写，此路罕见。
        m_lastError = QStringLiteral("MessageBus: mailbox unlink failed: %1 (%2)")
                          .arg(path, file.errorString());
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
