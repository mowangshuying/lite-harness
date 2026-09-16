#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

#include <functional>

class QProcess;
class SubAgent;

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
    // 待办清单更新（lcc s05 引入，跨车道契约；s06 起为无状态语义）：todo_write 每次校验通过
    // （含清为空清单）后发射，携带本次输入清单的快照 [{content, status}, ...]（不再持久化，
    // 由模型按计划逐轮重发全量清单）；校验失败与权限询问中不发射
    void todoUpdated(const QJsonArray &todos);
    // 循环结束，最终回复
    void finished(const QString &replyText);
    // 错误
    void error(const QString &errorMessage);

private:
    // task 子代理需要复用本类的钩子注册表、权限门静态检查、工具定义与 handler 表构建器，
    // 以及 ToolHandler 嵌套类型（对外部接口零暴露，仅友元可见）
    friend class SubAgent;

    // 发起一次流式聊天请求
    void startChatRequest(const QJsonArray &messages);
    // 有工具调用：追加带 tool_calls 的 assistant 消息并进入工具执行链
    void continueWithToolResults(const QJsonObject &assistantMessage);
    // 依次取出待执行工具，全部完成后回填结果并再次请求
    void runNextTool();

    // 工具同步 handler 签名（lcc s06 handlers 字典等价：名称 → args 进、结果文本出；
    // 一切失败都是字符串，无异常）。bash/task 为异步特判路径，不进表（见 executeTool）
    using ToolHandler = std::function<QString(const QJsonObject &args)>;

    // 统一工具执行入口（lcc s06 execute_tool，主循环专用）：先触发 PreToolUse 钩子链
    // （权限门为其中的内置钩子：硬拒绝直接回填 / "ASK:" 前缀暂停队列），然后
    // bash 走异步进程链、task 走子代理链（两个异步特判），其余经 handlers 表同步路由
    // （未注册名称回填 "Unknown tool: <name>"），最后 PostToolUse 钩子 + onToolFinished 统一收口
    // permissionGranted=true 为用户批准后的续跑路径，由 permission 钩子内部短路（仅 resolvePermission 内部使用）
    void executeTool(const QJsonObject &toolCall, const QHash<QString, ToolHandler> &handlers,
                     bool permissionGranted);
    // 主循环同步 handler 表：read/write/edit/glob（复用 baseFileToolHandlers）+ todo_write；
    // bash/task 为异步特判，不在此表
    QHash<QString, ToolHandler> mainToolHandlers();
    // 基础文件工具 handler 表（lcc s06 主/子代理共享注册形态）：以传入 workDir 为沙箱根，
    // 宿主与子代理各自构建、互不串扰（子代理不经 todo_write/task，工具集为其白名单子集）
    static QHash<QString, ToolHandler> baseFileToolHandlers(const QString &workDir);

    // 权限门·硬拒绝列表（仅 bash）：命中返回 "Blocked: {pattern} is on the deny list"，未命中返回空串
    // （s04 起由内置 permission 钩子调用，逻辑与文案未变；s06 提为静态供子代理共用）
    static QString checkDenyList(const QString &command);
    // 权限门·询问规则：命中返回规则文案（钩子链据此携带 ASK 前缀触发 permissionRequired 暂停），未命中返回空串
    // （s06 起显式传入 workDir：文件工具逃逸判定以该沙箱根为准，供子代理共用同一逻辑）
    static QString checkPermissionRules(const QString &workDir, const QString &toolName,
                                        const QJsonObject &args);
    // —— 供 SubAgent（友元）复用同一份实现的静态转发（转调 .cpp 内部同名工具函数）——
    static const QString &askPrefixOf();
    static QString toolSummaryOf(const QString &toolName, const QJsonObject &args);
    static const QStringList &dangerousCommandList();
    // 单个工具执行完成的统一收口（安全/超时/未知/沙箱等快捷路径也走这里）
    void onToolFinished(const QJsonObject &toolCall, const QString &toolName,
                        const QString &summary, const QString &output);
    // 异步执行 bash 命令（带安全检查与超时，不阻塞 UI）
    void executeBashAsync(const QJsonObject &toolCall, const QJsonObject &args);
    // task 工具（lcc s06）：启动 SubAgent 异步链（独立上下文黑盒；权限询问经本类
    // permissionRequired 转发；完成回调以汇总文本走 onToolFinished 收口）
    void startSubAgentTask(const QJsonObject &toolCall, const QJsonObject &args);
    // 子代理统一收口（lcc s06 R1）：级联 cancel → kill 流与进程 → 为 task 合成
    // "(cancelled)" tool_result 直写历史 → 清父队列；stop()/错误链/析构三路复用
    void cancelSubAgent();
    // 文件类工具（本地 IO，同步执行；经 handler 表路由，参数取自解析后的 arguments JSON）。
    // s06 起为静态并以 workDir 为沙箱根参数：宿主与子代理共用同一实现、各传各的目录
    static QString runReadFileIn(const QString &workDir, const QJsonObject &args);
    static QString runWriteFileIn(const QString &workDir, const QJsonObject &args);
    static QString runEditFileIn(const QString &workDir, const QJsonObject &args);
    static QString runGlobIn(const QString &workDir, const QJsonObject &args);
    // 待办工具（lcc s06 update_todos 等价：无状态）：校验入参并返回本次输入的渲染文本
    // （不落盘不持久，成功时以输入快照发射 todoUpdated）；任一校验失败返回 "Error:..."
    QString runTodoWrite(const QJsonObject &args);
    // 沙箱路径解析（静态化供上述工具共用）：相对路径按 workDir 解析；逃逸时返回空串并置 *error
    static QString safePathIn(const QString &workDir, const QString &p, QString *error);
    // 工具定义（bash / read_file / write_file / edit_file / glob / todo_write / task）
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
    // task 子代理（lcc s06）：串行队列保证同一时刻至多一个；随本对象父子销毁，QPointer 防回调竞态
    QPointer<SubAgent> m_activeSub;      // 正在运行的子代理（无则为 null）
    QJsonObject m_pendingTaskCall;       // 其对应的 tool call（cancelSubAgent 收口 "(cancelled)" 用）
    // 四事件钩子链（仅主线程访问；注册顺序即执行顺序，见 registerBuiltinHooks）
    QVector<UserPromptSubmitHook> m_userPromptSubmitHooks;
    QVector<PreToolUseHook> m_preToolUseHooks;
    QVector<PostToolUseHook> m_postToolUseHooks;
    QVector<StopHook> m_stopHooks;

    // 待办清单（lcc s06 起无状态：不再持有持久清单成员，todo_write 每次只校验+渲染入参；
    // TodoItem/renderTodos 保留为纯数据结构与纯函数）
    struct TodoItem
    {
        QString content;
        QString status; // pending | in_progress | completed（校验归一后的值）
    };
    // 渲染面板文本（lcc render 等价的纯函数）：空清单返回 "No todos"
    static QString renderTodos(const QVector<TodoItem> &items);
    int m_roundsSinceTodo = 0;          // lcc rounds_since_todo：每轮用户提问（run）归零起步
    bool m_usedTodoThisRound = false;   // lcc used_todo：每批 tool_calls 开始时归零
};