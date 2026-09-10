#include "multiseatdiscovery.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include "nvhttp.h"

MultiSeatDiscovery::MultiSeatDiscovery(QObject* parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_Timer(new QTimer(this))
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

void MultiSeatDiscovery::setHostsToProbe(const QStringList& addresses)
{
    m_Hosts = addresses;
}

void MultiSeatDiscovery::poll()
{
    // Refresh the address list first — hosts may have been added or removed since the last tick.
    emit aboutToPoll();

    // Nothing to probe until the user has a host. That is the right dependency: a seat lives on
    // the same machine as the Apollo they already added, so its address is already known.
    for (const QString& host : std::as_const(m_Hosts)) {
        for (int seat = 0; seat < MAX_SEATS_PROBED; seat++) {
            uint16_t port = static_cast<uint16_t>(SEAT_PORT_BASE + seat * SEAT_PORT_STRIDE);

            QString key = QStringLiteral("%1:%2").arg(host).arg(port);
            if (m_InFlight.contains(key)) {
                // Still waiting on the previous probe of this endpoint. Skipping avoids stacking
                // one request per tick against something slow or firewalled.
                continue;
            }

            QUrl url;
            url.setScheme(QStringLiteral("http"));
            url.setHost(host);
            url.setPort(port);
            url.setPath(QStringLiteral("/serverinfo"));

            QNetworkRequest request(url);
            request.setAttribute(QNetworkRequest::ConnectionEncryptedAttribute, false);
            // Do not let a probe hold a connection open; most ports probed will be closed.
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                 QNetworkRequest::AlwaysNetwork);

            m_InFlight.insert(key);
            m_Nam->get(request);
        }
    }
}

void MultiSeatDiscovery::handleReply(QNetworkReply* reply)
{
    reply->deleteLater();

    const QUrl url = reply->url();
    const QString host = url.host();
    const uint16_t port = static_cast<uint16_t>(url.port());
    m_InFlight.remove(QStringLiteral("%1:%2").arg(host).arg(port));

    if (reply->error() != QNetworkReply::NoError) {
        // Expected for every port with no seat on it, which is most of them. Staying quiet here
        // is correct — unlike the old code, silence now means "no seat on this port", not "the
        // whole mechanism could never work".
        return;
    }

    // Apollo answers /serverinfo with XML. Read the hostname; it is what tells a seat apart from
    // an unrelated Apollo that happens to sit on one of these ports.
    //
    // Reuses NvHTTP's parser rather than opening a second QXmlStreamReader on the same shape of
    // document — one place to be wrong about Apollo's XML is enough.
    const QString hostname = NvHTTP::getXmlString(QString::fromUtf8(reply->readAll()),
                                                  QStringLiteral("hostname"));

    if (hostname.isEmpty()) {
        return;
    }

    if (!hostname.startsWith(QLatin1String(SEAT_NAME_PREFIX))) {
        // Something is serving here, but it is not a MultiSeat seat. Leave it alone rather than
        // adding a host the user did not ask for.
        return;
    }

    emit seatFound(host, port, hostname);
}
