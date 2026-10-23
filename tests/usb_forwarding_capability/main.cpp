// Capability JSON parsing and the shared bus id rule.
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "../../app/backend/usbforwardingcapability.h"

#include <QtTest>

using UsbForwarding::Capability;

class CapabilityTest : public QObject
{
    Q_OBJECT

    static QByteArray token() { return QByteArray(64, 'a'); }
    static QByteArray ready(const QByteArray &port = "47996")
    {
        return "{\"version\":1,\"enabled\":true,\"available\":true,\"reason\":\"ready\",\"port\":" +
               port + ",\"token\":\"" + token() + "\"}";
    }

private slots:
    void readyCarriesPortAndToken()
    {
        const auto capability = Capability::parse(ready());
        QVERIFY(capability.has_value());
        QVERIFY(capability->available);
        QCOMPARE(capability->port, quint16(47996));
        QCOMPARE(capability->token, token());
    }

    void disabledAndUnavailableCarryNoCredentials()
    {
        for (const char *enabled : { "true", "false" }) {
            const auto disabled = Capability::parse(
                QByteArray("{\"version\":1,\"enabled\":") + enabled + ",\"available\":false}");
            QVERIFY(disabled.has_value());
            QVERIFY(!disabled->available);
            QVERIFY(disabled->token.isEmpty());
            QCOMPARE(disabled->port, quint16(0));
        }
        // enabled but not available must never be treated as ready, even
        // if a token is present.
        const auto stray = Capability::parse(
            "{\"version\":1,\"enabled\":false,\"available\":true,\"port\":5,\"token\":\"" + token() + "\"}");
        QVERIFY(stray.has_value());
        QVERIFY(!stray->available);
        QVERIFY(stray->token.isEmpty());
    }

    void portRangeIsValidated_data()
    {
        QTest::addColumn<QByteArray>("port");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("1") << QByteArray("1") << true;
        QTest::newRow("65535") << QByteArray("65535") << true;
        QTest::newRow("0") << QByteArray("0") << false;
        QTest::newRow("-1") << QByteArray("-1") << false;
        QTest::newRow("65536") << QByteArray("65536") << false;
        QTest::newRow("fraction") << QByteArray("47996.5") << false;
        QTest::newRow("string") << QByteArray("\"47996\"") << false;
        QTest::newRow("huge") << QByteArray("1e30") << false;
        QTest::newRow("null") << QByteArray("null") << false;
    }
    void portRangeIsValidated()
    {
        QFETCH(QByteArray, port);
        QFETCH(bool, accepted);
        QCOMPARE(Capability::parse(ready(port)).has_value(), accepted);
    }

    void malformedResponsesAreRejected_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("empty object") << QByteArray("{}");
        QTest::newRow("not json") << QByteArray("hello");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("short token") << QByteArray(ready()).replace(token(), "");
        QTest::newRow("non hex token") << QByteArray(ready()).replace(token(), QByteArray(64, 'z'));
        QTest::newRow("long token") << QByteArray(ready()).replace(token(), QByteArray(65, 'a'));
        QTest::newRow("version 2") << QByteArray(ready()).replace("\"version\":1", "\"version\":2");
        QTest::newRow("version string") << QByteArray(ready()).replace("\"version\":1", "\"version\":\"1\"");
        QTest::newRow("enabled string") << QByteArray(ready()).replace("\"enabled\":true", "\"enabled\":\"true\"");
    }
    void malformedResponsesAreRejected()
    {
        QFETCH(QByteArray, body);
        QVERIFY(!Capability::parse(body).has_value());
    }

    void responseSizeCapIsExactly4096()
    {
        QByteArray padded = ready();
        padded.append(QByteArray(UsbForwarding::kMaxHandshakeBytes - padded.size(), ' '));
        QCOMPARE(padded.size(), qsizetype(4096));
        QVERIFY(Capability::parse(padded).has_value());
        QVERIFY(!Capability::parse(padded + ' ').has_value());
    }

    void busIdRule_data()
    {
        QTest::addColumn<QByteArray>("busId");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("plain") << QByteArray("1-2") << true;
        QTest::newRow("hub path") << QByteArray("3-1.2.4") << true;
        QTest::newRow("colon") << QByteArray("usb:1") << true;
        QTest::newRow("letters") << QByteArray("AbZ-09") << true;
        QTest::newRow("31 bytes") << QByteArray(31, 'a') << true;
        QTest::newRow("32 bytes") << QByteArray(32, 'a') << false;
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("space") << QByteArray("1 2") << false;
        QTest::newRow("quote") << QByteArray("1-2\"") << false;
        QTest::newRow("semicolon") << QByteArray("1-2;calc") << false;
        QTest::newRow("ampersand") << QByteArray("1-2&calc") << false;
        QTest::newRow("percent") << QByteArray("%PATH%") << false;
        QTest::newRow("newline") << QByteArray("1-2\n") << false;
        QTest::newRow("nul") << QByteArray("1-2\0x", 5) << false;
        QTest::newRow("slash") << QByteArray("../x") << false;
        QTest::newRow("backslash") << QByteArray("1\\2") << false;
        QTest::newRow("utf8") << QByteArray("\xC3\xA9") << false;
        QTest::newRow("high bit") << QByteArray("1-\x80") << false;
    }
    void busIdRule()
    {
        QFETCH(QByteArray, busId);
        QFETCH(bool, accepted);
        QCOMPARE(UsbForwarding::isValidBusId(busId), accepted);
    }
};

QTEST_MAIN(CapabilityTest)
#include "main.moc"
