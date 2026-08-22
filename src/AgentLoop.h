#pragma once

#include <QObject>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QMutex>
#include <QThread>

class AgentLoop : public QObject
{
    Q_OBJECT
public:
    explicit AgentLoop(QObject *parent = nullptr);
    ~AgentLoop() override;

    // 启动代理循环（异步，不阻塞 UI 线程）
    void run(const QString &userMessage);

signals:
    // 工具执行结果（可选 UI 展示）
    void toolOutputReady(const QString &command, const QString &output);
    // 循环结束，最终回复
    void finished(const QString &replyText);
    // 错误
    void error(const QString &errorMessage);

private:
    // 在工作线程中执行的 Agent Loop
    void processLoop();
    // 执行 bash 命令（带安全检查与超时）
    QString executeBash(const QString &command);
    // bash 工具定义
    static QJsonArray createToolsDefinition();

private:
    QVector<QJsonObject> m_messages; // 对话历史
    QString m_model;                 // 模型 ID，从环境变量 MODEL_ID 读取
    QThread *m_thread = nullptr;     // 当前运行循环的工作线程
    QMutex m_mutex;                  // 保护 m_messages 跨线程访问
    QAtomicInt m_running = 0;        // 循环是否进行中（1 运行中 / 0 空闲）
};