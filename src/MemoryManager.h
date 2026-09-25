#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace QOpenAi { class AsyncRequest; }

/**
 * MemoryManager —— 持久记忆系统（对齐 lcc s09 memory_manager.py 的 MemoryManager）
 *
 * 非 QObject、无信号：存储位于 <会话数据根>/.memory/（会话数据根由宿主注入的 workDirSink
 * 提供，含 .lite-harness 或按会话隔离的 .lite-harness/sessions/<id>；MEMORY.md 索引 +
 * <slug>.md 记录文件，
 * 记录带极简 frontmatter，键序固定 name/description/type）。三大公共入口语义与 lcc 一致
 * （异步化迁移后 Async 版为唯一路径，设计文档 docs/async-chain-design.md）：
 *   - loadMemoriesAsync：召回——LLM 从目录中挑选与最近请求相关的记录（失败回落关键词打分），
 *     拼接为 JSON 文本注入 system prompt 尾段；
 *   - extractMemoriesAsync：对话自然结束后的沉淀提取（scope==persistent 门槛 + 临时标记 + 三重去重）；
 *   - consolidateMemoriesAsync：记录数达到阈值后的整体合并（快照-回滚语义）。
 *
 * 与宿主（AgentLoop）的耦合仿 CompactManager：构造函数注入 workDir/model 回调（目录随
 * setWorkDir 动态跟随），卡片经 CardSink 回传（宿主复用三参 toolOutputReady，"memory" 名义），
 * 零新增公共信号。三条链全部走 QOpenAi::AsyncRequest（召回 P1、提取/合并 P2），
 * 阻塞链族与迁移期兼容壳已随 P4 整体删除，宿主无需再守阻塞窗口。
 *
 * 无 PyYAML 依赖：frontmatter 采用极简解析（仅支持单行平铺标量），见 .cpp 偏差注释。
 */
class MemoryManager
{
public:
    using WorkDirSink = std::function<QString()>;
    using ModelSink = std::function<QString()>;
    using CardSink = std::function<void(const QString &summary, const QString &output)>;

    MemoryManager(WorkDirSink workDirSink, ModelSink modelSink);

    void setCardSink(CardSink sink);

    /** 召回异步版（P1，设计文档 §3.2）：语义与原阻塞链逐字一致，仅 LLM 选择段走
     *  QOpenAi::AsyncRequest（本函数立即返回，续延在回调线程＝主线程事件循环交付）。
     *  done 恒恰好调用一次且恒收到可用文本（可空串）：请求失败/超时内部降级为关键词
     *  打分兜底（原同步链 lcc except 路径同款），不抛不卡。ctx 为生命周期锚：请求 parent
     *  到 ctx、内部连接以 ctx 为 context——ctx 析构即链作废、done 永久静默（宿主契约：
     *  ctx 存活期间 done 必达一次）。
     *  返回在途 AsyncRequest 供宿主 cancel（§3.4 m_sideRequest 形态）；空存储/无近期
     *  user 消息走短路（零 LLM 调用、done 同步完成后返回 nullptr）。 */
    QOpenAi::AsyncRequest *loadMemoriesAsync(const QVector<QJsonObject> &conversation,
                                             QObject *ctx,
                                             std::function<void(const QString &recalled)> done) const;

    /** 提取异步版（P2，设计文档 §2.2/§3.2）：语义与原同步链逐字一致，仅 LLM 调用段走
     *  QOpenAi::AsyncRequest（本函数立即返回，续延在主线程事件循环交付）。prompt 构建与
     *  校验/去重/落盘段沿用原同步链拆分出的同一组私有方法（同步族已随 P4 删除）。
     *  done 恒恰好调用一次且恒收到可用计数：请求失败/超时折叠为 done(0)（原同步链 skipped
     *  降级路径同款 qWarning 日志，尽力而为语义——不抛不卡不重试）。ctx 为生命周期锚：
     *  请求 parent 到 ctx、回调以 ctx 为 context，ctx 析构即链作废、done 永久静默。
     *  偏离 §3.2 草案（返回 void）：与 loadMemoriesAsync 同范式返回在途 AsyncRequest 供
     *  宿主记账句柄（§3.4 m_memoryRequest）；空对话走短路（零 LLM 调用、done 同步完成后
     *  返回 nullptr）。对对话仅只读：调用方在途期间自由变更/清空。 */
    QOpenAi::AsyncRequest *extractMemoriesAsync(const QVector<QJsonObject> &conversation,
                                                QObject *ctx,
                                                std::function<void(int stored)> done) const;

