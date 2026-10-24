#include "usbforwardingmanager.h"

#include <QHostAddress>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QSslKey>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QtDebug>
#include <algorithm>
#include <vector>

namespace UsbForwarding {

/* ------------------------------------------------------------------------- */
/* Pure helpers                                                              */
/* ------------------------------------------------------------------------- */

QList<SharedDevice> sharedDevicesFromBackendList(const QVariantList &devices)
{
    QList<SharedDevice> result;
    for (const QVariant &entry : devices) {
        const QVariantMap map = entry.toMap();
        const QString identity = map.value(QStringLiteral("identity")).toString();
        const QByteArray busId = map.value(QStringLiteral("busId")).toString().toUtf8();
        /* Only a device that is plugged in, shared (bound) and has a bus id
         * usbipd can export, with an identity the preferences can name. */
        if (identity.isEmpty() || !map.value(QStringLiteral("isConnected")).toBool() ||
            !map.value(QStringLiteral("isBound")).toBool() ||
            !map.value(QStringLiteral("isSupported")).toBool() || !isValidBusId(busId)) {
            continue;
        }
        result.append({identity, busId});
    }
    return result;
}

QString ManagerConfig::canonicalHostAddress(const QString &address)
{
    if (address.isEmpty() || address.size() > 260) {
        return QString();
    }
    for (const QChar c : address) {
        if (c.isSpace() || c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return QString(); // no spaces or control characters, ever
        }
    }
    /* IPv6 literal, optionally in brackets. Brackets are accepted ONLY around
     * an IPv6 address. QHostAddress::toString() gives one spelling per
     * address; a scope id is kept as written, so scoped addresses only
     * match an identical scope. */
    QString inner = address;
    const bool bracketed = inner.startsWith(QLatin1Char('[')) && inner.endsWith(QLatin1Char(']'));
    if (bracketed) {
        inner = inner.mid(1, inner.size() - 2);
    }
    const QHostAddress ip(inner);
    if (!ip.isNull()) {
        if (bracketed && ip.protocol() != QAbstractSocket::IPv6Protocol) {
            return QString();
        }
        return ip.toString();
    }
    if (bracketed) {
        return QString();
    }
    /* A DNS name: one trailing dot is dropped, non-ASCII is converted to its
     * ASCII (punycode) form, and the result is lower case. */
    QString name = address;
    if (name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        return QString();
    }
    bool ascii = true;
    for (const QChar c : name) {
        ascii = ascii && c.unicode() < 0x80;
    }
    if (!ascii) {
        name = QString::fromLatin1(QUrl::toAce(name));
    }
    name = name.toLower();
    if (name.isEmpty() || name.size() > 253) {
        return QString();
    }
    /* An all-digit-and-dot string that did not parse above is a malformed IP. */
    static const QRegularExpression onlyDigitsAndDots(QStringLiteral("\\A[0-9.]+\\z"));
    if (onlyDigitsAndDots.match(name).hasMatch()) {
        return QString();
    }
    static const QRegularExpression nameRe(QStringLiteral(
        "\\A[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*\\z"));
    return nameRe.match(name).hasMatch() ? name : QString();
}

bool ManagerConfig::isPlausibleHostAddress(const QString &address)
{
    return !canonicalHostAddress(address).isEmpty();
}

bool ManagerConfig::validate(QString *reason) const
{
    const auto fail = [reason](const char *why) {
        if (reason != nullptr) {
            *reason = QString::fromLatin1(why);
        }
        return false;
    };
    if (!isPlausibleHostAddress(streamAddress)) {
        return fail("the stream address is empty or not a host address");
    }
    if (!isPlausibleHostAddress(pairedAddress)) {
        return fail("the paired address is empty or not a host address");
    }
    if (canonicalHostAddress(streamAddress) != canonicalHostAddress(pairedAddress)) {
        return fail("the stream address is not the paired host's address");
    }
    if (pinnedServerCertificate.isNull()) {
        return fail("the paired server certificate is missing");
    }
    if (clientSsl.localCertificate().isNull() || clientSsl.privateKey().isNull()) {
        return fail("the paired client certificate or key is missing");
    }
    return true;
}

int Snapshot::forwardingCount() const
{
    return static_cast<int>(std::count_if(devices.begin(), devices.end(), [](const DeviceStatus &d) {
        return d.state == DeviceState::Forwarding;
    }));
}

QString Manager::statusFor(State state, const QList<DeviceStatus> &devices, int configuredCount)
{
    switch (state) {
    case State::Off:
        return tr("USB forwarding is off.");
    case State::InvalidConfig:
        return tr("USB forwarding could not start: the host settings are not valid.");
    case State::Checking:
        return tr("Checking USB forwarding on the host...");
    case State::Retrying:
        return tr("Could not reach the host for USB forwarding. Trying again...");
    case State::HostDisabled:
        return tr("USB forwarding is disabled on the host.");
    case State::HostUnavailable:
        return tr("USB forwarding is not available on the host right now.");
    case State::HostForbidden:
        return tr("The host does not allow USB forwarding for this device.");
    case State::Paused:
        return tr("USB forwarding is paused.");
    case State::Failed:
        return tr("USB forwarding stopped after repeated errors.");
    case State::Stopped:
        return tr("USB forwarding stopped.");
    case State::Active:
        break;
    }
    if (configuredCount == 0) {
        return tr("No USB devices are selected for forwarding.");
    }
    int forwarding = 0, connecting = 0, waiting = 0, failed = 0, ended = 0, notShared = 0;
    for (const DeviceStatus &d : devices) {
        switch (d.state) {
        case DeviceState::Forwarding: ++forwarding; break;
        case DeviceState::Connecting:
        case DeviceState::Pending: ++connecting; break;
        case DeviceState::WaitingToRetry: ++waiting; break;
        case DeviceState::Failed: ++failed; break;
        case DeviceState::Ended: ++ended; break;
        case DeviceState::NotShared: ++notShared; break;
        }
    }
    if (forwarding > 0) {
        return tr("Forwarding %n USB device(s).", nullptr, forwarding);
    }
    if (connecting > 0) {
        return tr("Connecting USB devices...");
    }
    if (waiting > 0) {
        return tr("A USB device connection failed. Trying again...");
    }
    if (failed > 0) {
        return tr("A USB device could not be forwarded.");
    }
    if (ended > 0) {
        return tr("The host closed the USB connection.");
    }
    Q_UNUSED(notShared);
    return tr("None of the selected USB devices is shared right now.");
}

/* ------------------------------------------------------------------------- */
/* Real tunnel                                                               */
/* ------------------------------------------------------------------------- */

namespace {

class RealTunnelHandle final : public TunnelHandle
{
public:
    explicit RealTunnelHandle(const TunnelConfig &config) : m_Tunnel(config)
    {
        connect(&m_Tunnel, &Tunnel::forwarding, this, &TunnelHandle::forwarding);
        connect(&m_Tunnel, &Tunnel::finished, this, &TunnelHandle::finished);
    }
    bool start(QString *error) override { return m_Tunnel.start(error); }
    void stop() noexcept override { m_Tunnel.stop(); }

private:
    Tunnel m_Tunnel;
};

QString redact(QString text, const QByteArray &token)
{
    if (!token.isEmpty()) {
        text.replace(QString::fromLatin1(token), QStringLiteral("[redacted]"));
    }
    return text;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Shared state and the worker-thread engine                                 */
/* ------------------------------------------------------------------------- */

struct Manager::Shared
{
    std::mutex mutex;
    Snapshot snapshot;
};

class Engine;
/* Lets a pool thread post a result to the engine without racing its deletion. */
struct Link
{
    std::mutex mutex;
    Engine *engine = nullptr;
};

class Engine final : public QObject
{
    Q_OBJECT

public:
    Engine(ManagerSeams seams, ManagerOptions options, std::shared_ptr<Manager::Shared> shared,
           std::shared_ptr<Link> link)
        : m_Seams(std::move(seams)), m_Options(std::move(options)), m_Shared(std::move(shared)),
          m_Link(std::move(link))
    {
        {
            std::lock_guard<std::mutex> lock(m_Link->mutex);
            m_Link->engine = this;
        }
        m_FetchTimer = new QTimer(this);
        m_FetchTimer->setSingleShot(true);
        connect(m_FetchTimer, &QTimer::timeout, this, [this] { startFetch(); });
    }

    ~Engine() override
    {
        std::lock_guard<std::mutex> lock(m_Link->mutex);
        m_Link->engine = nullptr;
    }

    /* All of these run on the worker thread (or, for stopAll after the thread
     * has finished, on the joining thread with nothing else running). */
    void begin(const ManagerConfig &config)
    {
        if (m_Started) {
            return;
        }
        m_Config = config;
        m_Started = true;
        m_Running = true;
        m_Paused = false;
        m_CapFailures = 0;
        m_Rechecks = 0;
        setState(State::Checking);
        startFetch();
    }

    void toggle()
    {
        if (!m_Running) {
            return;
        }
        if (m_Paused) {
            m_Paused = false;
            m_CapFailures = 0;
            m_Rechecks = 0;
            setState(State::Checking);
            startFetch();
        } else {
            m_Paused = true;
            teardown();
            setState(State::Paused);
        }
    }

    /* stop() was called from a slot running on this thread (a seam, a tunnel
     * handler). Tearing down in place would free Dev objects the caller is
     * still using, so only mark the session dead now and tear down once the
     * running slot has returned. */
    void requestStop()
    {
        m_Running = false;
        QMetaObject::invokeMethod(this, [this] { stopAll(); }, Qt::QueuedConnection);
    }

    void stopAll()
    {
        if (!m_Running && m_State == State::Stopped) {
            return;
        }
        m_Running = false;
        teardown();
        setState(State::Stopped);
    }

    void onFetched(quint64 id, bool ok, const Capability &capability)
    {
        if (!m_Running || m_Paused || id != m_ExpectedFetch) {
            return; // stale: the session was stopped, paused or a newer request replaced it
        }
        m_ExpectedFetch = 0;
        if (m_State == State::Active) {
            /* A device asked for a fresh capability. The session already has
             * its answer: whatever this request says only concerns the
             * devices waiting for it, never the session state. */
            if (ok && capability.available) {
                m_Capability = capability;
                reconcile();
            } else {
                failPendingDevices();
            }
            return;
        }
        if (!ok) {
            ++m_CapFailures;
            if (m_CapFailures > m_Options.retryBackoffMs.size()) {
                qWarning() << "USB forwarding: the host could not be reached; giving up";
                setState(State::Failed);
                return;
            }
            qInfo() << "USB forwarding: the host could not be reached; retry" << m_CapFailures;
            setState(State::Retrying);
            m_FetchTimer->start(m_Options.retryBackoffMs.at(m_CapFailures - 1));
            return;
        }
        m_CapFailures = 0;
        if (!capability.available) {
            const QString reason = capability.reason.trimmed().toLower();
            if (reason == QLatin1String("disabled")) {
                setState(State::HostDisabled); // the host's choice; nothing to retry
                return;
            }
            if (reason == QLatin1String("forbidden") || reason == QLatin1String("unauthorized")) {
                setState(State::HostForbidden);
                return;
            }
            /* "unavailable": the host may simply not have seen the stream yet.
             * Look again a bounded number of times, then stay quiet. */
            setState(State::HostUnavailable);
            if (m_Rechecks < m_Options.retryBackoffMs.size()) {
                m_FetchTimer->start(m_Options.retryBackoffMs.at(m_Rechecks));
                ++m_Rechecks;
            }
            return;
        }
        m_Rechecks = 0;
        m_Capability = capability;
        m_State = State::Active;
        reconcile();
    }

private:
    struct Dev
    {
        QString identity;
        QByteArray busId;
        DeviceState state = DeviceState::Pending;
        int attempts = 0;
        std::unique_ptr<TunnelHandle> tunnel;
        QTimer *retry = nullptr;
        QElapsedTimer forwardingSince; // valid while the tunnel has been forwarding
    };

    void setState(State state)
    {
        m_State = state;
        publish();
    }

    QStringList wantedIdentities() const
    {
        QStringList wanted;
        QSet<QString> seen;
        for (const QString &raw : m_Config.autoDevices) {
            const QString id = raw.trimmed().toLower();
            if (!id.isEmpty() && !seen.contains(id)) {
                seen.insert(id);
                wanted.append(id);
            }
        }
        return wanted;
    }

    void publish()
    {
        Snapshot snap;
        snap.state = m_State;
        for (const auto &dev : m_Devs) {
            snap.devices.append({dev->identity, dev->busId, dev->state});
        }
        snap.status = Manager::statusFor(m_State, snap.devices, wantedIdentities().size());
        {
            std::lock_guard<std::mutex> lock(m_Shared->mutex);
            snap.revision = m_Shared->snapshot.revision + 1;
            m_Shared->snapshot = snap;
        }
        emit changed();
    }

    void startFetch()
    {
        if (!m_Running || m_Paused || !m_Seams.fetchCapability) {
            return;
        }
        const quint64 id = ++m_FetchSeq;
        m_ExpectedFetch = id;
        const CapabilityFetcher fetcher = m_Seams.fetchCapability;
        const std::shared_ptr<Link> link = m_Link;
        /* The request can take seconds: it must not occupy this event loop, or
         * stop() would wait for it. A late answer finds no engine and is dropped. */
        QThreadPool::globalInstance()->start([fetcher, link, id] {
            bool ok = false;
            Capability capability;
            try {
                capability = fetcher();
                ok = true;
            } catch (...) {
                /* Deliberately no detail: nothing from the exchange is logged. */
            }
            std::lock_guard<std::mutex> lock(link->mutex);
            if (Engine *engine = link->engine) {
                QMetaObject::invokeMethod(
                    engine, [engine, id, ok, capability] { engine->onFetched(id, ok, capability); },
                    Qt::QueuedConnection);
            }
        });
    }

    /* Asks which devices usbipd is sharing. The lister can wait on a process
     * for seconds, so it runs on a pool thread, never on this event loop:
     * stop() must not wait for it. A late answer finds no engine, or an id
     * that is no longer expected, and is dropped. */
    void reconcile()
    {
        if (!m_Running || m_Paused) {
            return;
        }
        const quint64 id = ++m_ListSeq;
        m_ExpectedList = id;
        const DeviceLister lister = m_Seams.listDevices;
        const std::shared_ptr<Link> link = m_Link;
        QThreadPool::globalInstance()->start([lister, link, id] {
            QList<SharedDevice> shared;
            try {
                if (lister) {
                    shared = lister();
                }
            } catch (...) {
                shared.clear();
            }
            std::lock_guard<std::mutex> lock(link->mutex);
            if (Engine *engine = link->engine) {
                QMetaObject::invokeMethod(
                    engine, [engine, id, shared] { engine->onListed(id, shared); }, Qt::QueuedConnection);
            }
        });
    }

    void onListed(quint64 id, const QList<SharedDevice> &shared)
    {
        if (!m_Running || m_Paused || id != m_ExpectedList) {
            return; // stale: stopped, paused, or a newer listing replaced it
        }
        m_ExpectedList = 0;
        /* NotShared rows hold no resources: rebuild them from the current list. */
        m_Devs.erase(std::remove_if(m_Devs.begin(), m_Devs.end(),
                                    [](const std::unique_ptr<Dev> &d) { return d->state == DeviceState::NotShared; }),
                     m_Devs.end());

        const QStringList wanted = wantedIdentities();
        QSet<QString> found;
        for (const SharedDevice &sd : shared) {
            const QString id = sd.identity.trimmed().toLower();
            if (!wanted.contains(id) || !isValidBusId(sd.busId)) {
                continue;
            }
            found.insert(id);
            const bool known = std::any_of(m_Devs.begin(), m_Devs.end(),
                                           [&](const std::unique_ptr<Dev> &d) { return d->busId == sd.busId; });
            if (!known) {
                auto dev = std::make_unique<Dev>();
                dev->identity = id;
                dev->busId = sd.busId;
                dev->state = DeviceState::Pending;
                m_Devs.push_back(std::move(dev));
            }
        }
        for (const QString &id : wanted) {
            if (!found.contains(id)) {
                auto dev = std::make_unique<Dev>();
                dev->identity = id;
                dev->state = DeviceState::NotShared;
                m_Devs.push_back(std::move(dev));
            }
        }
        /* Index loop, and a check after every seam call: a seam may have asked
         * for a stop (the teardown itself waits until we return). */
        for (size_t i = 0; i < m_Devs.size() && m_Running && !m_Paused; ++i) {
            if (m_Devs[i]->state == DeviceState::Pending) {
                startTunnel(*m_Devs[i]);
            }
        }
        publish();
    }

    void startTunnel(Dev &dev)
    {
        TunnelConfig tc;
        /* The trust boundary: the host is the address the stream is connected
         * to (validated equal to the paired address), the certificate is the
         * pinned one; nothing here comes from a settings field. */
        tc.host = ManagerConfig::canonicalHostAddress(m_Config.streamAddress);
        tc.port = m_Capability.port;
        tc.sessionToken = m_Capability.token;
        tc.sslConfiguration = m_Config.clientSsl;
        tc.pinnedServerCertificate = m_Config.pinnedServerCertificate;
        tc.busId = dev.busId;
        tc.localPort = m_Options.localUsbipPort;
        tc.startupTimeoutMs = m_Options.tunnelStartupTimeoutMs;

        std::unique_ptr<TunnelHandle> handle;
        try {
            handle = m_Seams.createTunnel ? m_Seams.createTunnel(tc)
                                          : std::unique_ptr<TunnelHandle>(new RealTunnelHandle(tc));
        } catch (...) {
            handle.reset();
        }
        if (!m_Running || m_Paused) {
            if (handle) {
                handle->stop(); // the session ended while the factory ran
            }
            return;
        }
        if (!handle) {
            failDevice(dev);
            return;
        }
        Dev *p = &dev;
        connect(handle.get(), &TunnelHandle::forwarding, this, [this, p] {
            p->state = DeviceState::Forwarding;
            p->forwardingSince.start();
            publish();
        });
        connect(handle.get(), &TunnelHandle::finished, this,
                [this, p](const QString &message) { onTunnelFinished(p, message); });
        dev.tunnel = std::move(handle);
        dev.state = DeviceState::Connecting;
        QString error;
        bool started = false;
        try {
            started = dev.tunnel->start(&error);
        } catch (...) {
            started = false;
        }
        if (!started && m_Running && dev.state == DeviceState::Connecting) {
            failDevice(dev);
        }
    }

    void retire(Dev &dev)
    {
        if (dev.tunnel) {
            dev.tunnel->disconnect(this);
            dev.tunnel->stop();
            /* Possibly inside its own signal: keep the object alive until stopAll. */
            m_Graveyard.push_back(std::move(dev.tunnel));
        }
    }

    void failPendingDevices()
    {
        for (size_t i = 0; i < m_Devs.size(); ++i) {
            if (m_Devs[i]->state == DeviceState::Pending) {
                failDevice(*m_Devs[i]); // counts an attempt and schedules its own backoff
            }
        }
        publish();
    }

    void failDevice(Dev &dev)
    {
        retire(dev);
        /* Only a tunnel that stayed up for a while earns a fresh budget, so a
         * device that connects and drops at once cannot retry forever. */
        if (dev.forwardingSince.isValid()) {
            if (dev.forwardingSince.elapsed() >= m_Options.stableAfterMs) {
                dev.attempts = 0;
            }
            dev.forwardingSince.invalidate();
        }
        ++dev.attempts;
        if (dev.attempts > m_Options.retryBackoffMs.size()) {
            dev.state = DeviceState::Failed;
        } else {
            dev.state = DeviceState::WaitingToRetry;
            if (dev.retry == nullptr) {
                dev.retry = new QTimer(this);
                dev.retry->setSingleShot(true);
                Dev *p = &dev;
                connect(dev.retry, &QTimer::timeout, this, [this, p] {
                    if (!m_Running || m_Paused) {
                        return;
                    }
                    /* A fresh capability first: the old token may be stale. */
                    p->state = DeviceState::Pending;
                    publish();
                    startFetch();
                });
            }
            dev.retry->start(m_Options.retryBackoffMs.at(dev.attempts - 1));
        }
        publish();
    }

    void onTunnelFinished(Dev *dev, const QString &message)
    {
        if (!m_Running) {
            return;
        }
        if (message.isEmpty() && dev->state == DeviceState::Forwarding) {
            retire(*dev);
            dev->state = DeviceState::Ended; // the host closed it: final
            qInfo() << "USB forwarding: the host closed a device tunnel";
            publish();
            return;
        }
        qInfo().noquote() << "USB forwarding: a device tunnel ended:" << redact(message, m_Capability.token);
        failDevice(*dev);
    }

    void teardown()
    {
        m_ExpectedFetch = 0;
        m_ExpectedList = 0;
        m_FetchTimer->stop();
        for (auto &dev : m_Devs) {
            if (dev->retry != nullptr) {
                dev->retry->stop();
                delete dev->retry;
                dev->retry = nullptr;
            }
            if (dev->tunnel) {
                dev->tunnel->disconnect(this);
                dev->tunnel->stop();
                dev->tunnel.reset();
            }
        }
        m_Devs.clear();
        /* Tunnels retired from their own signals: safe to delete now. */
        for (auto &tunnel : m_Graveyard) {
            tunnel->disconnect(this);
            tunnel->stop();
        }
        m_Graveyard.clear();
    }

signals:
    void changed();

private:
    ManagerSeams m_Seams;
    ManagerOptions m_Options;
    std::shared_ptr<Manager::Shared> m_Shared;
    std::shared_ptr<Link> m_Link;
    ManagerConfig m_Config;
    Capability m_Capability;
    State m_State = State::Off;
    QTimer *m_FetchTimer = nullptr;
    std::vector<std::unique_ptr<Dev>> m_Devs;
    std::vector<std::unique_ptr<TunnelHandle>> m_Graveyard;
    quint64 m_FetchSeq = 0;
    quint64 m_ExpectedFetch = 0;
    quint64 m_ListSeq = 0;
    quint64 m_ExpectedList = 0;
    int m_CapFailures = 0;
    int m_Rechecks = 0;
    bool m_Started = false;
    bool m_Running = false;
    bool m_Paused = false;
};

/* ------------------------------------------------------------------------- */
/* Manager                                                                   */
/* ------------------------------------------------------------------------- */

struct Manager::Runtime
{
    QThread thread;
    Engine *engine = nullptr;
    std::shared_ptr<Link> link;
};

Manager::Manager(ManagerSeams seams, ManagerOptions options, QObject *parent)
    : QObject(parent), m_Seams(std::move(seams)), m_Options(std::move(options)),
      m_Shared(std::make_shared<Shared>())
{
    m_Shared->snapshot.state = State::Off;
    m_Shared->snapshot.status = statusFor(State::Off, {}, 0);
}

Manager::~Manager()
{
    stop();
}

void Manager::publishLocal(State state)
{
    std::lock_guard<std::mutex> lock(m_Shared->mutex);
    Snapshot &snap = m_Shared->snapshot;
    snap.state = state;
    snap.devices.clear();
    snap.status = statusFor(state, {}, 0);
    ++snap.revision;
}

Snapshot Manager::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_Shared->mutex);
    return m_Shared->snapshot;
}

void Manager::abandonListing() noexcept
{
    try {
        if (m_Seams.abandonListing) {
            m_Seams.abandonListing();
        }
    } catch (...) {
    }
}

void Manager::teardownLocked()
{
    if (!m_Runtime) {
        return;
    }
    m_Running = false;
    abandonListing();
    if (m_Runtime->thread.isRunning()) {
        Engine *engine = m_Runtime->engine;
        QMetaObject::invokeMethod(engine, [engine] { engine->stopAll(); }, Qt::BlockingQueuedConnection);
        m_Runtime->thread.quit();
        m_Runtime->thread.wait();
    }
    m_WorkerThread.store(nullptr);
    m_WorkerEngine.store(nullptr);
    delete m_Runtime->engine;
    m_Runtime.reset();
}

bool Manager::start(const ManagerConfig &config)
{
    bool accepted = true;
    {
        std::lock_guard<std::mutex> lifecycle(m_Lifecycle);
        teardownLocked();
        m_Enabled = config.enabled;
        QString why;
        if (!config.enabled) {
            publishLocal(State::Off); // nothing at all: no thread, no request
        } else if (!config.validate(&why) || !m_Seams.fetchCapability || !m_Seams.listDevices) {
            qWarning().noquote() << "USB forwarding: refused to start:"
                                 << (why.isEmpty() ? QStringLiteral("incomplete setup") : why);
            publishLocal(State::InvalidConfig);
            accepted = false;
        } else {
            auto runtime = std::make_unique<Runtime>();
            runtime->link = std::make_shared<Link>();
            runtime->engine = new Engine(m_Seams, m_Options, m_Shared, runtime->link);
            runtime->engine->moveToThread(&runtime->thread);
            connect(runtime->engine, &Engine::changed, this, &Manager::snapshotChanged);
            runtime->thread.start();
            publishLocal(State::Checking);
            Engine *engine = runtime->engine;
            m_WorkerThread.store(&runtime->thread);
            m_WorkerEngine.store(engine);
            m_Runtime = std::move(runtime);
            m_Running = true;
            QMetaObject::invokeMethod(engine, [engine, config] { engine->begin(config); },
                                      Qt::QueuedConnection);
        }
    }
    emit snapshotChanged();
    return accepted;
}

void Manager::stop() noexcept
{
    try {
        /* Called from a slot running on the worker itself (a seam, a tunnel
         * handler): tear down in place; the thread is joined by the next stop(),
         * start() or the destructor. */
        QThread *worker = m_WorkerThread.load();
        if (worker != nullptr && QThread::currentThread() == worker) {
            m_Running = false;
            abandonListing();
            static_cast<Engine *>(m_WorkerEngine.load())->requestStop();
            return;
        }
        std::lock_guard<std::mutex> lifecycle(m_Lifecycle);
        teardownLocked();
    } catch (...) {
    }
}

bool Manager::toggle()
{
    std::lock_guard<std::mutex> lifecycle(m_Lifecycle);
    if (!m_Runtime || !m_Running || !m_Enabled) {
        return false;
    }
    Engine *engine = m_Runtime->engine;
    QMetaObject::invokeMethod(engine, [engine] { engine->toggle(); }, Qt::QueuedConnection);
    return true;
}

} // namespace UsbForwarding

#include "usbforwardingmanager.moc"
