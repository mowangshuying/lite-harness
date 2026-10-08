#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

/**
 * MessageBus —— 团队消息总线（lcc s13 34775c8 message_bus.py 移植）
 *
 * 一人一个 JSONL 邮箱文件（`<会话根>/.mailboxes/<name>.jsonl`），追加写 + 破坏性读取：
 * 文件在=有信、文件没了=已读，天然「至多一次投递」（无游标、无 ack）。文件当邮箱 = 持久、
 * 可现场查验、崩了重启账还在。本类是纯同步文件 IO 的引擎类（与 TaskStore 同口径：零线程、
 * 无信号槽、无 GUI 依赖、QtCore-only），实例由宿主创建并注入各协作者，不建模块级单例。
 *
 * sessionRootSink 惰性取宿主会话数据根（仿 TaskStore/CronSchedulerManager 注入法）：
 * 每次调用现取，不缓存根路径——setWorkDir 切根后自然生效。落盘根 = sessionRoot 直下
 * 拼叶子段 AgentConst::kMailboxesDirName（.lite-harness 中间层由宿主 sink 统一提供）。
 *（lite 有意偏差：lcc 邮箱落 workDir 直下 `.lcc/mailboxes`，lite 收敛进会话根——
 *  同 TaskStore `.task` 的既有登记偏差先例，见 D2。）
 *
 * 失败折叠纪律（对齐 TaskStore）：内核 send/drain/hasPending 不把 lcc 的 raise 直接抛出，
 * 三重路径校验或 IO 失败一律返回 false / 空并置 lastError()（折叠串供上层当工具输出）。
 *
 * 保留名判定（lead/agent）不属本类职责：lcc message_bus 仅暴露常量供 Lane B/C/D 复用，
 * 真正的队友名合规在 AgentTeamsManager 侧裁决，本类只做路径 fail-closed。
 */
struct BusMessage
{
    // 六字段信封 {from,to,content,type,ts,metadata}：type 是协议路由键，metadata 带
    // request_id 等对账凭据。声明序即信封语义序（落盘为 JSON 对象，键序由 Qt 排序输出，
    // 解析按键名取值，语义等价——登记偏差：lcc json.dumps 保插入序）。
    QString from;
    QString to;
    QString content;
    QString type;
    double ts = 0.0;        // D3：浮点秒（time.time() 口径），非字符串；落盘为 JSON number
    QJsonObject metadata;
};

class MessageBus
{
public:
    // sessionRootSink 惰性取宿主会话数据根（含 .lite-harness 中间层/会话段）
    explicit MessageBus(std::function<QString()> sessionRootSink);

    // 追加一封：在线性信封里打 ts（QDateTime::currentMSecsSinceEpoch()/1000.0），单行单消息，
    // O_APPEND 语义（QIODevice::Append）落 <to>.jsonl，顺手 mkdir（lcc 每次 send 白捡检查）。
    // type 全集 9 种（message / plan_approval_request / plan_approval_response /
    // shutdown_request / shutdown_response / plan_request / result / idle_notification / error）
    // 由调用方传字符串，本类不校验取值。失败返回 false 并经 lastError() 给折叠串。
    bool send(const QString &from, const QString &to, const QString &content,
              const QString &type = QStringLiteral("message"),
              const QJsonObject &metadata = {});

    // peek（门铃，不消费）：文件存在且 size>0——查尺寸防「0 字节崩溃残留」把等待循环活锁。
    // 校验不过返回 false 并置 lastError()。
    bool hasPending(const QString &name) const;

    // 破坏性整读（read）：读后即 unlink 整个 jsonl（at-most-once，无 ack）。不存在返回空且不记错误
    //（lcc read 语义：无文件=无信，非异常）。畸形行（非法 JSON / 缺键）跳过不炸账——
    // D9-defensive，有意偏离 lcc（python json.loads 会 raise 卡死邮箱，我们防毒邮箱）。
    QVector<BusMessage> drain(const QString &name);

    // 最近一次失败的折叠串（成功/无操作后为空）
    QString lastError() const;

private:
    // 邮箱目录 = sessionRoot/.mailboxes（叶子段单源于 AgentConst::kMailboxesDirName）
    QString mailboxesDir() const;

    // fail-closed 三关（照 lcc _path :38-50，宁拒不猜）：① 收件人名 fullmatch
    // `^[A-Za-z0-9_-]{1,64}$`（防 "abc/../evil" 前缀合法后缀越狱）；② 邮箱目录必须在
    // sessionRoot 内；③ 解析后的文件路径必须仍在邮箱目录下。任一不过返回 false 经 *error 出参。
    // 成功时 *path = 归一化后的 <mailboxes>/<name>.jsonl 绝对串。
    bool resolveMailboxPath(const QString &name, QString *path, QString *error) const;

    std::function<QString()> m_sessionRootSink; // 宿主会话数据根惰性获取（见类头注释）
    mutable QString m_lastError;                // 失败折叠串（hasPending 为 const 亦可置错，故 mutable）
};