    /** 合并异步版（P2，设计文档 §2.2/§3.2）：语义与原同步链逐字一致，仅 LLM 调用段走
     *  QOpenAi::AsyncRequest。阈值判断/prompt 构建（含超尺寸护栏）在发起段同步完成，
     *  快照/破坏性替换/回滚段在回调内同步执行（.memory/ 文件的读-删-写全程无 await 点，
     *  原子性与原同步链等价）。链不消费对话，故签名无 conversation 参数（偏离 §3.2 草案，
     *  与原同步链对齐）。未达阈值/超尺寸/失败/超时均折叠为 done(0)（降级日志与原同步链同款）。
     *  done 恒恰好调用一次；ctx 锚与返回句柄语义同 extractMemoriesAsync。 */
    QOpenAi::AsyncRequest *consolidateMemoriesAsync(QObject *ctx,
                                                    std::function<void(int consolidated)> done) const;

    /** 读取 MEMORY.md 索引文本（lcc read_memory_index）：不存在返回空串 */
    QString readMemoryIndex() const;

private:
    // 一条记录文件的解析视图（lcc list_memory_files 的 dict 等价）
    struct MemoryRecord
    {
        QString filename;
        QString name;
        QString description;
        QString type;
        QString body;
    };

    // ---- 存储路径与安全（lcc memory_path 的 ValueError 族改为 bool + error 出参） ----
    QString storeDir() const;
    QString indexFilePath() const;
    bool memoryPathSafe(const QString &filename, bool allowIndex,
                        QString *resolvedPath, QString *error) const;

    // ---- 解析与序列化（lcc parse_frontmatter / memory_slug / memory_document / ...） ----
    static bool parseFrontmatter(const QString &text, QJsonObject *metadata, QString *body);
    static QString memorySlug(const QString &name);
    static QString normalizedMemoryText(const QString &value);
    static QString memoryDocument(const QString &name, const QString &type,
                                  const QString &description, const QString &body);
    static QString messageText(const QJsonObject &message);
    static QJsonArray extractJsonArray(const QString &text);
    static bool validateMemoryRecord(const QJsonObject &record, bool requireScope,
                                     QJsonObject *out);
    static bool shouldStoreMemory(const QJsonObject &candidate,
                                  const QVector<MemoryRecord> &existing);

    // ---- 存储读写（lcc list/write/rebuild/read 系列） ----
    QVector<MemoryRecord> listMemoryFiles() const;
    QString readMemoryFile(const QString &filename) const;
    bool writeMemoryFile(const QString &name, const QString &type,
                         const QString &description, const QString &body,
                         QString *error) const;
    void rebuildMemoryIndex() const;

    // ---- 召回链（lcc recent_user_text / keyword_memory_selection / select_relevant_memories / dialogue_text） ----
    static QString recentUserText(const QVector<QJsonObject> &messages, int maxTurns = 3);
    QString dialogueText(const QVector<QJsonObject> &messages) const;
    static QStringList keywordMemorySelection(const QVector<MemoryRecord> &records,
                                              const QString &query, int maxItems);
    // ---- 召回链三段拆分（P1 异步化）：构建 prompt / 解析 LLM 选择 / 拼接注入文本 ----
    static QString buildRecallPrompt(const QVector<MemoryRecord> &records, const QString &query);
    static QStringList parseRecallSelection(const QString &reply, const QVector<MemoryRecord> &records);
    QString formatRecalled(const QStringList &selected) const;
    // ---- 提取链/合并链三段拆分（P2 异步化）：构建 prompt / 处理 LLM 回复（含落盘/回滚） ----
    static QString buildExtractPrompt(const QString &dialogue, const QVector<MemoryRecord> &records);
    int processExtractReply(const QString &reply, QVector<MemoryRecord> existingRecords) const;
    static QString buildConsolidatePrompt(const QVector<MemoryRecord> &records);
    int applyConsolidateReply(const QString &reply, const QVector<MemoryRecord> &records) const;

    void emitCard(const QString &summary, const QString &output) const;

    WorkDirSink m_workDirSink;
    ModelSink m_modelSink;
    CardSink m_cardSink;
};
