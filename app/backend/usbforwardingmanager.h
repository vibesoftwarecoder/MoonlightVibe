#pragma once

#include "usbforwardingcapability.h"
#include "usbforwardingtunnel.h"

#include <QList>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

/*
 * Lifecycle of USB forwarding for ONE streaming session.
 *
 * Everything the manager needs is injected, so it knows nothing about Session,
 * NvHTTP or the preference object:
 *
 *   ManagerConfig  what to do and WHO to talk to (see the trust boundary below)
 *   ManagerSeams   capability fetcher, shared-device lister, tunnel factory
 *   ManagerOptions retry/backoff numbers
 *
 * Threading
 *   The manager lives on the caller's thread. start() only validates the
 *   configuration and posts the work to a private worker thread; it never
 *   touches the network and never blocks. When forwarding is turned off in the
 *   preferences, start() does nothing at all: no thread, no request.
 *   The capability request (up to 5 s) runs on a pool thread, never on the
 *   worker's event loop, so stop() is never held up by it.
 *   stop(), toggle() and snapshot() may be called from any thread.
 *   Seams are called from the worker (capability fetcher: from a pool thread);
 *   they must be thread safe and must own whatever they capture.
 *
 * Trust boundary
 *   The tunnel host is exactly ManagerConfig::streamAddress, the address the
 *   stream itself is connected to, and it must equal pairedAddress, the
 *   address of the paired computer record whose certificate is pinned.
 *   Neither is ever free text from a settings field. The pinned server
 *   certificate and the client certificate/key are mandatory. Anything else
 *   is refused before any thread or socket exists.
 *
 * Failure policy
 *   Failures never touch the stream. Retries are bounded and backed off
 *   (ManagerOptions::retryBackoffMs); a clean close by the host is final.
 *   The capability body and the token are never logged.
 *
 * Polling without an event loop
 *   snapshot() returns a plain copy taken under a mutex. The SDL/stream loop
 *   can poll it. snapshotChanged() is for contexts that do have an event loop.
 */
namespace UsbForwarding {

/* A device usbipd is currently sharing. identity is the stable identity used
 * in the preferences ("vid:pid" or "vid:pid#serial"); busId may change. */
struct SharedDevice
{
    QString identity;
    QByteArray busId;
};

/* Picks the shareable devices out of UsbForwardingBackend::devices(). */
QList<SharedDevice> sharedDevicesFromBackendList(const QVariantList &devices);

/* The tunnel, as the manager sees it. The real one wraps UsbForwarding::Tunnel. */
class TunnelHandle : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool start(QString *error) = 0;
    virtual void stop() noexcept = 0;
signals:
    void forwarding();
    void finished(QString message);
};

using CapabilityFetcher = std::function<Capability()>;           // may throw
using DeviceLister = std::function<QList<SharedDevice>()>;
using TunnelFactory = std::function<std::unique_ptr<TunnelHandle>(const TunnelConfig &)>;

struct ManagerSeams
{
    CapabilityFetcher fetchCapability;
    DeviceLister listDevices;
    /* Optional: defaults to the real UsbForwarding::Tunnel. */
    TunnelFactory createTunnel;
    /* Optional. Called by stop() before it joins the worker, from any thread:
     * a lister that is waiting on something (a process) must give up and
     * return soon. stop() never waits for a lister either way. */
    std::function<void()> abandonListing;
};

struct ManagerOptions
{
    /* Delay before retry n (1-based); the list length is the retry budget. */
    QList<int> retryBackoffMs = {2000, 5000, 15000, 30000, 60000};
    int tunnelStartupTimeoutMs = 15 * 1000;
    /* A tunnel that stayed up this long gets a fresh retry budget when it drops. */
    int stableAfterMs = 30 * 1000;
    /* The local USB/IP server (usbipd-win). Only tests change this. */
    quint16 localUsbipPort = 3240;
};

struct ManagerConfig
{
    /* StreamingPreferences::usbForwardingEnabled / usbForwardingAutoDevices. */
    bool enabled = false;
    QStringList autoDevices;

    /* Address the stream is connected to, and the address of the paired
     * computer record. They must be the same host. */
    QString streamAddress;
    QString pairedAddress;
    /* The paired server certificate (pinned) and the paired client identity. */
    QSslCertificate pinnedServerCertificate;
    QSslConfiguration clientSsl;

