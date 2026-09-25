# memory/compact 阻塞链异步化 — 架构设计（@oracle 产出，第四轮实施规格）

状态：✅ 已全部落地（P1 1256d51 / P2 9563901 / P3 dc73367 / P4 7e83012，审查修复 777a18c，见 §8）。原文按定稿存档，§1 现状表描述的是迁移前代码。
行号基于 HEAD=0162430 时代码，实施时以实际代码为准。

## 0. 事实基线（oracle 读码纠偏）

- 无「130s singleShot」：实际 `blockingTimeout=120000`（QOpenAi.cpp:28）+ 嵌套循环内 QTimer 总超时（:107-116）+ `streamIdleTimeout=60000` 每收字节重置（:273-276）。
- CompactManager 无 onComplete/onBusy 回调形态：prepare 为同步 void（:567-580），"busy 路径"实为 AgentLoop::applyCompactPipeline（AgentLoop.cpp:752-754）同步调用 + m_running 卫兵。

## 1. 六阻塞点全景（现状）

| # | 位置 | 链 |
|---|---|---|
| ① | run() :589 → m_memory.loadMemories → selectRelevantMemories（MemoryManager :676-727）→ blockingCreate | 召回，嵌套循环上限 120s |
| ② | startChatRequest :606 → applyCompactPipeline :752 → CompactManager::prepare→compactHistory :524→summarizeHistory :452→create | 压缩（唯一网络步骤） |
| ③ | messageFinished 终局 :680 extractMemories（:757-843→blockingCreate :790） | 沉淀 |
| ④ | :682 consolidateMemories（:845-985→blockingCreate :879，破坏段 :926-977 同步原子） | 整理；③④连续阻塞使 finished 延迟最长 2×120s |
| ⑤ | error 反应式压缩 :708-739：reactiveCompact :723 → 卫兵 :726 → applyCompressedConversation :728 → 重发 :732 | |
| ⑥ | runNextTool 批尾 :834-843 compactHistory → 卫兵 :840 → persistHistory :848 → startChatRequest :853 | 顺序红线 :831-833：reminder→results→压缩替换不可交换 |

cron 契约：tryDeliverCron :2004-2034 仅 !m_running 交付（:2008），emit scheduledUserMessage 同栈直连后回读 m_running（:2025-2032）——依赖 run() :561 同步置位 m_running=true。
SubAgent :130 纯异步，零改动。

## 2. 目标时序

**召回（①）**：run() 同步段不变（m_running=true 位置 :561 红线不动）→ 异步召回（AsyncRequest）→ 回调内 `if(!m_running) return;` → parse/失败走关键词兜底（MemoryManager :707）或空注入 → rebuildSystemPromptMessage → startChatRequest。召回回调→startChatRequest 是唯一续延路径。

**沉淀/整理（③④）→ finished 之后 fire-and-forget**：终局重排为 cron finalize→m_running=false→persistHistory→emit finished→emit memoryPhaseStarted→startMemoryChain()。每 AgentLoop 一个单槽队列（m_memoryChainActive + m_memoryChainPending）：链在跑则置 pending 返回；chainDone 时若 pending 且 !m_running 立即起下一条、若 pending 且 m_running 则丢弃（尽力而为）。同会话 extract→consolidate 严格串行；与新一轮召回并发可接受（召回只读、extract 追加写、consolidate 破坏段同步原子，单线程文件视图一致）。

**压缩（②⑤⑥）→ sendChat 前完成的续延风格**：统一拆为「同步前置段 → 异步 summarize → 回调内同步后置段 + 卫兵 + 原续延」。② applyCompactPipelineAsync(next)：四段本地管线同步跑，仅 compactHistory 触发时挂起；⑤ reactiveCompactAsync：writeTranscript+retreatToolBatch 同步→异步 summarize→回调卫兵→原序 :728-732；⑥ compactHistoryAsync：卫兵→persistHistory→startChatRequest，红线 :831-833 不动。侧链从不增量改 m_messages，只在既有检查点整体替换 → 配对协议安全。

## 3. API 草案

