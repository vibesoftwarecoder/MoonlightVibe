// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include "usbforwardingtunnel.h"
#include "usbforwardingcapability.h"
#include "pairedcertificate.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QTcpSocket>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#endif

namespace UsbForwarding {

namespace {
/* Stop reading from one side while the other side is this far behind. TCP
 * back-pressures the USB/IP peer instead of us buffering without limit. */
constexpr qint64 kHighWaterMark = 4 * 1024 * 1024;

#ifdef Q_OS_WIN
/* Older SDK headers lack these; the numbers are fixed by Windows 10 1709+. */
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif

bool setTcpInt(SOCKET s, int option, int value)
{
    return ::setsockopt(s, IPPROTO_TCP, option, reinterpret_cast<const char *>(&value), sizeof(value)) == 0;
}

int getTcpInt(SOCKET s, int level, int option)
{
    int value = 0;
    int length = sizeof(value);
    if (::getsockopt(s, level, option, reinterpret_cast<char *>(&value), &length) != 0) {
        return 0;
    }
    return value;
}
#endif

/* Turn on keepalive with the configured timing and report what the OS holds. */
KeepAliveReport applyKeepAlive(QAbstractSocket *socket, const TunnelConfig &config)
{
    KeepAliveReport report;
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
#ifdef Q_OS_WIN
    const SOCKET s = static_cast<SOCKET>(socket->socketDescriptor());
    if (s != INVALID_SOCKET) {
        const bool timed = setTcpInt(s, TCP_KEEPIDLE, config.keepAliveIdleSec) &&
                           setTcpInt(s, TCP_KEEPINTVL, config.keepAliveIntervalSec) &&
                           setTcpInt(s, TCP_KEEPCNT, config.keepAliveCount);
        if (!timed) {
            /* Pre-1709 fallback: idle and interval only; the probe count is fixed. */
            tcp_keepalive vals = {};
            vals.onoff = 1;
            vals.keepalivetime = static_cast<ULONG>(config.keepAliveIdleSec) * 1000;
            vals.keepaliveinterval = static_cast<ULONG>(config.keepAliveIntervalSec) * 1000;
            DWORD returned = 0;
            ::WSAIoctl(s, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0, &returned, nullptr, nullptr);
        }
        report.enabled = getTcpInt(s, SOL_SOCKET, SO_KEEPALIVE) != 0;
        report.idleSec = getTcpInt(s, IPPROTO_TCP, TCP_KEEPIDLE);
        report.intervalSec = getTcpInt(s, IPPROTO_TCP, TCP_KEEPINTVL);
        report.count = getTcpInt(s, IPPROTO_TCP, TCP_KEEPCNT);
        return report;
    }
#else
    Q_UNUSED(config);
#endif
    report.enabled = socket->socketOption(QAbstractSocket::KeepAliveOption).toInt() != 0;
    return report;
}

/* The host's error text is untrusted: it must never reach the user as free
 * text (it could be markup or an attempt to impersonate system messages).
 * Only a few known codes are mapped; everything else gets a generic message. */
QString describeHostReason(const QString &reason)
{
    const QString code = reason.trimmed().toLower();
    if (code == QLatin1String("disabled")) {
        return Tunnel::tr("USB forwarding is disabled on the host.");
    }
    if (code == QLatin1String("unavailable")) {
        return Tunnel::tr("USB forwarding is not available on the host.");
    }
    if (code == QLatin1String("unauthorized") || code == QLatin1String("forbidden")) {
        return Tunnel::tr("The host did not allow USB forwarding for this device.");
    }
    if (code == QLatin1String("busy")) {
        return Tunnel::tr("That USB device is already in use on the host.");
    }
    return QString();
}
} // namespace

bool TunnelConfig::valid() const noexcept
{
    if (host.isEmpty() || port == 0 || sessionToken.isEmpty() || busId.isEmpty() ||
        localPort == 0) {
        return false;
    }
    /* Mirror the server's valid_busid(): [0-9A-Za-z.:-], at most 31 bytes. */
    if (!isValidBusId(busId)) {
        return false;
    }
    /* The host authenticates us by client certificate: refuse to try without one. */
    if (sslConfiguration.localCertificate().isNull() || sslConfiguration.privateKey().isNull()) {
        return false;
    }
    if (startupTimeoutMs <= 0) {
        return false;
    }
    if (keepAliveIdleSec <= 0 || keepAliveIntervalSec <= 0 || keepAliveCount <= 0) {
        return false;
    }
    /* The local hop is plaintext USB/IP: it must stay on this machine. */
    if (!QHostAddress(localHost).isLoopback()) {
        return false;
    }
    return true;
}

/* Socket I/O must continue while Session's SDL loop owns the GUI thread.
 * Keep the transport and its timers together on one dedicated event loop.
 * Design from aldobarr's qiin2333/moonlight-qt#241 IO-thread rework, adopted after his
 * mouse-latency measurements reproduced on qiin2333/moonlight-qt#242. User-facing strings stay
 * in Tunnel's tr() context so existing translations keep matching. */
class TunnelWorker final : public QObject
{
    Q_OBJECT

public:
    explicit TunnelWorker(TunnelConfig config, QObject *parent = nullptr);
    ~TunnelWorker() override;

