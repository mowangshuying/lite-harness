#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>

#include <functional>

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

    // 权限门：收到 permissionRequired 后工具队列暂停，UI 取得用户裁决后调用本方法续跑
    // （allow=true 继续执行该工具调用；false 回填 "Permission denied"；无待决询问时忽略）
    void resolvePermission(bool allow);

signals:
    // 思考过程增量（forward 给 UI）
    void thinkingDelta(const QString &delta);
    // 回复文本增量（逐字/逐段）
    void textDelta(const QString &delta);
    // 工具执行结果（UI 展示）：工具名 / 人类可读关键参数摘要 / 完整输出
    // 每次工具执行完成发射一次（含 Dangerous blocked / Unknown tool / 沙箱拒绝等错误结果）
    void toolOutputReady(const QString &toolName, const QString &summary, const QString &output);
    // 权限门：工具调用命中询问规则（不含硬拒绝列表项）时发射，队列暂停等待 resolvePermission() 裁决
    // toolName / summary 与 toolOutputReady 前两参同义；reason = 命中规则文案
    //（"Writing outside workspace" / "Potentially destructive command"）
    void permissionRequired(const QString &toolName, const QString &summary, const QString &reason);
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
    // 按工具名路由一次工具调用：先触发 PreToolUse 钩子链（权限门为其中的内置钩子：
    // 硬拒绝直接回填 / ASK 前缀暂停队列），然后 bash 走异步进程、文件类工具同步执行、
    // 未知工具回填错误结果
    // permissionGranted=true 为用户批准后的续跑路径，由 permission 钩子内部短路（仅 resolvePermission 内部使用）
    void dispatchToolCall(const QJsonObject &toolCall, bool permissionGranted = false);
    // 权限门·硬拒绝列表（仅 bash）：命中返回 "Blocked: {pattern} is on the deny list"，未命中返回空串
    // （s04 起由内置 permission 钩子调用，逻辑与文案未变）
    QString checkDenyList(const QString &command) const;
    // 权限门·询问规则：命中返回规则文案（钩子链据此携带 ASK 前缀触发 permissionRequired 暂停），未命中返回空串
    QString checkPermissionRules(const QString &toolName, const QJsonObject &args) const;
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

    // ---- 生命周期钩子（lcc s04 HOOKS 注册表的内聚移植，事件名 → 有序 handler 列表）----
    // 各事件回调签名；返回空串表示放行/无副作用，非空含义按事件约定：
    // - PreToolUse：硬拦截文本（回填为 tool_result）或 "ASK:<reason>" 前缀（触发异步询问）
    // - Stop：强制继续文本（仅注入历史，不续跑——对齐 lcc 语义，详见 .cpp 注释）
    // - UserPromptSubmit / PostToolUse：约定恒为空（纯日志副作用）
    using UserPromptSubmitHook = std::function<QString(const QString &prompt)>;
    using PreToolUseHook = std::function<QString(const QJsonObject &toolCall, bool permissionGranted)>;
    using PostToolUseHook = std::function<QString(const QJsonObject &toolCall, const QString &output)>;
    using StopHook = std::function<QString()>;

    // 注册内置钩子（构造函数调用；对应 lcc s04 模块尾部的 6 个 register_hook）
    void registerBuiltinHooks();
    // 触发对应事件钩子链：按注册顺序执行，第一个非空返回值短路返回（逐字对齐 lcc trigger_hooks），
    // 全空则返回空串
    QString triggerUserPromptSubmitHooks(const QString &prompt);
    QString triggerPreToolUseHooks(const QJsonObject &toolCall, bool permissionGranted);
    QString triggerPostToolUseHooks(const QJsonObject &toolCall, const QString &output);
    QString triggerStopHooks();

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
    QJsonObject m_pendingPermissionCall; // 等待权限裁决的工具调用（队列暂停上下文）
    bool m_awaitingPermission = false;   // 权限询问中（permissionRequired 已发、resolvePermission 未到）
    // 四事件钩子链（仅主线程访问；注册顺序即执行顺序，见 registerBuiltinHooks）
    QVector<UserPromptSubmitHook> m_userPromptSubmitHooks;
    QVector<PreToolUseHook> m_preToolUseHooks;
    QVector<PostToolUseHook> m_postToolUseHooks;
    QVector<StopHook> m_stopHooks;
};