// `usbipd state` parsing and the elevated command lines, without running
// usbipd or showing a UAC prompt.
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "../../app/backend/usbforwardingbackend.h"

#include <QSignalSpy>
#include <QtTest>

class BackendListTest : public QObject
{
    Q_OBJECT

    static QVariantList parse(const QByteArray &json, QString *error)
    {
        return UsbForwardingBackend::parseUsbipdState(json, error);
    }

    static QVariantMap onlyDevice(const QByteArray &json)
    {
        QString error;
        const QVariantList devices = parse(json, &error);
        return (error.isEmpty() && devices.size() == 1) ? devices.first().toMap() : QVariantMap();
    }

private slots:
    void sharedDevice()
    {
        const QVariantMap device = onlyDevice(
            R"({"Devices":[{"BusId":"1-2","ClientIPAddress":null,"Description":"DualSense Wireless Controller",)"
            R"("InstanceId":"USB\\VID_054C&PID_0CE6\\5&1&0&2","IsForced":false,)"
            R"("PersistedGuid":"9b6a62f8-0123-4567-89ab-0123456789ab","StubInstanceId":null}]})");
        QVERIFY(!device.isEmpty());
        QCOMPARE(device.value("busId").toString(), QStringLiteral("1-2"));
        QCOMPARE(device.value("description").toString(), QStringLiteral("DualSense Wireless Controller"));
        QCOMPARE(device.value("vidPid").toString(), QStringLiteral("054c:0ce6"));
        QVERIFY(device.value("isBound").toBool());
        QVERIFY(device.value("isConnected").toBool());
        QVERIFY(!device.value("isAttached").toBool());
        QVERIFY(device.value("isSupported").toBool());
        QVERIFY(!device.value("isForced").toBool());
        QCOMPARE(device.value("persistedGuid").toString(), QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab"));
    }

    void attachedAndUnboundDevice()
    {
        const QVariantMap device = onlyDevice(
            R"({"Devices":[{"BusId":"3-1","ClientIPAddress":"192.168.1.9","Description":"Keyboard",)"
            R"("InstanceId":"USB\\VID_0001&PID_0002\\x","IsForced":true,"PersistedGuid":null}]})");
        QVERIFY(!device.isEmpty());
        QVERIFY(!device.value("isBound").toBool());
        QVERIFY(device.value("isAttached").toBool());
        QVERIFY(device.value("isForced").toBool());
        QVERIFY(device.value("isSupported").toBool());
    }

    void disconnectedAndIncompatibleDevicesAreNotSupported()
    {
        // Persisted share whose device is unplugged: BusId is null.
        const QVariantMap unplugged = onlyDevice(
            R"({"Devices":[{"BusId":null,"Description":"Gone","InstanceId":"USB\\VID_0001&PID_0002\\x",)"
            R"("PersistedGuid":"9b6a62f8-0123-4567-89ab-0123456789ab"}]})");
        QVERIFY(!unplugged.isEmpty());
        QVERIFY(!unplugged.value("isConnected").toBool());
        QVERIFY(!unplugged.value("isSupported").toBool());
        QVERIFY(unplugged.value("isBound").toBool());

        const QVariantMap hub = onlyDevice(R"({"Devices":[{"BusId":"IncompatibleHub","Description":"Hub"}]})");
        QVERIFY(!hub.isEmpty());
        QVERIFY(!hub.value("isSupported").toBool());

        // A hostile bus id in the listing is shown at most, never offered.
        const QVariantMap hostile = onlyDevice(R"({"Devices":[{"BusId":"1-2 & calc","Description":"x"}]})");
        QVERIFY(!hostile.isEmpty());
        QVERIFY(!hostile.value("isSupported").toBool());
    }

    void emptyAndMalformedState()
    {
        QString error = QStringLiteral("stale");
        QVERIFY(parse(R"({"Devices":[]})", &error).isEmpty());
        QVERIFY(error.isEmpty());

        for (const char *invalid : { "", "not json", "[]", "{}", R"({"Devices":42})", R"({"Devices":[{"BusId":)" }) {
            error.clear();
            QVERIFY2(parse(invalid, &error).isEmpty(), invalid);
            QVERIFY2(!error.isEmpty(), invalid);
        }
    }

    void busIdShapes_data()
    {
        QTest::addColumn<QString>("busId");
        QTest::addColumn<bool>("shareable");
        QTest::newRow("1-2") << QStringLiteral("1-2") << true;
        QTest::newRow("12-34") << QStringLiteral("12-34") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("hub") << QStringLiteral("IncompatibleHub") << false;
        QTest::newRow("leading zero") << QStringLiteral("01-2") << false;
        QTest::newRow("zero port") << QStringLiteral("1-0") << false;
        QTest::newRow("dotted") << QStringLiteral("1-2.3") << false;
        QTest::newRow("trailing newline") << QStringLiteral("1-2\n") << false;
        QTest::newRow("extra option") << QStringLiteral("1-2 --force") << false;
        QTest::newRow("quote break") << QStringLiteral("1-2\" --evil \"") << false;
        QTest::newRow("shell meta") << QStringLiteral("1-2&calc") << false;
        QTest::newRow("too long") << QStringLiteral("1-") + QString(40, QLatin1Char('9')) << false;
    }
    void busIdShapes()
    {
        QFETCH(QString, busId);
        QFETCH(bool, shareable);
        QCOMPARE(UsbForwardingBackend::isShareableBusId(busId), shareable);
        // bindArguments is the only way a bus id reaches the elevated command line.
        QCOMPARE(!UsbForwardingBackend::bindArguments(busId).isEmpty(), shareable);
        if (shareable) {
            QCOMPARE(UsbForwardingBackend::bindArguments(busId), QStringLiteral("bind --busid ") + busId);
        }
    }

    void unbindArgumentForms()
    {
        const QString guid = QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab");
        QCOMPARE(UsbForwardingBackend::unbindArguments(QStringLiteral("1-2"), guid),
                 QStringLiteral("unbind --busid 1-2"));
        QCOMPARE(UsbForwardingBackend::unbindArguments(QString(), guid),
                 QStringLiteral("unbind --guid ") + guid);
        // A device that is unplugged has no usable bus id: fall back to the GUID.
        QCOMPARE(UsbForwardingBackend::unbindArguments(QStringLiteral("IncompatibleHub"), guid),
                 QStringLiteral("unbind --guid ") + guid);
        QVERIFY(UsbForwardingBackend::unbindArguments(QString(), QString()).isEmpty());
        QVERIFY(UsbForwardingBackend::unbindArguments(QStringLiteral("1-2 --force"), QString()).isEmpty());
    }

    void guidRejectsInjection_data()
    {
        QTest::addColumn<QString>("guid");
        QTest::addColumn<bool>("valid");
        QTest::newRow("good") << QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab") << true;
        QTest::newRow("upper") << QStringLiteral("9B6A62F8-0123-4567-89AB-0123456789AB") << true;
        QTest::newRow("braces") << QStringLiteral("{9b6a62f8-0123-4567-89ab-0123456789ab}") << false;
        QTest::newRow("trailing option") << QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab --all") << false;
        QTest::newRow("quote") << QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab\"") << false;
        QTest::newRow("short") << QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789a") << false;
        QTest::newRow("non hex") << QStringLiteral("zb6a62f8-0123-4567-89ab-0123456789ab") << false;
        QTest::newRow("newline") << QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab\n") << false;
        QTest::newRow("empty") << QString() << false;
    }
    void guidRejectsInjection()
    {
        QFETCH(QString, guid);
        QFETCH(bool, valid);
        QCOMPARE(UsbForwardingBackend::isValidPersistedGuid(guid), valid);
        QCOMPARE(!UsbForwardingBackend::unbindArguments(QString(), guid).isEmpty(), valid);
    }

