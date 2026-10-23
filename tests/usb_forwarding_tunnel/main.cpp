// The reverse USB/IP tunnel against a fake TLS "host" and a fake local
// usbipd, both on loopback. Throwaway certificates only.
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "../../app/backend/usbforwardingtunnel.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using UsbForwarding::Tunnel;
using UsbForwarding::TunnelConfig;

namespace {
QStringList g_LogMessages;
void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_LogMessages.append(message);
}
const QByteArray kToken(64, 'c');
} // namespace

// Stands in for usbipd-win on the client machine.
class FakeLocalServer : public QObject
{
    Q_OBJECT
public:
    FakeLocalServer()
    {
        connect(&m_Server, &QTcpServer::newConnection, this, [this] {
            m_Socket = m_Server.nextPendingConnection();
            if (m_Paused) m_Socket->setReadBufferSize(64 * 1024);
            connect(m_Socket, &QTcpSocket::readyRead, this, [this] { if (!m_Paused) m_Received += m_Socket->readAll(); });
        });
    }
    bool listen() { return m_Server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_Server.serverPort(); }
    QByteArray received() const { return m_Received; }
    QTcpSocket *socket() const { return m_Socket; }
    // A paused usbipd stops reading, so TCP back-pressure builds up.
    void setPaused(bool paused)
    {
        m_Paused = paused;
        if (m_Socket) {
            m_Socket->setReadBufferSize(paused ? 64 * 1024 : 0);
            if (!paused) m_Received += m_Socket->readAll();
        }
    }

private:
    QTcpServer m_Server;
    QTcpSocket *m_Socket = nullptr;
    QByteArray m_Received;
    bool m_Paused = false;
};

// One tunnel, one fake host, one fake local server.
struct Rig
{
    explicit Rig(const TestIdentity &hostIdentity, const TestIdentity &pinned)
        : host(hostIdentity)
    {
        QVERIFY2(host.listen(), "fake host listen");
        QVERIFY2(local.listen(), "fake local listen");
        config.host = QStringLiteral("127.0.0.1");
        config.port = host.port();
        config.sessionToken = kToken;
        config.pinnedServerCertificate = pinned.certificate;
        config.busId = "1-2";
        config.localPort = local.port();
        const TestIdentity client = loadTestIdentity(QStringLiteral("a"));
        config.sslConfiguration.setLocalCertificate(client.certificate);
        config.sslConfiguration.setPrivateKey(client.key);
        // Parse the handshake line once, then follow the scenario.
        host.onData = [this](QSslSocket *socket, const QByteArray &all) {
            const qsizetype newline = all.indexOf('\n');
            if (newline < 0 || handshakeSeen) {
                return;
            }
            handshakeSeen = true;
            handshakeLength = newline + 1;
            handshake = QJsonDocument::fromJson(all.left(newline)).object();
            if (!reply.isEmpty()) {
                socket->write(reply);
            }
            if (closeAfterHandshake) {
                socket->disconnectFromHost();
            }
        };
    }

    FakeTlsServer host;
    FakeLocalServer local;
    TunnelConfig config;
    QByteArray reply;
    bool closeAfterHandshake = false;
    bool handshakeSeen = false;
    qsizetype handshakeLength = 0;
    QJsonObject handshake;
};

class TunnelTest : public QObject
{
    Q_OBJECT

    TestIdentity a, b;

private slots:
    void initTestCase()
    {
        a = loadTestIdentity(QStringLiteral("a"));
        b = loadTestIdentity(QStringLiteral("b"));
        QVERIFY2(a.valid() && b.valid(),
                 "test certificates missing: run tests/run_usb_forwarding_tests.ps1");
        qInstallMessageHandler(captureMessages);
    }

    void init() { g_LogMessages.clear(); }

    void cleanup()
    {
        // The token is a credential: it must not appear in any log line.
        for (const QString &line : std::as_const(g_LogMessages)) {
            QVERIFY2(!line.contains(QString::fromLatin1(kToken)), "token was logged");
        }
    }

