#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace QOpenAi { class AsyncRequest; }

/**
 * CompactManager —— 上下文压缩引擎（对齐 lcc s08 compact_manager.py 的 CompactManager）
 *
 * 非 QObject、无信号：以纯函数方式对"会话消息向量"（不含 system 消息，
 * 与 lcc 将 system 单独随每次请求下发一致）执行五级压缩流水线：
 *   tool_result_budget → snip_compact →(超阈值时) micro_compact → fit_tool_results → compact_history
 *
 * 与宿主（AgentLoop）的耦合全部经由构造函数注入的回调：
 *   - workDirSink：动态读取宿主会话数据根（含 .lite-harness 或按会话隔离的 sessions/<id>，
 *     setWorkDir/会话 ID 变化后路径自然跟随，对应 lcc env 目录）；
 *   - modelSink：摘要 LLM 调用使用的模型 id（与主循环一致）；
 *   - cardSink：压缩发生时回传 GUI 卡片（宿主复用 toolOutputReady，见裁决 e）。
 *
 * OpenAI 形态映射（R-1 裁决）：Anthropic 的 tool_use/tool_result content block
 * 在我们的历史里是"assistant 消息的 tool_calls 字段"与"role==tool 消息的 content 字符串"，
 * 块级改写即工具消息的 content 字段改写。
 */
class CompactManager
{
public:
    using WorkDirSink = std::function<QString()>;
    using ModelSink = std::function<QString()>;
    using CardSink = std::function<void(const QString &summary, const QString &output)>;

    CompactManager(WorkDirSink workDirSink, ModelSink modelSink);

    void setCardSink(CardSink sink);

    // ---- 异步链（P3，设计文档 §2.3/§3.3）：五条路径中仅"摘要 LLM 调用"一段挂起，
    //      转写落盘/尾段回退/卡片组装等本地段仍同步执行；prompt 构建与结果应用沿用
    //      迁移前三段拆分出的同一组私有方法。契约（同 P1 loadMemoriesAsync）：
    //      done 恒恰好调用一次；ctx 为生命周期锚（请求 parent 到 ctx，ctx 析构即链
    //      作废、done 永久静默）；返回在途 AsyncRequest 供宿主 cancel，本地短路路径
    //      同步交付后返回 nullptr。摘要失败/超时交付空串，由这些方法内部补
    //      "(empty summary)" 占位——降级语义与原同步链逐字一致，不抛不卡。 ----

    /** prepare 的异步版（lcc prepare 五级压缩流水线，原地修改 conversation）：
     *  前四段本地管线同步跑；仅触发全量压缩时挂起。
     *  autoCompactCardSummary 为触发全量压缩时卡片的档位描述（宿主注入译文）。
     *  修4（token 域门槛，D10）：conversationTokens 为宿主锚定的会话体 token 估算
     *  （usage.prompt_tokens 校准值减 overhead，首轮回落纯估算）——入口门槛只用它；
     *  overheadTokens 为随每次请求下发但不进 conversation 的固定开销（system +
     *  tools schema + 注入段）估算，管线内从全局预算扣除。段内退出判定仍用管线
     *  自身对 conversation 的即时估算（estimateTokens）。
     *  done(changed, conversation)：changed 为最终态与入参不等价（镜像迁移前同步宿主
     *  的 `conversation == original` 判定），conversation 为交付时的最终会话
     *  （偏离 §3.3 草案：按值经回调交付而非原地引用——挂起跨越 await 后调用方
     *  栈上引用可能已析构）。 */
    QOpenAi::AsyncRequest *prepareAsync(QVector<QJsonObject> conversation,
                                        qsizetype conversationTokens,
                                        qsizetype overheadTokens,
                                        const QString &activeRequest,
                                        const QString &autoCompactCardSummary,
                                        QObject *ctx,
                                        std::function<void(bool changed,
                                                           const QVector<QJsonObject> &conversation)> done) const;

    /** compactHistory 的异步版（lcc compact_history：compact 工具/自动压缩的全量压缩，
     *  整段历史替换为单条摘要消息；交付替换后的会话——不含 system，宿主按裁决 g 重新
     *  拼接 system 消息）：转写与卡片时序不变（卡片在 replaced 组装后、
     *  done 交付前发出），仅摘要段挂起。 */
    QOpenAi::AsyncRequest *compactHistoryAsync(const QVector<QJsonObject> &conversation,
                                               const QString &activeRequest,
                                                 const QString &cardSummary,
                                                 QObject *ctx,
                                                 std::function<void(const QVector<QJsonObject> &replaced)> done) const;

    /** reactiveCompact 的异步版（lcc reactive_compact：上下文超限后的反应式压缩，
     *  保留最近若干消息为尾段）：空 conversation 短路（零请求，与原同步链一致，
     *  done 同步交付原会话）；retreatToolBatch 配对保护在同步段先行、语义不动。 */
    QOpenAi::AsyncRequest *reactiveCompactAsync(const QVector<QJsonObject> &conversation,
                                                const QString &activeRequest,
                                                const QString &cardSummary,
    QObject *ctx,
                                                 std::function<void(const QVector<QJsonObject> &replaced)> done) const;