    void invalidBusIdIsRefusedBeforeAnyElevation()
    {
        // Safe to call: an invalid id returns before locating or launching anything.
        UsbForwardingBackend *backend = UsbForwardingBackend::get();
        QSignalSpy spy(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2 & calc"));
        backend->unbind(QStringLiteral("x\" --evil"), QStringLiteral("not-a-guid"));
        QCOMPARE(spy.count(), 2);
        QVERIFY(!spy.at(0).at(0).toBool());
        QVERIFY(!spy.at(1).at(0).toBool());
    }

    void identityIsStableAcrossPortsAndUsesARealSerial_data()
    {
        QTest::addColumn<QString>("instanceId");
        QTest::addColumn<QString>("expected");
        // Windows-invented ids contain '&' and change with the port: no serial.
        QTest::newRow("generated id") << QStringLiteral("USB\\VID_054C&PID_0CE6\\5&1&0&2") << QStringLiteral("054c:0ce6");
        QTest::newRow("same device other port") << QStringLiteral("USB\\VID_054C&PID_0CE6\\6&2a3c&0&4") << QStringLiteral("054c:0ce6");
        QTest::newRow("real serial") << QStringLiteral("USB\\VID_0781&PID_5581\\4C530001230101105424")
                                     << QStringLiteral("0781:5581#4c530001230101105424");
        QTest::newRow("serial case folded") << QStringLiteral("USB\\VID_0781&PID_5581\\AbC-123_x.y")
                                            << QStringLiteral("0781:5581#abc-123_x.y");
        QTest::newRow("no third part") << QStringLiteral("USB\\VID_0001&PID_0002") << QStringLiteral("0001:0002");
        QTest::newRow("odd characters in the serial") << QStringLiteral("USB\\VID_0001&PID_0002\\a b;c") << QStringLiteral("0001:0002");
        QTest::newRow("no vid/pid") << QStringLiteral("ROOT\\HUB\\0000") << QString();
        QTest::newRow("empty") << QString() << QString();
    }
    void identityIsStableAcrossPortsAndUsesARealSerial()
    {
        QFETCH(QString, instanceId);
        QFETCH(QString, expected);
        QCOMPARE(UsbForwardingBackend::identityFromInstanceId(instanceId), expected);
    }

    void listedDevicesCarryTheirIdentity()
    {
        const QVariantMap device = onlyDevice(
            R"({"Devices":[{"BusId":"1-2","ClientIPAddress":null,"Description":"Stick",)"
            R"("InstanceId":"USB\\VID_0781&PID_5581\\ABC123","IsForced":false,)"
            R"("PersistedGuid":"9b6a62f8-0123-4567-89ab-0123456789ab","StubInstanceId":null}]})");
        QCOMPARE(device.value("identity").toString(), QStringLiteral("0781:5581#abc123"));
        // The bus id is NOT part of it: it changes when the device moves.
        QVERIFY(!device.value("identity").toString().contains(QStringLiteral("1-2")));
    }
};

QTEST_MAIN(BackendListTest)
#include "main.moc"
