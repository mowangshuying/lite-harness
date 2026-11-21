#include "ModelListFetcher.h"

#include "ProviderSpec.h" // parseModelsResponse：响应形态的全部判据住在那边（已被 ctest 覆盖）

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace ModelFetch {

Request *Request::fetch(const QString &baseUrl,
                        const QString &token,
                        QObject *parent,
                        std::function<void(const QStringList &, const QString &)> done,
                        int timeoutMs)
{
    auto *req = new Request(parent);
    req->m_done = std::move(done);
    req->start(baseUrl, token, timeoutMs);
    return req;
}

Request::Request(QObject *parent) : QObject(parent)
{
}

void Request::start(const QString &baseUrl, const QString &token, int timeoutMs)
{
    // 基址为空 / 非 http(s)：不发请求，直接以错误终态交付。
    // 判据复用 ProviderSpec::isValidBaseUrl——设置页录入校验与这里必须是同一把尺子，
    // 否则会出现「设置页存进去了、拉取时才发现打不出去」。
    if (baseUrl.trimmed().isEmpty() || !ProviderSpec::isValidBaseUrl(baseUrl))
    {
        // 延迟一拍：与 QOpenAi 同口径，调用方此刻往往还没 connect 完
        QTimer::singleShot(0, this, [this] {
            fire(QStringList(), tr("服务地址未设置或不合法（需以 http/https 开头）。"));
        });
        return;
    }

    const QUrl url(ProviderSpec::modelsEndpointFor(baseUrl));
    QNetworkRequest netReq{url};
    netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!token.trimmed().isEmpty())
        netReq.setRawHeader("Authorization", QByteArray("Bearer ") + token.trimmed().toUtf8());

    m_manager = new QNetworkAccessManager(this);
    m_reply = m_manager->get(netReq);
    connect(m_reply, &QNetworkReply::finished, this, &Request::onFinished);

    // 总超时：列端点属轻量 GET，15s 不给第二次机会——拉不到就让用户回手填路径，
    // 挂在那里转圈比失败更难接受。
    if (timeoutMs > 0)
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (m_reply)
                m_reply->abort(); // abort 触发 finished → onFinished 折叠成错误终态
        });
        m_timer->start(timeoutMs);
    }
}

void Request::onFinished()
{
    if (!m_reply)
        return;
    if (m_timer)
        m_timer->stop();

    const QNetworkReply::NetworkError netErr = m_reply->error();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = m_reply->readAll();
    const QString replyText = m_reply->errorString();

    QStringList models;
    QString error;

    if (netErr != QNetworkReply::NoError)
    {
        // 超时（abort）与真错误都走这里；按 status 是否拿到过区分文案，
        // 让用户分得清「打不通」和「通了但服务端报错」——两者的下一步动作完全不同。
        error = status > 0
            ? tr("服务端返回错误（HTTP %1）。").arg(status)
            : tr("请求未能完成：%1").arg(replyText.isEmpty() ? tr("网络不可达。") : replyText);
    }
    else
    {
        models = ProviderSpec::parseModelsResponse(payload);
        if (models.isEmpty())
        {
            // 200 但一个名字都没解析出来：多半是该端点不实现 /models（返回 HTML 错误页、
            // 空 data、或本仓未覆盖的第四种形态）。如实说「没取到」而不是报成功，
            // 否则界面会显示一个空列表，用户以为这家没有模型。
            error = tr("端点未返回可用的模型清单（可能不支持 /models）。");
        }
    }

    fire(models, error);
}

void Request::fire(const QStringList &models, const QString &error)
{
    if (m_fired)
        return; // 首到门闩：超时 abort 与 finished 可能先后到，只交一次
    m_fired = true;

    if (m_timer)
        m_timer->stop();
    if (m_reply)
    {
        m_reply->disconnect(this);
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    auto handler = std::move(m_done);
    m_done = nullptr;
    if (handler)
        handler(models, error);

    deleteLater();
}

void Request::cancel()
{
    // 取消后 done 永久静默（与 AsyncRequest::cancel 同契约）：先立门闩再断链，
    // 否则 abort 触发的 finished 仍会把一个「取消」当成结果交付出去。
    m_fired = true;
    if (m_timer)
        m_timer->stop();
    if (m_reply)
    {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_done = nullptr;
    deleteLater();
}

} // namespace ModelFetch