### 3.1 QOpenAi::AsyncRequest（P0）
```cpp
class AsyncRequest : public QObject {  // namespace QOpenAi 内
    Q_OBJECT
public:
    // 一次性异步文本请求：内部复用 ChatStream（流式组装），只提取正文 content。
    // done 恰好调用一次；error 空串=成功；超时/取消/配置缺失折叠为 error 字符串。
    // totalTimeoutMs 对齐原 blockingTimeout（默认 120000；<=0 仅依赖 ChatStream idle 超时）。
    static AsyncRequest *sendText(const QJsonObject &input, QObject *parent,
                                  std::function<void(const QString &content, const QString &error)> done,
                                  int totalTimeoutMs = 120000);
    void cancel();  // 取消后 done 永久静默；deleteLater 自清理
private:
    // ChatStream* m_stream（子对象）；QTimer m_totalTimer；bool m_done 防重入（镜像 ChatStream :502）
};
```
选择复用 ChatStream 而非加 stream=false：两侧链只消费 content，ChatStream 自带重试（:474-490）/idle 超时/配置缺失延迟 error（:241-245）/析构 abort reply（:547-555）。调用方持 QPointer<AsyncRequest> 即安全。

### 3.2 MemoryManager 异步三方法（非 QObject 形态保留，回调注入风格一致）
```cpp
void loadMemoriesAsync(const QVector<QJsonObject> &conversation, QObject *ctx,
                       std::function<void(const QString &recalled)> done) const;   // 内部已兜底，done 恒收到可用文本（可空）
void extractMemoriesAsync(const QVector<QJsonObject> &conversation, QObject *ctx,
                          std::function<void(int stored)> done) const;              // 0=跳过/失败
void consolidateMemoriesAsync(const QVector<QJsonObject> &conversation, QObject *ctx,
                              std::function<void(int consolidated)> done) const;    // 阈值未达/失败恒 0 不起 LLM
```
ctx=生命周期锚：AsyncRequest::sendText(..., ctx, ...) + connect 带 context。内部拆「prompt 构建段/结果处理段」为私有同步方法，LLM 夹中间走 AsyncRequest；写文件段 :808-841、替换段 :926-984 原样保留同步。同步版三方法迁移期共存，P4 删。

### 3.3 CompactManager Async 变体
```cpp
using CompactDone = std::function<void()>;  // 恒调用一次；失败内部折叠 "(empty summary)"（:471-485 语义不变）
void prepareAsync(..., QObject *ctx, std::function<void(bool changed)> done) const;  // changed=true 表示发生 compactHistory 级替换
void compactHistoryAsync(..., QObject *ctx, CompactDone done) const;
void reactiveCompactAsync(..., QObject *ctx, CompactDone done) const;
```
拆分点唯一：summarizeHistory 改 summarizeHistoryAsync(input, ctx, done(summaryText))，失败回调空串→占位降级。前四段纯本地管线不动。

### 3.4 AgentLoop 新增
```cpp
QPointer<QObject> m_sideRequest;    // 召回/压缩共用（m_running 期间同一时刻至多一条前链）
QPointer<QObject> m_memoryRequest;  // 记忆链（finished 后独立于主链生命周期）
bool m_memoryChainActive = false;
bool m_memoryChainPending = false;
// 新信号：
void memoryChainFinished();   // ChatSessionPage 关记忆 live 卡
void runningChanged(bool);    // 会话级禁发送（替代全局 BlockingGate）
```
stop()（:2036-2085）cancel m_currentStream 后追加 cancel m_sideRequest；m_memoryRequest 不 cancel（fire-and-forget，注释明示）。析构无需改：QPointer 对象 parent 到 this，ChatStream 析构自动 abort。新私有槽 startMemoryChain()/onMemoryChainDone() 承载 §2 队列。

### 3.5 ChatSessionPage 两处
- finished handler（:116-131）：memoryPhaseStarted~memoryChainFinished 窗口内跳过 m_currentBubble 置空（:122-126），memoryChainFinished 里 finishStreaming+置空；消除 :168-171 独立噪声气泡兜底（窗口外到达仍接受兜底）。
- 订阅 runningChanged → 本页 ChatMsgEdit 禁发送（会话级，替代全局 Gate）。

## 4. 分阶段计划

| 阶段 | 内容 | 文件 | 规模 | 验证 |
|---|---|---|---|---|
| P0 | AsyncRequest，零接入 | QOpenAi.h/.cpp | +100 | 构建过、现有行为零变化 |
| P1 | 召回异步化 + stop cancel m_sideRequest | AgentLoop、MemoryManager | ~120 改 | 记忆注入时序不变；召回中停止→不发请求；断网→兜底照常开聊；cron 契约回归 |
| P2 | 沉淀/整理 fire-and-forget + 信号 + 页面接线 | AgentLoop、MemoryManager、ChatSessionPage | ~210 改 | finished 即达、输入即用；记忆卡挂原气泡；pending 单槽不叠加；页面析构无崩溃 |
| P3 | 压缩三处续延化（最大阶段） | CompactManager、AgentLoop | ~250 改 | 50k 自动压缩/批尾压缩/溢出反应式三路径回归；红线 :831-833 不破；压缩中 stop→历史不替换 |
| P4 | 清理：删 blockingRequest/create/blockingCreate/BlockingSession/BlockingGate/setBlockingTimeout + 同步版三方法 + ChatMsgEdit Gate 接线 | QOpenAi、MemoryManager、CompactManager、ChatMsgEdit | 净删 ~150-200 | grep 全仓无 blockingRequest/QEventLoop 嵌套残留 |

