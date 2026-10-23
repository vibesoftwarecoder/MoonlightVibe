// The tunnel presents the paired client certificate to the host.
// Separate from usb_forwarding_tunnel because Qt's Schannel backend never asks
// a client for its certificate when acting as a server, so a fake host can
// only observe it with the OpenSSL backend (DLLs come from the runner's PATH).
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "../../app/backend/usbforwardingtunnel.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

using UsbForwarding::Tunnel;
using UsbForwarding::TunnelConfig;

class ClientCertTest : public QObject
{
    Q_OBJECT

private slots:
    void clientCertificateIsPresentedToTheHost()
    {
        QVERIFY2(QSslSocket::availableBackends().contains(QStringLiteral("openssl")), "OpenSSL backend missing");
        QCOMPARE(QSslSocket::activeBackend(), QStringLiteral("openssl")); // via QT_TLS_BACKEND
        QVERIFY2(QSslSocket::supportsSsl(), "OpenSSL libraries not found on PATH");

        const TestIdentity hostIdentity = loadTestIdentity(QStringLiteral("a"));
        const TestIdentity clientIdentity = loadTestIdentity(QStringLiteral("b")); // distinct from the host's
        QVERIFY(hostIdentity.valid() && clientIdentity.valid());

        FakeTlsServer host(hostIdentity);
        QVERIFY(host.listen());
        host.onData = [](QSslSocket *socket, const QByteArray &all) {
            if (all.contains('\n')) {
                socket->write("{\"op\":\"ready\"}\n");
            }
        };
        QTcpServer local; // stands in for usbipd
        QVERIFY(local.listen(QHostAddress::LocalHost, 0));

        TunnelConfig config;
        config.host = QStringLiteral("127.0.0.1");
        config.port = host.port();
        config.sessionToken = QByteArray(64, 'e');
        config.pinnedServerCertificate = hostIdentity.certificate;
        config.busId = "1-2";
        config.localPort = local.serverPort();
        config.sslConfiguration.setLocalCertificate(clientIdentity.certificate);
        config.sslConfiguration.setPrivateKey(clientIdentity.key);

        Tunnel tunnel(config);
        QSignalSpy forwarding(&tunnel, &Tunnel::forwarding);
        QVERIFY(tunnel.start());
        QTRY_COMPARE_WITH_TIMEOUT(forwarding.count(), 1, 10000);
        QVERIFY2(host.lastPeerCertificate() == clientIdentity.certificate,
                 "the host did not receive our client certificate");
    }
};

QTEST_MAIN(ClientCertTest)
#include "main.moc"