    /* True when the configuration may be used. Otherwise *reason is a short
     * developer-facing explanation (never shown to users, never contains secrets). */
    bool validate(QString *reason = nullptr) const;
    /* The ONE spelling used for comparing and connecting; empty means refused
     * (fail closed). Callers (the stream address and the paired record) must
     * both go through this. Accepted:
     *   - IPv4 and IPv6 literals; IPv6 may be in [brackets] (brackets are
     *     refused around anything else); output is the canonical spelling
     *   - a scoped IPv6 address, kept exactly as written: "fe80::1%12" never
     *     equals "fe80::1" or another scope
     *   - a DNS name: lower-cased, one trailing dot dropped, non-ASCII
     *     converted to punycode (QUrl::toAce); labels of letters, digits and
     *     hyphens, at most 253 characters
     * Refused: empty, any whitespace or control character, scheme, port, path,
     * user info, a malformed IPv4 such as 999.1.1.1, empty labels ("a..b").
     * A DNS name never equals an IP literal, even if it resolves to it. */
    static QString canonicalHostAddress(const QString &address);
    static bool isPlausibleHostAddress(const QString &address);
};

enum class State
{
    Off,             // not started, or turned off in the preferences
    InvalidConfig,   // refused: host/certificate inputs did not pass validation
    Checking,        // asking the host
    Retrying,        // the host could not be reached; trying again later
    HostDisabled,    // the host has USB forwarding switched off
    HostUnavailable, // the host cannot do it right now
    HostForbidden,   // the host does not allow it for this client
    Active,          // the host said yes; devices are being forwarded
    Paused,          // toggled off for this session
    Failed,          // gave up after repeated errors
    Stopped,         // the session ended
};

enum class DeviceState
{
    NotShared,      // selected in the preferences but not shared right now
    Pending,        // waiting for a fresh capability before connecting
    Connecting,
    Forwarding,
    WaitingToRetry,
    Failed,         // retry budget used up
    Ended,          // the host closed the tunnel; not retried
};

struct DeviceStatus
{
    QString identity;
    QByteArray busId;
    DeviceState state = DeviceState::NotShared;
};

struct Snapshot
{
    State state = State::Off;
    /* Plain text from fixed messages; never host-provided text. */
    QString status;
    QList<DeviceStatus> devices;
    /* Increases with every change. */
    quint64 revision = 0;

    int forwardingCount() const;
};

class Manager final : public QObject
{
    Q_OBJECT

public:
    explicit Manager(ManagerSeams seams, ManagerOptions options = {}, QObject *parent = nullptr);
    ~Manager() override;

    Q_DISABLE_COPY(Manager)

    /* Begins a session. Never blocks and never throws. Returns false when the
     * configuration was refused (state InvalidConfig); true otherwise, including
     * the "turned off" case in which nothing is started. A running session is
     * stopped first. */
    bool start(const ManagerConfig &config);
    /* Tears everything down before returning. Idempotent; any thread. */
    void stop() noexcept;
    /* Pauses or resumes forwarding for this session. Returns false when there
     * is nothing to toggle (not started, stopped, or off in the preferences). */
    bool toggle();

    /* Safe from any thread, with or without an event loop. */
    Snapshot snapshot() const;
    /* The fixed text for a state (used by the snapshot). */
    static QString statusFor(State state, const QList<DeviceStatus> &devices, int configuredCount);

signals:
    /* Emitted after every change, from the manager's thread via a queued connection. */
    void snapshotChanged();

private:
    friend class Engine;
    struct Shared;
    struct Runtime;
    void publishLocal(State state);
    void teardownLocked();
    void abandonListing() noexcept;

    ManagerSeams m_Seams;
    ManagerOptions m_Options;
    std::shared_ptr<Shared> m_Shared;
    std::mutex m_Lifecycle;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Enabled{false};
    std::unique_ptr<Runtime> m_Runtime;
    /* Identity of the worker, to detect stop() called from the worker itself. */
    std::atomic<QThread *> m_WorkerThread{nullptr};
    std::atomic<QObject *> m_WorkerEngine{nullptr};
};

} // namespace UsbForwarding
