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
    void startRequest(const QJsonObject &input);
    void readIncoming();
    void processFrame(const QByteArray &frame);
    void finishStream();
    void cleanupReply();

    friend class CategoryCompletion;

private:
    class Private;
    Private *d;
};

class CategoryCompletion
{
public:
    // 发起一次流式聊天请求，返回一个可监听信号的 ChatStream（parent 为其父对象）
    ChatStream *createStream(const QJsonObject &input, QObject *parent);
};

// 聊天入口（参照旧 TongYiOpenAi 的分类风格）
CategoryCompletion &completion();

// 初始化：从环境变量 QOpenAiUrl / QOpenAiToken 读取配置
void __initByEnv();

} // namespace QOpenAi