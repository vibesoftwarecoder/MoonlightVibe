// NvHTTP::getUsbForwardingCapability against fake HTTPS hosts on loopback:
// pin, size cap, redirects, status handling. The real nvhttp.cpp is compiled
// (a copy, next to stubs for its collaborators; see the .pro).
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "nvhttp.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QtTest>
#include <functional>

namespace {
QStringList g_LogMessages;
void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_LogMessages.append(message);
}
const QByteArray kToken(64, 'd');

QByteArray readyJson()
{
    return "{\"version\":1,\"enabled\":true,\"available\":true,\"reason\":\"ready\",\"port\":47996,\"token\":\"" +
           kToken + "\"}";
}

QByteArray http(int status, const QByteArray &reason, const QByteArray &body, const QByteArray &extra = QByteArray())
{
    return "HTTP/1.1 " + QByteArray::number(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

// Runs fn and returns the exception text, or null if nothing was thrown.
QString thrown(const std::function<void()> &fn)
{
    try {
        fn();
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return QString();
}

// Answers every request with one canned response, then closes.
void serveCanned(FakeTlsServer &server, const QByteArray &response, QByteArray *firstRequestLine = nullptr)
{
    server.onData = [response, firstRequestLine](QSslSocket *socket, const QByteArray &all) {
        if (!all.contains("\r\n\r\n")) {
            return;
        }
        if (firstRequestLine) {
            *firstRequestLine = all.left(all.indexOf("\r\n"));
        }
        socket->write(response);
        socket->disconnectFromHost();
    };
}
} // namespace

class NvHttpCapabilityTest : public QObject
{
    Q_OBJECT

    TestIdentity a, b;

    // Fresh NvHTTP pinned to certificate A and aimed at the fake host.
    std::unique_ptr<NvHTTP> client(quint16 port, const TestIdentity &pinned)
    {
        return std::make_unique<NvHTTP>(NvAddress(QStringLiteral("127.0.0.1"), 47989), port,
                                        pinned.certificate, false);
    }

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
        for (const QString &line : std::as_const(g_LogMessages)) {
            QVERIFY2(!line.contains(QString::fromLatin1(kToken)), "token was logged");
        }
    }

    void readyReplyYieldsPortAndToken()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        QByteArray requestLine;
        serveCanned(host, http(200, "OK", readyJson()), &requestLine);
        auto nv = client(host.port(), a);

        UsbForwarding::Capability capability;
        QVERIFY2(thrown([&] { capability = nv->getUsbForwardingCapability(); }).isNull(), "unexpected exception");
        QVERIFY(capability.available);
        QCOMPARE(capability.port, quint16(47996));
        QCOMPARE(capability.token, kToken);
        QVERIFY2(requestLine.startsWith("GET /api/v1/usb-forwarding"), requestLine.constData());
    }

    void disabledReplyIsNotAnError()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK",
                               "{\"version\":1,\"enabled\":false,\"available\":false,\"reason\":\"disabled\"}"));
        auto nv = client(host.port(), a);
        UsbForwarding::Capability capability;
        QVERIFY(thrown([&] { capability = nv->getUsbForwardingCapability(); }).isNull());
        QVERIFY(!capability.available);
        QCOMPARE(capability.reason, QStringLiteral("disabled"));
    }

    void responseCapIsExactly4096Bytes()
    {
        QByteArray atCap = readyJson();
        atCap.append(QByteArray(4096 - atCap.size(), ' '));
        {
            FakeTlsServer host(a);
            QVERIFY(host.listen());
            serveCanned(host, http(200, "OK", atCap));
            auto nv = client(host.port(), a);
            QVERIFY2(thrown([&] { nv->getUsbForwardingCapability(); }).isNull(), "4096 bytes must be accepted");
        }
        {
            FakeTlsServer host(a);
            QVERIFY(host.listen());
            serveCanned(host, http(200, "OK", atCap + " "));
            auto nv = client(host.port(), a);
            const QString message = thrown([&] { nv->getUsbForwardingCapability(); });
            QVERIFY2(message.contains(QStringLiteral("too large")), qPrintable(message));
        }
    }

    void hugeResponseIsCutOff()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK", QByteArray(3 * 1024 * 1024, 'z')));
        auto nv = client(host.port(), a);
        const QString message = thrown([&] { nv->getUsbForwardingCapability(); });
        QVERIFY2(message.contains(QStringLiteral("too large")), qPrintable(message));
    }

    void redirectsAreNeverFollowed()
    {
        // The redirect target presents the same trusted certificate, so a
        // client that followed redirects would be served by it.
        FakeTlsServer target(a);
        QVERIFY(target.listen());
        serveCanned(target, http(200, "OK", readyJson()));

        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(302, "Found", QByteArray(),
                               "Location: https://127.0.0.1:" + QByteArray::number(target.port()) + "/steal\r\n"));
        auto nv = client(host.port(), a);

        UsbForwarding::Capability capability;
        const QString message = thrown([&] { capability = nv->getUsbForwardingCapability(); });
        QVERIFY2(!message.isNull(), "a redirect must be an error");
        QVERIFY2(message.contains(QStringLiteral("redirect")), qPrintable(message));
        QCOMPARE(target.connections(), 0);
        QVERIFY(!capability.available);
    }

    void nonOkStatusIsAnError()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(404, "Not Found", "{}"));
        auto nv = client(host.port(), a);
        QVERIFY(!thrown([&] { nv->getUsbForwardingCapability(); }).isNull());
    }

    void garbageBodyIsAnError()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK", "<html>not json</html>"));
        auto nv = client(host.port(), a);
        QVERIFY(!thrown([&] { nv->getUsbForwardingCapability(); }).isNull());
    }

    void impostorCertificateGetsNoRequestAndNoCredentialsAreAccepted()
    {
        // Host presents B, we paired with A, and its reply carries a token.
        // B is trusted by the client's CA store and matches 127.0.0.1 (see the
        // identitymanager.h stub), so ordinary TLS verification accepts it and
        // only the pin can refuse it.
        FakeTlsServer host(b);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK", readyJson()));
        auto nv = client(host.port(), a);
        UsbForwarding::Capability capability;
        const QString message = thrown([&] { capability = nv->getUsbForwardingCapability(); });
        QVERIFY2(!message.isNull(), "an unpinned certificate must be an error");
        QVERIFY(!capability.available);
        QVERIFY(capability.token.isEmpty());
        // The request (which carries our client identity) was never sent.
        QVERIFY2(host.received().isEmpty(), "request reached an unpinned host");
    }

    void unpairedHostIsRefusedWithoutConnecting()
    {
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK", readyJson()));
        auto nv = client(host.port(), TestIdentity());
        const QString message = thrown([&] { nv->getUsbForwardingCapability(); });
        QVERIFY2(message.contains(QStringLiteral("paired")), qPrintable(message));
        QCOMPARE(host.connections(), 0);
    }

    void unboundedRequestsKeepTheirOldBehaviour()
    {
        // Existing callers pass no size cap, so a large reply still comes back whole.
        FakeTlsServer host(a);
        QVERIFY(host.listen());
        serveCanned(host, http(200, "OK", QByteArray(300000, 'q')));
        auto nv = client(host.port(), a);
        QString body;
        QVERIFY(thrown([&] {
            body = nv->openConnectionToString(nv->m_BaseUrlHttps, QStringLiteral("serverinfo"), QString(), 5000);
        }).isNull());
        QCOMPARE(body.size(), 300000);
    }
};

QTEST_GUILESS_MAIN(NvHttpCapabilityTest)
#include "main.moc"
