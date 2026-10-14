// AgentLoopWorkflow.cpp — s16 Lane C: host wiring TU for the workflow orchestration engine.
//
// lcc s16 42a555d reference: workflow tool dispatch + AnthropicAgentRunner bridge + registry
// policy gate. Deviations registered inline (async deferred backfill, token estimation,
// single-flight guard, cancel channel — lcc has no stop path).
//
// Composition: WorkflowManager (Lane A, pure engine, value member m_workflow) owns the
// registry / journal ledger / host policy table. WorkflowRuntime (Lane B, NOT a QObject)
// is a one-shot orchestrator created per run; runtime state is kept alive by the heap
// WfRun shared_ptr capture chain, so the host slot uses std::shared_ptr + pointer-match
// guard instead of QPointer (interface mismatch vs task spec registered in report).

#include "AgentLoop.h"

#include "AgentConstants.h"
#include "AgentLoopInternal.h"
#include "LiteLog.h"
#include "ToolNames.h"
#include "WorkflowManager.h"
#include "WorkflowRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>

// Start one workflow run (lcc s16 workflow tool: same-turn synchronous blocking orchestration
// is NOT adopted — lite deviation: async deferred tool_result backfill, mirroring the SubAgent
// channel so the main loop never blocks the UI thread).
//
// Reentrancy note (verified against executeTool): the ASK permission gate suspends the queue
// BEFORE any special-case dispatch is reached, so a permission-resume re-entering executeTool
// runs startWorkflowRun exactly once per approval — same mechanism as bash/task, no extra
// dedupe needed here. The single-flight guard below covers the other real race: a second
// workflow tool call inside the same batch while the first run is still in flight
// (lcc has neither a concurrency guard nor a stop path — lite additions, registered).
void AgentLoop::startWorkflowRun(const QJsonObject &toolCall, const QJsonObject &args)
{
    // Single-flight guard (lite addition, lcc s16 has no concurrency notion on this path).
    if (m_activeWorkflow)
    {
        onToolFinished(toolCall, ToolNames::WORKFLOW,
                       args.value(QStringLiteral("name")).toString(),
                       QStringLiteral("Error: another workflow run is already active"));
        return;
    }

    // Fresh in-flight table for this run (previous run's entries are terminal-null or
    // cancelled by cancelActiveWorkflow; clear() keeps the slot deterministic anyway).
    m_workflowInFlight.clear();

    auto runtime = std::make_shared<WorkflowRuntime>();
    m_activeWorkflow = runtime;
    m_pendingWorkflowCall = toolCall;

    // Runner bridge (lcc s16 AnthropicAgentRunner → lite AsyncRequest, same async discipline
    // as the compact/memory side chains). done() is guaranteed called at most once by the
    // AsyncRequest contract (finished / error / total timeout — first wins; cancel() = forever
    // silent). Deviation: lcc reads real usage tokens; AsyncRequest carries no usage, so we
    // estimate locally with the shared char→token codec.
    const QPointer<AgentLoop> self(this);
    const QString model = m_recoveryCurrentModel.isEmpty() ? m_model : m_recoveryCurrentModel;
    WfAgentRunner runner = [self, model](const QString &prompt, const QString &schemaJson,
                                         const QString &label, const WfAgentDone &done)
    {
        if (!self)
            return; // host gone — drop silently (Lane B dtor discipline)

        QJsonObject input;
        input[QStringLiteral("model")] = model;
        QJsonArray messages;
        QJsonObject system;
        system[QStringLiteral("role")] = QStringLiteral("system");
        system[QStringLiteral("content")] = WorkflowRuntime::kFocusedSystemPrompt();
        messages.append(system);
        QJsonObject user;
        user[QStringLiteral("role")] = QStringLiteral("user");
        // schemaJson assembly stays inside Lane B's helper so the prompt bytes match the
        // validation layer's expectations exactly.
        user[QStringLiteral("content")] = WorkflowRuntime::buildRunnerPrompt(prompt, schemaJson);
        messages.append(user);
        input[QStringLiteral("messages")] = messages;
        input[QStringLiteral("max_tokens")] = WorkflowRuntime::kAgentMaxTokens;
        // No tools / no stream_options on this chain: the focused runner expects a single
        // JSON answer text (lcc s16 parity).

        LOG_DEBUG << QStringLiteral("[workflow] runner turn: ")
                  << label << QStringLiteral(" promptChars=")
                  << QString::number(prompt.size());

        QOpenAi::AsyncRequest *req = QOpenAi::AsyncRequest::sendText(
            input, self.data(),
            [self, prompt, done](const QString &content, const QString &error)
            {
                if (!self)
                    return;
                if (!error.isEmpty())
                {
                    done(QJsonValue(), qint64(0), error);
                    return;
                }
                // Tokens: estimate(prompt)+estimate(content), no usage available on this
                // transport (registered deviation).
                const qint64 tokens =
                    qint64(AgentConst::estimateTokens(prompt)
                           + AgentConst::estimateTokens(content));
                // Value passes through VERBATIM: Lane B parses and validates internally
                // (WorkflowRuntime.cpp agent() consumption — pre-parsing here would turn
                // the object into an empty toString() and break the schema retry layer).
                done(QJsonValue(content), tokens, QString());
            });
        if (req)
            self->m_workflowInFlight.append(QPointer<QOpenAi::AsyncRequest>(req));
    };

    // Terminal backfill (ToolOutputDone): exactly once. Double guard — QPointer against
    // host destruction racing the deferred queue (same family as the settleTeamOnExit
    // dtor red line), pointer-match against a slot cleared by cancelActiveWorkflow()
    // (suppresses a late backfill after stop()) or replaced by a newer run.
    WorkflowRuntime::ToolOutputDone out = [self, runtime](const QString &toolOutput)
    {
        if (!self)
            return;
        if (self->m_activeWorkflow.get() != runtime.get())
            return; // cancelled/superseded — late result discarded (lcc parity: no consumer)
        const QJsonObject call = self->m_pendingWorkflowCall;
        self->m_activeWorkflow.reset();
        self->m_pendingWorkflowCall = QJsonObject();
        self->m_workflowInFlight.clear(); // terminal entries already null; drop stale handles
        self->onToolFinished(call, ToolNames::WORKFLOW,
                             call.value(QStringLiteral("arguments"))
                                 .toObject()
                                 .value(QStringLiteral("name"))
                                 .toString(),
                             toolOutput);
    };

    // toolArgs shape = the workflow tool's arguments object ({name, args, resume_from_run_id})
    // — exactly what executeTool parsed and hands in as args (lcc s16 tool-input parity).
    runtime->start(m_workflow, args, runner, out);
}