    /// OpenAI 形态会话字符总量估算（UTF-16 码元口径，压缩管线旧计量）。
    /// 修4 后管线门槛改用 estimateTokens；本函数按规格保留（历史数据口径参照）。
    static qsizetype estimateChars(const QVector<QJsonObject> &conversation);

    /// OpenAI 形态会话 token 总量估算（修4 计量口径：逐条 Compact 序列化后交
    /// AgentConst::estimateTokens 求和）。公开供宿主计量/锚点组合复用，防第二套口径漂移。
    static qsizetype estimateTokens(const QVector<QJsonObject> &conversation);

private:
    // ---- lcc 六方法在 OpenAI 形态下的等价实现（均原地修改 conversation） ----
    /** 修4：batch/large 判定切 token 域，比例不变（batch=4×会话体预算、large=0.6×会话体预算），
     *  会话体预算 convBudgetTokens 由 prepareAsync 现算注入（= 全局预算 − overhead，钳位）。 */
    void toolResultBudget(QVector<QJsonObject> &conversation, qsizetype convBudgetTokens) const;
    /** 修2（D3 回滞双门槛）：条数 > kSnipTriggerMessages 且 tokenGateOk（调用方判
     *  会话体估算 > 会话体预算/2）才归档；两门槛消除「低占用也压缩」与逐请求反复归档。 */
    void snipCompact(QVector<QJsonObject> &conversation, bool tokenGateOk) const;
    void microCompact(QVector<QJsonObject> &conversation, qsizetype targetTokens) const;
    void fitToolResults(QVector<QJsonObject> &conversation, qsizetype targetTokens) const;
    QString summaryInput(const QVector<QJsonObject> &conversation) const;
    /** 摘要请求体（model / system+user 两条消息 / max_tokens）——异步链构建段。 */
    QJsonObject buildSummaryRequest(const QVector<QJsonObject> &conversation) const;
    /** 摘要 LLM 段（P3 异步化）：走 QOpenAi::AsyncRequest；失败/超时交付空串
     *  （与原同步链的 error 键降级日志同款），"(empty summary)" 占位由上层调用点补齐——
     *  沿用原同步链"占位在 summarizeHistory 末尾"的归属，行为等价。 */
    QOpenAi::AsyncRequest *summarizeHistoryAsync(const QVector<QJsonObject> &conversation,
                                                 QObject *ctx,
                                                 std::function<void(const QString &summary)> done) const;

    // ---- 落盘与路径辅助（lcc write_transcript / persisted_output_path / save_output / ...） ----
    QString writeTranscript(const QVector<QJsonObject> &conversation) const;
    QString persistedOutputPath(const QString &content) const;
    bool saveOutput(const QString &toolUseId, const QString &output, QString *savedPath) const;
    QString persistedPreview(const QString &toolUseId, const QString &output,
                             qsizetype previewChars) const;
    /** 修4：直通判定切 token 域（估算 ≤ largeTokenLimit 不落盘），阈值由调用方按
     *  会话体预算派生注入（落盘预览 kBudgetPreviewChars 仍为字符域，D11 不动）。 */
    QString persistLargeOutput(const QString &toolUseId, const QString &output,
                               qsizetype largeTokenLimit) const;
    /** 修2（D5）：被归档中段逐行追加到会话级固定文件 <转写目录>/snip_archive.jsonl
     *  （Append 只增不重写，替换旧每轮 UUID 全量快照）。返回落盘路径；失败返回空串，
     *  调用方 snip 本轮直通降级（沿用「落盘失败不归档」旧语义）。 */
    QString appendSnipTranscript(const QVector<QJsonObject> &archived) const;

    // ---- OpenAI 形态谓词与估算 ----
    static bool isToolResult(const QJsonObject &message);   // role=="tool"（lcc is_tool_result）
    static bool hasToolUse(const QJsonObject &message);     // assistant 且 tool_calls 非空（lcc has_tool_use）
    /** lcc 尾段回退规则在 OpenAI 形态下的推广：尾段起点若落在工具结果串中间，
     *  整串回退，确保 assistant(tool_calls) 与其后全部 tool 消息不被切开（400 风险）。 */
    static qsizetype retreatToolBatch(const QVector<QJsonObject> &conversation, qsizetype tailStart);
    QJsonObject summaryMessage(const QString &label, const QString &request,
                               const QString &summary, const QString &transcript) const;
    /** 修4：卡片前后值改 token 口径（文案 `≈tokens` 同步；Transcript 路径行不变）。 */
    void emitCard(const QString &cardSummary, qsizetype beforeTokens, qsizetype afterTokens,
                  const QString &transcript) const;

    QString transcriptDir() const;
    QString toolResultsDir() const;

    WorkDirSink m_workDirSink;
    ModelSink m_modelSink;
    CardSink m_cardSink;
};
