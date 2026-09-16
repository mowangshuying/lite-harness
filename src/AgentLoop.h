#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>

class QProcess;

class AgentLoop : public QObject
{
    Q_OBJECT
public:
    explicit AgentLoop(QObject *parent = nullptr);
    ~AgentLoop() override;

    // 启动代理循环（异步，不阻塞 UI 线程）
    void run(const QString &userMessage);
    // 停止：取消当前流、kill 正在运行的 QProcess，并通知错误
    void stop();

    // 设置工作目录：作为 bash 执行的 cwd，并注入 system prompt（空串忽略，路径归一化为绝对路径）
    void setWorkDir(const QString &dir);
    QString workDir() const;

signals:
    // 思考过程增量（forward 给 UI）
    void thinkingDelta(const QString &delta);
    // 回复文本增量（逐字/逐段）
    void textDelta(const QString &delta);
    // 工具执行结果（UI 展示）：工具名 / 人类可读关键参数摘要 / 完整输出
    // 每次工具执行完成发射一次（含 Dangerous blocked / Unknown tool / 沙箱拒绝等错误结果）
    void toolOutputReady(const QString &toolName, const QString &summary, const QString &output);
    // 循环结束，最终回复
    void finished(const QString &replyText);
    // 错误
    void error(const QString &errorMessage);

private:
    // 发起一次流式聊天请求
    void startChatRequest(const QJsonArray &messages);
    // 有工具调用：追加带 tool_calls 的 assistant 消息并进入工具执行链
    void continueWithToolResults(const QJsonObject &assistantMessage);
    // 依次取出待执行工具，全部完成后回填结果并再次请求
    void runNextTool();
    // 按工具名路由一次工具调用：bash 走异步进程，文件类工具同步执行，未知工具回填错误结果
    void dispatchToolCall(const QJsonObject &toolCall);
    // 单个工具执行完成的统一收口（安全/超时/未知/沙箱等快捷路径也走这里）
    void onToolFinished(const QJsonObject &toolCall, const QString &toolName,
                        const QString &summary, const QString &output);
    // 异步执行 bash 命令（带安全检查与超时，不阻塞 UI）
    void executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args);
    // 文件类工具（本地 IO，同步执行；参数取自 tool 调用解析后的 arguments JSON）
    QString runReadFile(const QJsonObject &args);
    QString runWriteFile(const QJsonObject &args);
    QString runEditFile(const QJsonObject &args);
    QString runGlob(const QJsonObject &args);
    // 沙箱路径解析：相对路径按 m_workDir 解析；逃逸工作区时返回空串并置 *error
    QString safePath(const QString &p, QString *error) const;
    // 工具定义（bash / read_file / write_file / edit_file / glob）
    static QJsonArray createToolsDefinition();

private:
    QString m_model;                 // 模型 ID，从环境变量 MODEL_ID 读取
    QString m_workDir;               // 工作目录，默认 QDir::currentPath()（构造时初始化）
    QVector<QJsonObject> m_messages; // 对话历史（仅主线程访问，无需 mutex）
    bool m_running = false;          // 防并发（尽量只主线程）
    QPointer<QObject> m_currentStream = nullptr; // 当前 ChatStream（弱引用）
    int m_toolIterations = 0;        // 工具调用轮次计数
    QJsonArray m_pendingToolCalls;   // 待执行 tool 调用队列
    QJsonArray m_toolResultsReady;   // 已执行完的 tool 结果消息
    QList<QProcess *> m_activeProcesses; // 正在运行的 QProcess，stop()/析构时 kill
};