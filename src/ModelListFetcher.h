#pragma once

// ============================================================================
// 远端模型清单拉取器（s18.3 B 项）
//
// 【只做搬运，不做判据】
// 「怎么从响应里取出模型名」的全部判据住在 ProviderSpec::parseModelsResponse，那边是纯
// QtCore 函数、已被 tst_providerspec 覆盖（三种响应形态 / id 与 name 优先级 / 去重 /
// 上限 / 坏 JSON 给空表不崩）。本类因此只负责「发一个 GET、把字节交出去、保证回调恰好
// 一次」，没有可判错的算法——这也是它能待在网络层文件里的前提：它链 Qt6::Network，
// 进不了只链 Qt6::Core 的测试目标，若把判据写进来就等于把不可测的面积扩大。
//
// 【为什么是独立一次性请求而不复用 ChatStream】
// ChatStream 是 SSE 流式客户端，绑定了 /chat/completions 的帧解析、退避重试与换模状态；
// /models 是一次普通 GET，套进去只会让两条语义不同的链互相牵连（例如 529 换模对拉列表
// 毫无意义）。故这里直接持一支自己的 QNetworkAccessManager，与 AsyncRequest 的
// 「done 恰好一次」契约对齐：成功 / HTTP 错误 / 网络错误 / 超时 四条终态路径共用一个
// 首到门闩，迟到路径吞掉。
//
// 【失败必须可见但不弹窗】
// 拉取失败是常态而非异常：端点可能不实现 /models、可能 401、可能是内网自签证书。调用方
// （设置页）把 error 文本显示在按钮旁即可，用户仍可回到手填路径——这正是 ProviderSpec.h
// 头注释里「拉取结果仅供参考 + 仍可手填」的落地形态。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace ModelFetch {

// 一次性 GET {baseUrl}/models。token 为空即不带 Authorization 头（部分本地网关如 Ollama
// 无鉴权也能列模型），非空则按 OpenAI 兼容口径带 Bearer。
// done(models, error)：error 空串 = 成功；两者不会同时有值。
// parent 即生命周期锚——锚析构时请求作废、done 永不再触发（与 AsyncRequest 同口径）。
// 返回值是本次请求对象，调用方通常丢弃；需要提前取消时持它调 cancel()。
class Request : public QObject
{
    Q_OBJECT
public:
    static Request *fetch(const QString &baseUrl,
                          const QString &token,
                          QObject *parent,
                          std::function<void(const QStringList &models, const QString &error)> done,
                          int timeoutMs = 15000);

    void cancel();

private:
    explicit Request(QObject *parent);
    void start(const QString &baseUrl, const QString &token, int timeoutMs);
    void onFinished();
    // 终态唯一出口：首到生效，后到吞掉；交付后自清理
    void fire(const QStringList &models, const QString &error);

    QNetworkAccessManager *m_manager = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_timer = nullptr;
    std::function<void(const QStringList &, const QString &)> m_done;
    bool m_fired = false;
};

} // namespace ModelFetch
