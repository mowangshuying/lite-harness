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
     *  done(changed, conversation)：changed 为最终态与入参不等价（镜像迁移前同步宿主
     *  的 `conversation == original` 判定），conversation 为交付时的最终会话
     *  （偏离 §3.3 草案：按值经回调交付而非原地引用——挂起跨越 await 后调用方
     *  栈上引用可能已析构）。 */
    QOpenAi::AsyncRequest *prepareAsync(QVector<QJsonObject> conversation,
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

private:
    // ---- lcc 六方法在 OpenAI 形态下的等价实现（均原地修改 conversation） ----
    void toolResultBudget(QVector<QJsonObject> &conversation) const;
    void snipCompact(QVector<QJsonObject> &conversation) const;
    void microCompact(QVector<QJsonObject> &conversation, qsizetype targetChars) const;
    void fitToolResults(QVector<QJsonObject> &conversation, qsizetype targetChars) const;
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
    QString persistLargeOutput(const QString &toolUseId, const QString &output) const;
    bool isArchiveMarker(const QJsonObject &message) const;

    // ---- OpenAI 形态谓词与估算 ----
    static bool isToolResult(const QJsonObject &message);   // role=="tool"（lcc is_tool_result）
    static bool hasToolUse(const QJsonObject &message);     // assistant 且 tool_calls 非空（lcc has_tool_use）
    static qsizetype estimateChars(const QVector<QJsonObject> &conversation);
    /** lcc 尾段回退规则在 OpenAI 形态下的推广：尾段起点若落在工具结果串中间，
     *  整串回退，确保 assistant(tool_calls) 与其后全部 tool 消息不被切开（400 风险）。 */
    static qsizetype retreatToolBatch(const QVector<QJsonObject> &conversation, qsizetype tailStart);
    QJsonObject summaryMessage(const QString &label, const QString &request,
                               const QString &summary, const QString &transcript) const;
    void emitCard(const QString &cardSummary, qsizetype beforeChars, qsizetype afterChars,
                  const QString &transcript) const;

    QString transcriptDir() const;
    QString toolResultsDir() const;

    WorkDirSink m_workDirSink;
    ModelSink m_modelSink;
    CardSink m_cardSink;
};