    void forwardsBytesBothWaysAfterHandshake()
    {
        Rig rig(a, a);
        // The ready line and the first USB/IP bytes arrive in one write.
        rig.reply = "{\"op\":\"ready\"}\nHELLO-FROM-HOST";
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QString error;
        QVERIFY2(tunnel.start(&error), qPrintable(error));

        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QCOMPARE(rig.handshake.value("op").toString(), QStringLiteral("forward"));
        QCOMPARE(rig.handshake.value("token").toString().toLatin1(), kToken);
        QCOMPARE(rig.handshake.value("busid").toString(), QStringLiteral("1-2"));

        // Bytes that followed the ready line reach the local server, unmodified.
        QTRY_COMPARE_WITH_TIMEOUT(rig.local.received(), QByteArray("HELLO-FROM-HOST"), 10000);

        // Local -> host.
        QVERIFY(rig.local.socket());
        rig.local.socket()->write("LOCAL-URB");
        QTRY_COMPARE_WITH_TIMEOUT(rig.host.received().mid(rig.handshakeLength), QByteArray("LOCAL-URB"), 10000);

        // Host -> local, after the handshake.
        rig.host.lastSocket()->write("MORE");
        QTRY_COMPARE_WITH_TIMEOUT(rig.local.received(), QByteArray("HELLO-FROM-HOSTMORE"), 10000);

        tunnel.stop();
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(rig.local.socket()->state(), QAbstractSocket::UnconnectedState, 10000);
    }

    void busIdIsSentVerbatimIncludingColon()
    {
        Rig rig(a, a);
        rig.config.busId = "3-1.2:4";
        rig.reply = "{\"op\":\"ready\"}\n";
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QCOMPARE(rig.handshake.value("busid").toString(), QStringLiteral("3-1.2:4"));
    }

    void impostorCertificateIsRefusedBeforeAnyCredentialIsSent()
    {
        // The host presents certificate B but we paired with A.
        Rig rig(b, a);
        rig.reply = "{\"op\":\"ready\"}\nSECRET-DEVICE-DATA";
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY2(!finished.at(0).at(0).toString().isEmpty(), "failure must carry a message");
        QCOMPARE(forwarding.count(), 0);
        // Not one application byte (so no token, no bus id) reached the impostor.
        QVERIFY2(rig.host.received().isEmpty(), "credentials leaked to an unpinned host");
        QVERIFY(!rig.handshakeSeen);
        // And nothing from the impostor reached the local USB/IP server.
        QVERIFY(rig.local.received().isEmpty());
    }

    void hostErrorReplyIsReported()
    {
        Rig rig(a, a);
        rig.reply = "{\"op\":\"error\",\"reason\":\"busy\"}\n";
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(finished.at(0).at(0).toString().contains(QStringLiteral("already in use")));
        QCOMPARE(forwarding.count(), 0);
    }

    void handshakeLineCapIs4096ContentBytes()
    {
        {
            // 4096 content bytes + newline: within the cap, rejected only
            // because it is not "ready".
            Rig rig(a, a);
            rig.reply = QByteArray(4096, 'x') + "\n";
            Tunnel tunnel(rig.config);
            QSignalSpy finished(&tunnel, &Tunnel::finished);
            QVERIFY(tunnel.start());
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
            QVERIFY2(finished.at(0).at(0).toString().contains(QStringLiteral("could not start")),
                     qPrintable(finished.at(0).at(0).toString()));
        }
        {
            // 4097 content bytes: over the cap.
            Rig rig(a, a);
            rig.reply = QByteArray(4097, 'x') + "\n";
            Tunnel tunnel(rig.config);
            QSignalSpy finished(&tunnel, &Tunnel::finished);
            QVERIFY(tunnel.start());
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
            QVERIFY2(finished.at(0).at(0).toString().contains(QStringLiteral("invalid")),
                     qPrintable(finished.at(0).at(0).toString()));
        }
    }

    void endlessHandshakeWithoutNewlineIsCut()
    {
        Rig rig(a, a);
        rig.reply = QByteArray(6000, 'y'); // never a newline
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(finished.at(0).at(0).toString().contains(QStringLiteral("invalid")));
        QCOMPARE(forwarding.count(), 0);
    }