## 5. BlockingGate 处置：P4 整体废除
- 禁发送 → runningChanged(bool) 页面级（覆盖整个回合含召回期，比 Gate 只覆盖等待窗口更完整；多会话天然隔离，修掉现全局误禁缺陷；且修复今 m_running 期间输入不禁 only 事后弹 error 的缺口）。
- 应用级 WaitCursor → 废除（GUI 线程不再被劫持，等待期本就该可浏览/切页/停止）。
- P1-P3 迁移期 Gate 保留服务未迁移链，P3 后调用方归零，P4 一次删净（QOpenAi.h:62-100、QOpenAi.cpp:599-643、ChatMsgEdit.cpp:113-122）。

## 6. 风险清单与对策

1. 重入：run() 侧链飞行中被再入（cron 同栈回读）→ m_running=true 同步置于任何 await 前（:561 不动），重入卫兵 :555-558 保留，注释标契约。
2. done 双触发（error/finished 竞态）→ AsyncRequest m_done 首终态触发其余吞掉，cancel 后永久静默。
3. 页面析构链未完 → 全 AsyncRequest parent 到 AgentLoop；ctx+connect context；析构 abort。
4. 超时平移 → totalTimeoutMs=120000 singleShot→cancel→done(空,"超时")→各链既有降级路径接住；idle 超时 ChatStream 自带双保险。
5. 记忆卡气泡错位 → §3.5 窗口保留；边缘接受兜底。
6. cron 与记忆链并发 → 接受（单线程文件视图一致）；不 gate cron，注释记决策。
7. stop 语义分裂 → stop cancel m_sideRequest；续延回调首行 if(!m_running) return;（平移 :726/:754/:840 模式）；记忆链不 cancel。
8. NAM 6 连接/主机上限 → 个位数会话远低于，文档登记，不做限流（YAGNI）。
9. 配对协议 → 侧链只在检查点整体替换；P3 验证含三压缩路径+多工具批次回归。
10. persistHistory 顺序 → 新序 persistHistory→finished→记忆链；.memory 与 history.json 独立文件域，崩溃窗口最坏丢记忆不丢历史（优于现状），注释记录。

## 7. 工作量
P0 0.5d / P1 0.5-1d / P2 1d / P3 1-1.5d / P4 0.5d，合计 3.5-4.5 天，净增约 +260 行。无自动测试，验证以构建+手工场景清单。

## 8. 落地状态（落账，2026-09）
- P1 召回链异步化：已落地（commit 1256d51）。
- P2 沉淀/整理链异步化（含 runningChanged/memoryChainFinished 信号与页面接线）：已落地（commit 9563901）。
- P3 压缩三处续延化：已落地（commit dc73367）。
- P4 清理归零：已落地（本次提交）——blockingRequest / CategoryChat::create / CategoryCompletion::create / blockingCreate 族 / BlockingGate / BlockingSession / setBlockingTimeout 及同步版三方法、applyCompactPipeline、ChatMsgEdit Gate 接线全部删除；grep 全仓无阻塞族残留；禁发送唯一来源为 runningChanged 会话级信号。
- 实现偏离登记：§3.2 草案三个 Async 方法为 void，实际均返回 `QOpenAi::AsyncRequest *`（宿主句柄记账/取消所需，短路路径返回 nullptr）；consolidateMemoriesAsync 实际签名无 conversation 参数（整理链不消费对话）；§3.3 草案 prepareAsync 原地引用收 conversation，实际按值传入、done 按值交付最终 conversation（挂起跨 await 后调用方栈引用有悬挂风险，按值更安全）。三处均为实现优于草案的修正（审查 L5 补记）。
- 审查后修复（独立审查结论 0 Critical/0 High/1 Medium/5 Low）：M1 记忆结果卡加相位闸——`toolOutputReady("memory")` 在 m_memoryBubble 为空（清屏/交叠丢相位）时静默丢弃，不再生成孤儿兜底气泡（数据已落盘，纯 UI 留痕）；L1 双回合交叠窄窗口（pending 链不重发 memoryPhaseStarted、旧链 finished 抢先定稿新气泡）登记为已知限制，修它需给信号加世代参数，收益不配成本；L2-L4 陈旧注释/缩进/行号引用修正。