// Settle an active workflow run on stop()/destruction (lcc has no stop path — lite channel,
// mirrors cancelSubAgent's silent pairing discipline).
void AgentLoop::cancelActiveWorkflow()
{
    if (!m_activeWorkflow)
        return;

    // Clear the slot FIRST so the out closure's pointer-match guard rejects the terminal
    // backfill (requestCancel folds to a "workflow: run cancelled" result on the next
    // deferred beat; Lane B header claims an immediate terminal when nothing is in flight,
    // but the .cpp only raises the flag — registered as an observed deviation).
    const std::shared_ptr<WorkflowRuntime> runtime = m_activeWorkflow;
    m_activeWorkflow.reset();

    runtime->requestCancel();

    // Cancel the network bridge: done() never fires after cancel(), so the WfRun continuation
    // chain strands mid-flight (memory leak, not UB — the chain holds only Lane A/B state).
    // The tool_use↔tool_result pairing must therefore be synthesized by the host below.
    for (QPointer<QOpenAi::AsyncRequest> &req : m_workflowInFlight)
    {
        if (req)
            req->cancel();
    }
    m_workflowInFlight.clear();

    // Half-batch pairing: write the tool_result straight into history (no onToolFinished,
    // no display signals — safe on the destruction path, same red line family as
    // cancelSubAgent's "(cancelled)" and stop()'s "(stopped)" syntheses).
    if (!m_pendingWorkflowCall.isEmpty())
    {
        QJsonObject result = AgentLoopDetail::makeToolResult(
            m_pendingWorkflowCall.value(QStringLiteral("id")).toString(),
            QStringLiteral("(stopped)"));
        m_messages.append(result);
        m_pendingWorkflowCall = QJsonObject();
    }
}