    bool start(QString *error = nullptr);
    void stop() noexcept;

signals:
    void forwarding();
    void finished(QString message);

private:
    void handleRemoteReadyRead();
    void handleLocalReadyRead();
    void failWith(const QString &message);
    void finishCleanly();

    TunnelConfig m_Config;
    QTcpSocket *m_Local = nullptr;
    QSslSocket *m_Remote = nullptr;
    QTimer *m_StartupTimer = nullptr;
    QByteArray m_HandshakeBuffer;
    bool m_HandshakeDone = false;
    bool m_PeerVerified = false;
    bool m_Finished = false;
};

TunnelWorker::TunnelWorker(TunnelConfig config, QObject *parent)
    : QObject(parent), m_Config(std::move(config))
{
    m_StartupTimer = new QTimer(this);
    m_StartupTimer->setSingleShot(true);
    connect(m_StartupTimer, &QTimer::timeout, this,
            [this] { failWith(Tunnel::tr("The USB tunnel connection timed out.")); });
}

TunnelWorker::~TunnelWorker()
{
    stop();
}

bool TunnelWorker::start(QString *error)
{
    if (!m_Config.valid()) {
        if (error != nullptr) {
            *error = Tunnel::tr("The USB tunnel configuration is incomplete.");
        }
        return false;
    }
    if (m_Local != nullptr || m_Remote != nullptr) {
        if (error != nullptr) {
            *error = Tunnel::tr("The USB tunnel is already running.");
        }
        return false;
    }

    if (m_Config.pinnedServerCertificate.isNull()) {
        /* A forwarded USB device is a high-trust channel: never fall back to
         * default CA verification. Require the cert pinned at pairing time. */
        if (error != nullptr) {
            *error = Tunnel::tr("Pair with this host before forwarding USB devices.");
        }
        return false;
    }

    m_Local = new QTcpSocket(this);
    m_Remote = new QSslSocket(this);
    m_Finished = false;
    m_HandshakeDone = false;
    m_PeerVerified = false;
    m_HandshakeBuffer.clear();
    m_Local->setReadBufferSize(kHighWaterMark);
    m_Remote->setReadBufferSize(kHighWaterMark);

    /* Verification is manual (mirrors nvhttp): Qt's chain/hostname checks
     * cannot express "dial by IP + trust exactly this generic-CN self-signed
     * certificate", so the handshake runs unverified and the encrypted
     * callback below rejects anything whose DER differs from the pin. */
    QSslConfiguration sslConfig = m_Config.sslConfiguration;
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    m_Remote->setSslConfiguration(sslConfig);

    connect(m_Remote, &QSslSocket::connected, this, [this] {
        /* USB interrupt reports must not wait for Nagle's algorithm and a
         * delayed peer ACK. Set this after the native socket exists. */
        m_Remote->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    });
    connect(m_Remote, &QSslSocket::encrypted, this, [this] {
        if (!PairedCertificate::matches(m_Config.pinnedServerCertificate,
                                        m_Remote->peerCertificate())) {
            failWith(Tunnel::tr("The host certificate was rejected: unexpected certificate"));
            return;
        }
        m_PeerVerified = true;
        /* The handshake is the only Moonlight-owned protocol on this socket. */
        QJsonObject request{
            { QStringLiteral("op"), QStringLiteral("forward") },
            { QStringLiteral("token"), QString::fromUtf8(m_Config.sessionToken) },
            { QStringLiteral("busid"), QString::fromUtf8(m_Config.busId) },
        };
        QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
        line.append('\n');
        m_Remote->write(line);
    });
    connect(m_Remote, &QSslSocket::readyRead, this, &TunnelWorker::handleRemoteReadyRead);
    connect(m_Remote, &QSslSocket::disconnected, this, [this] { finishCleanly(); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_Remote, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError socketError) {
        if (m_HandshakeDone && (socketError == QAbstractSocket::SocketTimeoutError ||
                                  socketError == QAbstractSocket::NetworkError)) {
            /* Keepalive probes went unanswered: the host is gone. */
            failWith(Tunnel::tr("The host stopped responding."));
            return;
        }
        failWith(
            Tunnel::tr("The connection to the host was lost: %1").arg(m_Remote->errorString()));
    });
#else
    connect(
        m_Remote, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
        [this](QAbstractSocket::SocketError) {
            failWith(
                Tunnel::tr("The connection to the host was lost: %1").arg(m_Remote->errorString()));
        });
#endif
    connect(m_Remote, QOverload<const QList<QSslError> &>::of(&QSslSocket::sslErrors), this,
            [this](const QList<QSslError> &errors) {
                // Pairing pins the exact certificate, independent of hostname/CA
                // validity. Never ignore errors for a different peer certificate.
                if (PairedCertificate::matches(m_Config.pinnedServerCertificate,
                                               m_Remote->peerCertificate())) {
                    m_Remote->ignoreSslErrors(errors);
                    return;
                }
                QStringList messages;
                for (const QSslError &sslError : errors) {
                    messages.append(sslError.errorString());
                }
                failWith(Tunnel::tr("The host certificate was rejected: %1")
                             .arg(messages.join(QStringLiteral("; "))));
            });
    /* Drain the peer once our queue empties so the pump resumes after a
     * high-water pause. */
    connect(m_Remote, &QSslSocket::bytesWritten, this, [this](qint64) { handleLocalReadyRead(); });

    connect(m_Local, &QTcpSocket::readyRead, this, &TunnelWorker::handleLocalReadyRead);
    connect(m_Local, &QTcpSocket::connected, this, [this] {
        /* The loopback hop carries the same latency-sensitive USB requests. */
        m_Local->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        handleRemoteReadyRead();
    });
    connect(m_Local, &QTcpSocket::disconnected, this, [this] { finishCleanly(); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_Local, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        failWith(
            Tunnel::tr("The local USB service connection failed: %1").arg(m_Local->errorString()));
    });
#else
    connect(m_Local, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
            [this](QAbstractSocket::SocketError) {
                failWith(Tunnel::tr("The local USB service connection failed: %1")
                             .arg(m_Local->errorString()));
            });
#endif
    connect(m_Local, &QTcpSocket::bytesWritten, this, [this](qint64) { handleRemoteReadyRead(); });

    m_Local->connectToHost(m_Config.localHost, m_Config.localPort);
    m_Remote->connectToHostEncrypted(m_Config.host, m_Config.port);
    /* Deadline covering TCP connect, TLS, the JSON handshake, and the host-side
     * usbip attach that must finish before the server sends its ready line
     * (the server allows 12 s for the same window; the client must outlast it). */
    m_StartupTimer->start(m_Config.startupTimeoutMs);
    return true;
}

void TunnelWorker::stop() noexcept
{
    m_Finished = true;
    m_StartupTimer->stop();
    if (m_Local != nullptr) {
        m_Local->disconnect(this);
        m_Local->abort();
        m_Local->deleteLater();
        m_Local = nullptr;
    }
    if (m_Remote != nullptr) {
        m_Remote->disconnect(this);
        m_Remote->abort();
        m_Remote->deleteLater();
        m_Remote = nullptr;
    }
}

void TunnelWorker::handleRemoteReadyRead()
{
    if (m_Finished || !m_PeerVerified || m_Remote == nullptr || m_Local == nullptr ||
        m_Local->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    if (!m_HandshakeDone) {
        m_HandshakeBuffer.append(m_Remote->readAll());
        const qsizetype newline = m_HandshakeBuffer.indexOf('\n');
        if (newline < 0) {
            if (m_HandshakeBuffer.size() > kMaxHandshakeBytes) {
                failWith(Tunnel::tr("The host sent an invalid USB tunnel response."));
            }
            return;
        }
        if (newline > kMaxHandshakeBytes) {
            /* Content alone exceeds the mirrored server limit; the server
             * would have closed at the same boundary. */
            failWith(Tunnel::tr("The host sent an invalid USB tunnel response."));
            return;
        }
        const QByteArray line = m_HandshakeBuffer.left(newline);
        m_HandshakeBuffer.remove(0, newline + 1);

        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        const QString op = reply.value(QStringLiteral("op")).toString();
        if (op != QStringLiteral("ready")) {
            const QString reason = describeHostReason(reply.value(QStringLiteral("reason")).toString());
            failWith(reason.isEmpty()
                         ? Tunnel::tr("The host could not start USB forwarding.")
                         : Tunnel::tr("The host could not start USB forwarding: %1").arg(reason));
            return;
        }
        m_HandshakeDone = true;
        m_StartupTimer->stop();
        {
            const KeepAliveReport report = applyKeepAlive(m_Remote, m_Config);
            if (m_Config.onKeepAliveApplied) {
                m_Config.onKeepAliveApplied(report);
            }
        }
        emit forwarding();
        if (m_Finished)
            return;
        /* Residual bytes past the handshake line stay in m_HandshakeBuffer and
         * are flushed through the bounded pump below — never written directly,
         * so kHighWaterMark always applies. */
    }

    /* Bounded opaque byte pump: host -> local USB/IP server. Handshake
     * residual bytes and fresh socket reads share the same high-water limit;
     * m_Local's bytesWritten signal resumes this pump after a pause. */
    while (!m_Finished && m_Local && m_Remote && m_Local->bytesToWrite() < kHighWaterMark) {
        QByteArray chunk;
        if (!m_HandshakeBuffer.isEmpty()) {
            chunk = m_HandshakeBuffer.left(64 * 1024);
            m_HandshakeBuffer.remove(0, chunk.size());
        } else if (m_Remote->bytesAvailable() > 0) {
            chunk = m_Remote->read(64 * 1024);
        } else {
            break;
        }
        if (chunk.isEmpty()) {
            break;
        }
        m_Local->write(chunk);
    }

    /* Drain anything the local USB/IP server queued before we were ready. */
    if (!m_Finished && m_Local && m_HandshakeBuffer.isEmpty() &&
        m_Local->bytesToWrite() < kHighWaterMark) {
        handleLocalReadyRead();
    }
}

void TunnelWorker::handleLocalReadyRead()
{
    if (m_Remote == nullptr || m_Local == nullptr || !m_HandshakeDone) {
        /* Hold local data until the authenticated tunnel is ready. USB/IP
         * import must flow before the host's attach command can complete. */
        return;
    }

    /* Opaque byte pump: local USB/IP server -> host. */
    while (!m_Finished && m_Local && m_Remote && m_Local->bytesAvailable() > 0 &&
           m_Remote->bytesToWrite() < kHighWaterMark) {
        const QByteArray chunk = m_Local->read(64 * 1024);
        if (chunk.isEmpty()) {
            break;
        }
        m_Remote->write(chunk);
    }
}

void TunnelWorker::failWith(const QString &message)
{
    if (m_Finished) {
        return;
    }
    stop();
    emit finished(message);
}

void TunnelWorker::finishCleanly()
{
    if (m_Finished) {
        return;
    }
    if (!m_HandshakeDone) {
        failWith(Tunnel::tr("The USB tunnel closed before the connection was ready."));
        return;
    }
    stop();
    emit finished(QString());
}

class Tunnel::Impl
{
public:
    explicit Impl(TunnelConfig configuration) : config(std::move(configuration)) {}

    TunnelConfig config;
    QThread ioThread;
    TunnelWorker *worker = nullptr;
    quint64 generation = 0;
};

Tunnel::Tunnel(TunnelConfig config, QObject *parent)
    : QObject(parent), m_Impl(std::make_unique<Impl>(std::move(config)))
{
}

Tunnel::~Tunnel()
{
    stop();
}

bool Tunnel::start(QString *error)
{
    if (!m_Impl->config.valid()) {
        if (error != nullptr) {
            *error = tr("The USB tunnel configuration is incomplete.");
        }
        return false;
    }
    if (m_Impl->worker != nullptr) {
        if (error != nullptr) {
            *error = tr("The USB tunnel is already running.");
        }
        return false;
    }
    if (m_Impl->config.pinnedServerCertificate.isNull()) {
        if (error != nullptr) {
            *error = tr("Pair with this host before forwarding USB devices.");
        }
        return false;
    }

    auto *worker = new TunnelWorker(m_Impl->config);
    m_Impl->worker = worker;
    const quint64 generation = ++m_Impl->generation;
    worker->moveToThread(&m_Impl->ioThread);
    connect(&m_Impl->ioThread, &QThread::started, worker, [worker] {
        QString startError;
        if (!worker->start(&startError)) {
            emit worker->finished(startError);
        }
    });
    connect(worker, &TunnelWorker::forwarding, this, [this, generation] {
        if (m_Impl->worker != nullptr && generation == m_Impl->generation) {
            emit forwarding();
        }
    });
    connect(worker, &TunnelWorker::finished, this, [this, generation](const QString &message) {
        if (m_Impl->worker == nullptr || generation != m_Impl->generation) {
            return;
        }
        stop();
        emit finished(message);
    });
    m_Impl->ioThread.start();
    return true;
}

void Tunnel::stop() noexcept
{
    TunnelWorker *worker = m_Impl->worker;
    if (worker == nullptr) {
        return;
    }
    m_Impl->worker = nullptr;
    ++m_Impl->generation;
    worker->disconnect(this);
    /* Close sockets on their owning thread before returning, so device
     * restoration cannot race with a still-open USB/IP connection. This
     * does not wait for network I/O or any GUI-thread callback. */
    if (m_Impl->ioThread.isRunning()) {
        QMetaObject::invokeMethod(
            worker, [worker] { worker->stop(); }, Qt::BlockingQueuedConnection);
    } else {
        worker->stop();
    }
    m_Impl->ioThread.quit();
    m_Impl->ioThread.wait();
    /* deleteLater posted around thread exit never gets processed on a dead
     * loop; the owner deletes the worker outright once the thread joined. */
    delete worker;
}

} // namespace UsbForwarding

#include "usbforwardingtunnel.moc"
