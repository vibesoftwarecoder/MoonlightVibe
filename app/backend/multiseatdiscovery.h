#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QSet>
#include <QStringList>
#include <QTimer>

// Finds MultiSeat seats by probing the seat port block on hosts the user already has, and emits
// seatFound() for each one so ComputerManager can add it.
//
// ⭐ It does NOT ask the MultiSeat service. The previous implementation polled
// http://127.0.0.1:9550/api/seats, which only ever worked when Moonlight ran on the host itself —
// on any other machine that address is the CLIENT, which runs no MultiSeat service. The request
// failed and was swallowed deliberately ("silent failure, will retry"), so seats simply never
// appeared and nothing said why. See MoonlightVibe#1.
//
// Asking the service would also mean exposing MultiSeat's dashboard API to the LAN and putting its
// API key on every client. Probing needs neither: a seat's Apollo already answers /serverinfo on
// its own port, which is exactly what adding it by hand does.
//
// ⛔ mDNS is NOT an alternative here. A seat's Apollo logs "Registered Apollo mDNS service", but
// the registration never reaches the network: Apollo registers through Windows' responder rather
// than binding 5353 itself, and a registration made inside an RDP session does not escape it.
// Measured 2026-09-10 — browsing _nvstream._tcp with a seat running returns the console Apollo and
// nothing else, every time.
class MultiSeatDiscovery : public QObject
{
    Q_OBJECT

public:
    static constexpr int POLL_INTERVAL_MS = 15000;

    // Mirrors MultiSeat's Constants.PortBase / Constants.PortsPerSeat. Seat N answers on
    // PortBase + N * PortsPerSeat, so the defaults give 48100, 48130, 48160, 48190.
    //
    // ⚠️ Both are configurable on the host (MultiSeat:PortBase, MultiSeat:MaxSeats) and nothing
    // advertises them, so a host that has changed them needs its seats added by hand. Probing the
    // default block covers the normal case without any host-side cooperation at all.
    static constexpr uint16_t SEAT_PORT_BASE = 48100;
    static constexpr uint16_t SEAT_PORT_STRIDE = 30;
    static constexpr int MAX_SEATS_PROBED = 4;

    // A seat's Apollo is named MultiSeat-{Account}-{N} by ApolloConfigBuilder, so its /serverinfo
    // hostname identifies it. This is what keeps the probe from mistaking an unrelated Apollo on a
    // nearby port for a seat.
    static constexpr const char* SEAT_NAME_PREFIX = "MultiSeat-";

    explicit MultiSeatDiscovery(QObject* parent = nullptr);
    ~MultiSeatDiscovery();

    void start();
    void stop();

    // Addresses to probe — the hosts the user already knows about. ComputerManager keeps this
    // current; discovery has no opinion about where hosts come from.
    void setHostsToProbe(const QStringList& addresses);

signals:
    // Emitted for each seat that answers on a seat port with a MultiSeat- hostname.
    // port is the seat's Apollo HTTP port (its portBase).
    void seatFound(QString host, uint16_t port, QString displayName);

    // Raised immediately before each poll so the owner can refresh setHostsToProbe().
    //
    // Pulling the addresses here rather than pushing them from every place a host is added or
    // removed keeps this off ComputerManager's locking paths — the add path runs on a thread pool
    // while holding the write lock, and reaching back into it from there invites a deadlock.
    void aboutToPoll();

private slots:
    void poll();
    void handleReply(QNetworkReply* reply);

private:
    QNetworkAccessManager* m_Nam;
    QTimer* m_Timer;
    QStringList m_Hosts;

    // Keyed by "host:port". Stops a slow or unreachable endpoint from accumulating one request
    // per tick, which the old single m_RequestPending flag could not express once probes run in
    // parallel.
    QSet<QString> m_InFlight;
};
