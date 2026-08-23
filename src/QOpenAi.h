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

// 设置 / 获取请求超时（毫秒，默认 30000；<=0 表示不限时）
void setTimeout(int milliseconds);
int timeout();

// 设置 / 获取调试日志开关（输出请求 URL、请求体、状态码、SSE 帧）
void setVerbose(bool enabled);
bool verbose();

// 初始化：从环境变量 QOpenAiBaseUrl / QOpenAiToken 读取配置
void initByEnv();

} // namespace QOpenAi