    void hostClosingBeforeReadyIsAFailure()
    {
        Rig rig(a, a);
        rig.closeAfterHandshake = true;
        Tunnel tunnel(rig.config);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished.at(0).at(0).toString().isEmpty());
    }

    void hostReasonTextNeverReachesTheUserVerbatim_data()
    {
        QTest::addColumn<QByteArray>("reason");
        QTest::addColumn<QString>("expectedFragment");
        QTest::newRow("html") << QByteArray("<b>Update now</b><img src=x onerror=alert(1)>") << QString();
        QTest::newRow("link") << QByteArray("see https://evil.example/pay") << QString();
        QTest::newRow("long") << QByteArray(3000, 'A') << QString();
        QTest::newRow("known code, odd case") << QByteArray("  DISABLED ") << QStringLiteral("disabled on the host");
        QTest::newRow("known unavailable") << QByteArray("unavailable") << QStringLiteral("not available");
    }
    void hostReasonTextNeverReachesTheUserVerbatim()
    {
        QFETCH(QByteArray, reason);
        QFETCH(QString, expectedFragment);
        Rig rig(a, a);
        QJsonObject object{ { "op", "error" }, { "reason", QString::fromLatin1(reason) } };
        rig.reply = QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n";
        Tunnel tunnel(rig.config);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const QString message = finished.at(0).at(0).toString();
        QVERIFY2(!message.contains(QLatin1Char('<')) && !message.contains(QStringLiteral("http")) &&
                     !message.contains(QStringLiteral("alert")) && message.size() < 200,
                 qPrintable(message));
        if (expectedFragment.isEmpty()) {
            QCOMPARE(message, QStringLiteral("The host could not start USB forwarding."));
        } else {
            QVERIFY2(message.contains(expectedFragment), qPrintable(message));
        }
    }

    void missingClientCertificateOrKeyNeverConnects()
    {
        {
            Rig rig(a, a);
            rig.config.sslConfiguration = QSslConfiguration();
            QVERIFY(!rig.config.valid());
            Tunnel tunnel(rig.config);
            QVERIFY(!tunnel.start());
            QTest::qWait(200);
            QCOMPARE(rig.host.connections(), 0);
        }
        {
            Rig rig(a, a);
            rig.config.sslConfiguration.setPrivateKey(QSslKey());
            QVERIFY(!rig.config.valid());
        }
        {
            Rig rig(a, a);
            rig.config.sslConfiguration.setLocalCertificate(QSslCertificate());
            QVERIFY(!rig.config.valid());
        }
        {
            Rig rig(a, a);
            QVERIFY(rig.config.valid());
            rig.config.startupTimeoutMs = 0;
            QVERIFY(!rig.config.valid());
        }
    }

    void stopDuringHandshakeIsCleanAndSilent()
    {
        Rig rig(a, a); // the host reads the request but never answers
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_VERIFY_WITH_TIMEOUT(rig.handshakeSeen, 10000);
        tunnel.stop();
        tunnel.stop(); // idempotent
        // The host sees the connection drop, and nothing is signalled afterwards.
        QTRY_COMPARE_WITH_TIMEOUT(rig.host.lastSocket()->state(), QAbstractSocket::UnconnectedState, 10000);
        QTest::qWait(300);
        QCOMPARE(forwarding.count(), 0);
        QCOMPARE(finished.count(), 0);
    }

    void destroyingTheTunnelDuringHandshakeIsClean()
    {
        Rig rig(a, a);
        auto *tunnel = new Tunnel(rig.config);
        QSignalSpy finished(tunnel, &Tunnel::finished);
        QVERIFY(tunnel->start());
        QTRY_VERIFY_WITH_TIMEOUT(rig.handshakeSeen, 10000);
        delete tunnel; // joins the worker thread before returning
        QTRY_COMPARE_WITH_TIMEOUT(rig.host.lastSocket()->state(), QAbstractSocket::UnconnectedState, 10000);
        QTest::qWait(200);
        QCOMPARE(finished.count(), 0);
    }

    void stopBeforeAnyEventLoopTurnIsClean()
    {
        Rig rig(a, a);
        Tunnel tunnel(rig.config);
        QVERIFY(tunnel.start());
        tunnel.stop();
        QTest::qWait(300);
        QCOMPARE(rig.host.encryptedConnections(), 0);
    }

    void startupTimeoutEndsAHostThatNeverAnswers()
    {
        Rig rig(a, a);
        rig.config.startupTimeoutMs = 800;
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY2(finished.at(0).at(0).toString().contains(QStringLiteral("timed out")),
                 qPrintable(finished.at(0).at(0).toString()));
        QVERIFY2(clock.elapsed() < 5000, "the 800 ms deadline was not honoured");
        QCOMPARE(forwarding.count(), 0);
    }

    void backPressureStopsReadingWhenTheLocalSideIsSlow()
    {
        Rig rig(a, a);
        rig.reply = "{\"op\":\"ready\"}\n";
        rig.local.setPaused(true); // usbipd stops reading
        Tunnel tunnel(rig.config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.local.socket() != nullptr, 10000);

        constexpr qsizetype kTotal = 48 * 1024 * 1024;
        QByteArray payload(kTotal, 0);
        for (qsizetype i = 0; i < kTotal; ++i) {
            payload[i] = static_cast<char>((i * 7 + i / 251) & 0xFF);
        }
        rig.host.lastSocket()->write(payload);
        QTest::qWait(2500);
        // The tunnel must not have swallowed the stream into memory: with the
        // local side stalled, most of it is still queued on the host side.
        // (encryptedBytesToWrite() reports the unsent ciphertext on Schannel;
        // other backends report 0, so the check only applies there.)
        if (QSslSocket::activeBackend() != QStringLiteral("schannel")) {
            QSKIP("queue depth is only observable on the Schannel backend");
        }
        const qint64 queued = rig.host.lastSocket()->bytesToWrite() + rig.host.lastSocket()->encryptedBytesToWrite();
        QVERIFY2(queued > 16 * 1024 * 1024, qPrintable(QString::number(queued)));

        rig.local.setPaused(false);
        QTRY_COMPARE_WITH_TIMEOUT(rig.local.received().size(), kTotal, 60000);
        QVERIFY2(rig.local.received() == payload, "bytes were lost or reordered");
    }

    void logCaptureIsLiveAndTheTokenIsNeverLogged()
    {
        // The tunnel itself writes no log lines. This proves the capture works
        // (a canary is seen) and that the token is absent from everything the
        // process logged, including the failure messages handed to the UI.
        qInfo() << "canary-line";
        Rig rig(b, a); // impostor: a failure path
        rig.reply = "{\"op\":\"ready\"}\n";
        Tunnel tunnel(rig.config);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(g_LogMessages.contains(QStringLiteral("canary-line")));
        for (const QString &line : std::as_const(g_LogMessages)) {
            QVERIFY(!line.contains(QString::fromLatin1(kToken)));
        }
        QVERIFY(!finished.at(0).at(0).toString().contains(QString::fromLatin1(kToken)));
    }

    void invalidConfigurationNeverConnects_data()
    {
        QTest::addColumn<QByteArray>("busId");
        QTest::addColumn<QString>("localHost");
        QTest::addColumn<QByteArray>("token");
        QTest::addColumn<bool>("pinned");
        QTest::newRow("busid space") << QByteArray("1 2") << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("busid quote") << QByteArray("1-2\"") << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("busid semicolon") << QByteArray("1-2;x") << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("busid newline") << QByteArray("1-2\n{") << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("busid too long") << QByteArray(32, '1') << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("busid empty") << QByteArray() << QStringLiteral("127.0.0.1") << kToken << true;
        QTest::newRow("local host remote") << QByteArray("1-2") << QStringLiteral("192.168.1.50") << kToken << true;
        QTest::newRow("local host name") << QByteArray("1-2") << QStringLiteral("example.com") << kToken << true;
        QTest::newRow("no token") << QByteArray("1-2") << QStringLiteral("127.0.0.1") << QByteArray() << true;
        QTest::newRow("not paired") << QByteArray("1-2") << QStringLiteral("127.0.0.1") << kToken << false;
    }
    void invalidConfigurationNeverConnects()
    {
        QFETCH(QByteArray, busId);
        QFETCH(QString, localHost);
        QFETCH(QByteArray, token);
        QFETCH(bool, pinned);
        Rig rig(a, pinned ? a : TestIdentity());
        rig.config.busId = busId;
        rig.config.localHost = localHost;
        rig.config.sessionToken = token;
        Tunnel tunnel(rig.config);
        QSignalSpy finished(&tunnel, &Tunnel::finished);
        QString error;
        QVERIFY(!tunnel.start(&error));
        QVERIFY(!error.isEmpty());
        QTest::qWait(300);
        QCOMPARE(rig.host.connections(), 0);
        QCOMPARE(finished.count(), 0);
    }
};

QTEST_MAIN(TunnelTest)
#include "main.moc"
