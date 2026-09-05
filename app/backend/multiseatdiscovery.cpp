#include "multiseatdiscovery.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

MultiSeatDiscovery::MultiSeatDiscovery(QObject* parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_Timer(new QTimer(this)),
      m_RequestPending(false)
{
    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &MultiSeatDiscovery::handleReply);

    m_Timer->setInterval(POLL_INTERVAL_MS);
    m_Timer->setSingleShot(false);
    connect(m_Timer, &QTimer::timeout, this, &MultiSeatDiscovery::poll);
}

MultiSeatDiscovery::~MultiSeatDiscovery()
{
    stop();
}

void MultiSeatDiscovery::start()
{
    m_Timer->start();
    // Poll immediately on start, don't wait for first interval
    poll();
}

void MultiSeatDiscovery::stop()
{
    m_Timer->stop();
}

void MultiSeatDiscovery::poll()
{
    if (m_RequestPending) {
        return;
    }

    QUrl url;
    url.setScheme("http");
    url.setHost("127.0.0.1");
    url.setPort(MULTISEAT_API_PORT);
    url.setPath("/api/seats");

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::ConnectionEncryptedAttribute, false);

    m_RequestPending = true;
    m_Nam->get(request);
}

void MultiSeatDiscovery::handleReply(QNetworkReply* reply)
{
    m_RequestPending = false;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // MultiSeat service not running — silent failure, will retry
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (doc.isNull() || !doc.isArray()) {
        qWarning() << "MultiSeat: invalid seats response:" << parseError.errorString();
        return;
    }

    QJsonArray seats = doc.array();
    for (const QJsonValue& val : std::as_const(seats)) {
        if (!val.isObject()) continue;
        QJsonObject seat = val.toObject();

        QString status = seat["status"].toString();
        // Only expose seats that have Apollo running and ready for streaming
        if (status != "Ready" && status != "Streaming") continue;

        int portBase = seat["portBase"].toInt(0);
        if (portBase <= 0) continue;

        QString accountName = seat["accountName"].toString();
        QString displayName = accountName.isEmpty()
            ? QStringLiteral("MultiSeat Seat")
            : QStringLiteral("MultiSeat - %1").arg(accountName);

        emit seatFound("127.0.0.1", static_cast<uint16_t>(portBase), displayName);
    }
}
