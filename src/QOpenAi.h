#pragma once

#include <QObject>
#include <QJsonObject>

#include <functional>

class QTimer;

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

// ---- 一次性异步文本请求（异步链迁移 P0，规格 docs/async-chain-design.md §3.1）----
//
// 为什么要它：记忆召回/沉淀/整理与压缩摘要等侧链在异步迁移前走嵌套事件循环的
// 阻塞链，GUI 线程被同步等待劫持。
// AsyncRequest 复用 ChatStream 的流式组装（自带 5xx/429 退避重试、idle 静默超时、
// 配置缺失延迟 error、析构 abort reply），只提取 choices[0].message.content 正文，
// 以 done(content, error) 恰好一次的回调形态交付——网络零新代码，行为语义与阻塞链对齐。
//
// 契约：
//   - done 恰好调用一次：messageFinished / error / 总超时三条终态路径共用 m_done
//     门闩，首到终态生效、后到路径吞掉（error 与 finished 竞态防线）；
//   - error 为空串 = 成功；超时、网络错误、配置缺失一律折叠为 error 字符串，
//     不抛异常不弹窗，调用方降级路径接住（与"工具侧失败折叠为输出字符串"约定一致）；
//   - cancel() 后 done 永久静默；对象终态后自行 deleteLater，调用方持 QPointer 观察即可；
//   - parent 即生命周期锚：锚析构 = 请求作废（回调丢弃、ChatStream 析构 abort 在途 reply）。
//
// 迁移落账：P1 召回链、P2 沉淀/整理链、P3 压缩链已改走本类；阻塞族与其配套的
// 等待期禁发送网关已于 P4 整体删除（设计文档 docs/async-chain-design.md §4/§5 与文末落账节）。
class AsyncRequest : public QObject
{
    Q_OBJECT
public:
    // 发起一次性异步文本请求。input 为 /chat/completions 请求体（stream 参数由内部接管）。
    // totalTimeoutMs 总超时（默认 120000，沿用迁移前阻塞链的总时限）；<=0 表示不设总时限，
    // 仅依赖 ChatStream 的 idle 静默超时兜底。
    static AsyncRequest *sendText(const QJsonObject &input, QObject *parent,
                                  std::function<void(const QString &content, const QString &error)> done,
                                  int totalTimeoutMs = 120000);

    // 主动取消：调用后 done 永不触发，对象 deleteLater 自清理
    void cancel();

private:
    explicit AsyncRequest(QObject *parent);

    void start(const QJsonObject &input, int totalTimeoutMs);
    // 终态唯一出口：m_done 首到门闩 → 停表/取消流 → 交付回调 → deleteLater 自清理
    void fireDone(const QString &content, const QString &error);

    ChatStream *m_stream = nullptr;   // 子对象：流式组装执行者
    QTimer *m_totalTimer = nullptr;   // 子对象：总超时哨兵（idle 会因持续收字节重置，杀不死"慢而不断"的流，需总量防线）
    std::function<void(const QString &, const QString &)> m_handler; // sendText 移交的回调
    bool m_done = false;              // 防重入门闩，镜像 ChatStream::Private::done（QOpenAi.cpp:502）语义
};

class CategoryChat
{
public:
    // 发起一次流式聊天请求，返回一个可监听信号的 ChatStream（parent 为其父对象）
    ChatStream *createStream(const QJsonObject &input, QObject *parent);
};

class CategoryCompletion
{
public:
    // 发起一次流式 legacy 补全请求（/completions），返回一个可监听信号的 ChatStream
    ChatStream *createStream(const QJsonObject &input, QObject *parent);
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

// 设置 / 获取调试日志开关（输出请求 URL、请求体、状态码、SSE 帧）
void setVerbose(bool enabled);
bool verbose();

// 初始化：从环境变量 QOpenAiBaseUrl / QOpenAiToken 读取配置
void initByEnv();

} // namespace QOpenAi