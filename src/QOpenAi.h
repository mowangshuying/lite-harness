#pragma once

#include <QObject>
#include <QJsonObject>

namespace QOpenAi {

class ChatStream : public QObject
{
    Q_OBJECT
public:
    explicit ChatStream(QObject *parent = nullptr);
    ~ChatStream() override;

    // 流处理模式：Chat 解析 choices[].delta；LegacyCompletion 解析 choices[].text
    enum class Mode
    {
        Chat,
        LegacyCompletion,
    };

    // 主动取消流（调用后不再 emit 任何信号）
    void cancel();

signals:
    // enable_thinking 推理增量
    void thinkingDelta(const QString &delta);
    // 正式回答增量
    void textDelta(const QString &delta);
    // 完整 assistant 消息（含 content / reasoning_content / tool_calls）
    void messageFinished(const QJsonObject &fullMessage);
    // 错误
    void error(const QString &message);

private:
    void startRequest(const QJsonObject &input, Mode mode = Mode::Chat);
    void sendRequest();
    void readIncoming();
    void processFrame(const QByteArray &frame);
    void finishStream();
    void scheduleRetry();
    void cleanupReply();

    friend class CategoryChat;
    friend class CategoryCompletion;

private:
    class Private;
    Private *d;
};

// ---- 等待期网关（BlockingGate / BlockingSession）----
//
// 为什么需要它：create()/completion().create() 的阻塞式请求内部用嵌套事件循环同步等
// LLM 响应（记忆召回/沉淀/整合、压缩摘要全走这条链，且都在 GUI 线程——本应用有意零线程）。
// 嵌套循环期间应用"看似活着"：用户可继续打字/点发送，触发第二条阻塞链与第一条交错，
// 多会话并发时更乱，且没有任何"系统正忙"的视觉信号。该网关把等待期显式化：
//   1) 0→1 边界设应用级等待光标、发 busyChanged(true)；1→0 边界反之；
//   2) UI 层（ChatMsgEdit）监听 busyChanged 禁用发送入口。
// 多会话共享同一 QOpenAi 客户端单例 ⇒ 任一会话进入等待期即"全局"禁发送——预期行为，
// 不是缺陷：目的就是禁止第二链交错，唯一解法（异步化）明确超出本期范围。
class BlockingGate : public QObject
{
    Q_OBJECT
public:
    // 当前是否处于等待期。UI 组件构造接线时直读一次做初值同步，
    // 防止 connect 之前等待期已开始而漏禁（双信号方案无法查询状态，故弃用）
    bool busy() const;

signals:
    // 状态信号而非 started/finished 双事件：消费方需要的是"现在忙不忙"这个状态，
    // 双信号迫使每个消费方自维护标志且有配对漂移风险。
    // 仅在重入计数 0↔1 边界发射：嵌套重入（嵌套循环事件派发中又触发的同步路径）
    // 的中间层退出不发 false，防止 UI 提前解禁
    void busyChanged(bool busy);

private:
    friend class BlockingSession;         // 唯一状态变更路径（RAII 守卫，实现见 QOpenAi.cpp）
    friend BlockingGate *blockingGate();  // 唯一单例访问点
    explicit BlockingGate(QObject *parent = nullptr);

    int m_depth = 0; // 重入计数，由 RAII 守卫严格增减对；>0 即 busy
};

// 等待期网关单例（函数局部 static，生命周期覆盖所有窗口部件）
BlockingGate *blockingGate();

// RAII 等待期守卫：blockingRequest 进入函数体（嵌套循环段）时构造，
// 任何退出路径（各早退 return / 正常 return / 异常）由作用域语义自动析构，
// 保证计数增减严格平衡、无需在每个 return 点手工恢复。
// 深度 0→1 附带推入应用级等待光标、1→0 弹出——光标与 busy 信号共用同一计数，
// 视觉等待与发送解禁始终同步。
class BlockingSession
{
public:
    BlockingSession();
    ~BlockingSession();
    BlockingSession(const BlockingSession &) = delete;
    BlockingSession &operator=(const BlockingSession &) = delete;
};

class CategoryChat
{
public:
    // 发起一次流式聊天请求，返回一个可监听信号的 ChatStream（parent 为其父对象）
    ChatStream *createStream(const QJsonObject &input, QObject *parent);

    // 阻塞式请求。必须在 QNetworkAccessManager 所在线程（通常为主线程）调用，
    // 内部使用嵌套事件循环，GUI 信号照常派发。
    // 非流式请求，返回完整响应 JSON；出错时返回包含 "error" 字段的 JSON
    QJsonObject create(const QJsonObject &input);
};

class CategoryCompletion
{
public:
    // 发起一次流式 legacy 补全请求（/completions），返回一个可监听信号的 ChatStream
    ChatStream *createStream(const QJsonObject &input, QObject *parent);

    // 阻塞式请求。必须在 QNetworkAccessManager 所在线程（通常为主线程）调用，
    // 内部使用嵌套事件循环，GUI 信号照常派发。
    // 非流式 legacy 补全请求，返回完整响应 JSON；出错时返回包含 "error" 字段的 JSON
    QJsonObject create(const QJsonObject &input);
};

// 聊天入口（/chat/completions）
CategoryChat &chat();

// legacy 补全入口（/completions）
CategoryCompletion &completion();

// ---- 运行时配置（默认值来自 initByEnv，可在运行期覆盖）----

// 设置 / 获取接口基础 URL（如 https://api.example.com/v1，不含端点路径）
void setUrl(const QString &url);
QString url();

// 设置 / 获取 Bearer Token
void setToken(const QString &token);
QString token();

// 设置 / 获取最大重试次数（仅 5xx / 429，指数退避；默认 0 不重试）
void setMaxRetries(int retries);
int maxRetries();

// 设置 / 获取流式静默超时（毫秒，默认 60000）：每收到数据即重置，长回复不误杀；<=0 表示不限时
void setTimeout(int milliseconds);
int timeout();

// 设置 / 获取阻塞式非流式请求总超时（毫秒，默认 120000；<=0 表示不限时）
void setBlockingTimeout(int milliseconds);
int blockingTimeout();

// 设置 / 获取调试日志开关（输出请求 URL、请求体、状态码、SSE 帧）
void setVerbose(bool enabled);
bool verbose();

// 初始化：从环境变量 QOpenAiBaseUrl / QOpenAiToken 读取配置
void initByEnv();

} // namespace QOpenAi