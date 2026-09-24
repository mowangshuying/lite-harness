#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

/**
 * MemoryManager —— 持久记忆系统（对齐 lcc s09 memory_manager.py 的 MemoryManager）
 *
 * 非 QObject、无信号：存储位于 <会话数据根>/.memory/（会话数据根由宿主注入的 workDirSink
 * 提供，含 .lite-harness 或按会话隔离的 .lite-harness/sessions/<id>；MEMORY.md 索引 +
 * <slug>.md 记录文件，
 * 记录带极简 frontmatter，键序固定 name/description/type）。三大公共入口与 lcc 一致：
 *   - loadMemories：召回——LLM 从目录中挑选与最近请求相关的记录（失败回落关键词打分），
 *     拼接为 JSON 文本注入 system prompt 尾段；
 *   - extractMemories：对话自然结束后的沉淀提取（scope==persistent 门槛 + 临时标记 + 三重去重）；
 *   - consolidateMemories：记录数达到阈值后的整体合并（快照-回滚语义）。
 *
 * 与宿主（AgentLoop）的耦合仿 CompactManager：构造函数注入 workDir/model 回调（目录随
 * setWorkDir 动态跟随），卡片经 CardSink 回传（宿主复用三参 toolOutputReady，"memory" 名义），
 * 零新增公共信号。提取/合并/召回内部均为阻塞式 LLM 调用，宿主须在其"无活动流/权限挂起/
 * 子代理"的窗口调用并事后复验运行标志（lcc s08 裁决 f 同款豁免）。
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

    /** 召回（lcc load_memories）：选相关记录、按 RECALL_CHAR_LIMIT 截断拼接；空存储零 LLM 调用。
     *  返回 [{source, content}] 的 JSON 文本（无命中返回空串），供 system prompt 尾段。 */
    QString loadMemories(const QVector<QJsonObject> &conversation) const;

    /** 提取（lcc extract_memories）：从最近对话提取持久记忆并落盘，返回写入条数；
     *  任何失败（LLM/落盘）走 lcc 的 skipped 降级路径，返回 0。 */
    int extractMemories(const QVector<QJsonObject> &conversation) const;

    /** 合并（lcc consolidate_memories）：记录数 >= 阈值时以 LLM 重写整个存储（快照回滚），
     *  返回合并后条数；未达阈值或失败返回 0。 */
    int consolidateMemories() const;

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
    QStringList selectRelevantMemories(const QVector<QJsonObject> &messages) const;

    // ---- 阻塞式摘要/选择调用（lcc client.messages.create 的 OpenAI 形态等价） ----
    QString blockingCreate(const QString &prompt, int maxTokens, bool *ok, QString *error) const;

    void emitCard(const QString &summary, const QString &output) const;

    WorkDirSink m_workDirSink;
    ModelSink m_modelSink;
    CardSink m_cardSink;
};
