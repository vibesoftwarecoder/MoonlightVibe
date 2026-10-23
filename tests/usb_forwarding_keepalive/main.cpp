// After the handshake the tunnel turns on TCP keepalive so a host that vanishes
// without closing is noticed, and it must NOT end a merely quiet device.
//
// What this cannot do: make packets vanish. On loopback the kernel always
// answers keepalive probes, so the probe-timeout path itself (the OS giving up
// on a silent peer) is not reproducible here. These tests check that the
// option and its timing are really set on the live socket (read back from the
// OS), that quiet is not fatal, and that a host that disappears abruptly ends
// the tunnel cleanly with a status.
#include "../../app/backend/usbforwardingtunnel.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <atomic>
#include <mutex>

using UsbForwarding::KeepAliveReport;
using UsbForwarding::Tunnel;
using UsbForwarding::TunnelConfig;

namespace {
const QByteArray kToken(64, 'd');

struct Reports
{
    std::mutex mutex;
    QList<KeepAliveReport> list;
    int count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(list.size());
    }
    KeepAliveReport first()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return list.value(0);
    }
};

struct Rig
{
    explicit Rig(const TestIdentity &host, const TestIdentity &client) : server(host)
    {
        QVERIFY2(server.listen(), "fake host listen");
        QVERIFY2(localServer.listen(QHostAddress::LocalHost, 0), "fake local listen");
        QObject::connect(&localServer, &QTcpServer::newConnection, &localServer,
                         [this] { localSocket = localServer.nextPendingConnection(); });
        config.host = QStringLiteral("127.0.0.1");
        config.port = server.port();
        config.sessionToken = kToken;
        config.pinnedServerCertificate = host.certificate;
        config.busId = "1-2";
        config.localPort = localServer.serverPort();
        config.sslConfiguration.setLocalCertificate(client.certificate);
        config.sslConfiguration.setPrivateKey(client.key);
        auto r = reports;
        config.onKeepAliveApplied = [r](const KeepAliveReport &report) {
            std::lock_guard<std::mutex> lock(r->mutex);
            r->list.append(report);
        };
        server.onData = [this](QSslSocket *socket, const QByteArray &all) {
            if (!replied && all.contains('\n')) {
                replied = true;
                socket->write(reply);
            }
        };
    }
    FakeTlsServer server;
    QTcpServer localServer;
    QTcpSocket *localSocket = nullptr;
    TunnelConfig config;
    std::shared_ptr<Reports> reports = std::make_shared<Reports>();
    QByteArray reply = "{\"op\":\"ready\"}\n";
    bool replied = false;
};
} // namespace

class KeepAliveTest : public QObject
{
    Q_OBJECT

    TestIdentity a, b;

private slots:
    void initTestCase()
    {
        a = loadTestIdentity(QStringLiteral("a"));
        b = loadTestIdentity(QStringLiteral("b"));
        QVERIFY2(a.valid() && b.valid(), "test certificates missing: run tests/run_usb_forwarding_tests.ps1");
    }

    void keepAliveIsSetOnTheLiveSocketOnlyAfterTheHandshake()
    {
        Rig rig(a, b);
        rig.reply.clear(); // the host reads the request but does not answer yet
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QVERIFY(tunnel.start());
        QTRY_VERIFY_WITH_TIMEOUT(rig.replied, 10000);
        QTest::qWait(200);
        QCOMPARE(rig.reports->count(), 0); // not before the host said ready

        rig.server.lastSocket()->write("{\"op\":\"ready\"}\n");
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QCOMPARE(rig.reports->count(), 1);
        const KeepAliveReport report = rig.reports->first();
        QVERIFY2(report.enabled, "SO_KEEPALIVE is not set on the remote socket");
#ifdef Q_OS_WIN
        // Read back from the OS, not echoed from our own config: about a minute to notice.
        QCOMPARE(report.idleSec, 30);
        QCOMPARE(report.intervalSec, 5);
        QCOMPARE(report.count, 6);
#endif
    }

    void configuredTimingReachesTheSocket()
    {
        Rig rig(a, b);
        rig.config.keepAliveIdleSec = 11;
        rig.config.keepAliveIntervalSec = 3;
        rig.config.keepAliveCount = 4;
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        const KeepAliveReport report = rig.reports->first();
        QVERIFY(report.enabled);
#ifdef Q_OS_WIN
        QCOMPARE(report.idleSec, 11);
        QCOMPARE(report.intervalSec, 3);
        QCOMPARE(report.count, 4);
#endif
    }

    void aHostThatRefusesNeverGetsKeepAliveApplied()
    {
        Rig rig(a, b);
        rig.reply = "{\"op\":\"error\",\"reason\":\"forbidden\"}\n";
        Tunnel tunnel(rig.config);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(rig.reports->count(), 0);
    }

    void invalidKeepAliveSettingsAreRefused_data()
    {
        QTest::addColumn<int>("idle");
        QTest::addColumn<int>("interval");
        QTest::addColumn<int>("count");
        QTest::newRow("idle 0") << 0 << 5 << 6;
        QTest::newRow("interval 0") << 30 << 0 << 6;
        QTest::newRow("count 0") << 30 << 5 << 0;
        QTest::newRow("negative") << -1 << 5 << 6;
    }
    void invalidKeepAliveSettingsAreRefused()
    {
        QFETCH(int, idle);
        QFETCH(int, interval);
        QFETCH(int, count);
        Rig rig(a, b);
        QVERIFY(rig.config.valid());
        rig.config.keepAliveIdleSec = idle;
        rig.config.keepAliveIntervalSec = interval;
        rig.config.keepAliveCount = count;
        QVERIFY(!rig.config.valid());
        Tunnel tunnel(rig.config);
        QVERIFY(!tunnel.start());
        QTest::qWait(100);
        QCOMPARE(rig.server.connections(), 0);
    }

    void aQuietDeviceIsNotKilledByAnIdleTimer()
    {
        Rig rig(a, b);
        rig.config.startupTimeoutMs = 400; // the startup deadline must not outlive the handshake
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.localSocket != nullptr, 10000);

        QTest::qWait(1500); // several times the startup deadline with no bytes either way
        QCOMPARE(finished.count(), 0);

        // And the tunnel still carries data afterwards.
        rig.server.lastSocket()->write("PING");
        QTRY_COMPARE_WITH_TIMEOUT(rig.localSocket->bytesAvailable(), qint64(4), 10000);
        QCOMPARE(rig.localSocket->readAll(), QByteArray("PING"));
    }

    void aHostThatVanishesAbruptlyEndsTheTunnelCleanlyWithAStatus()
    {
        Rig rig(a, b);
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.localSocket != nullptr, 10000);

        rig.server.lastSocket()->abort(); // no TLS shutdown, as if the process died

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1); // reported once, not repeatedly
        // The local USB/IP connection is released too.
        QTRY_COMPARE_WITH_TIMEOUT(rig.localSocket->state(), QAbstractSocket::UnconnectedState, 10000);
        tunnel.stop(); // still safe afterwards
    }
};

QTEST_GUILESS_MAIN(KeepAliveTest)
#include "main.moc"
