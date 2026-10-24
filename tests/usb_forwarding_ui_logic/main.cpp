// The pure logic behind the USB forwarding settings section.
#include "../../app/backend/usbforwardinguilogic.h"

#include <QSet>
#include <QtTest>

using namespace UsbForwarding::Ui;

class UiLogicTest : public QObject
{
    Q_OBJECT

    static QVariantMap device(bool bound, bool connected, bool attached, bool supported)
    {
        QVariantMap m;
        m.insert(QStringLiteral("isBound"), bound);
        m.insert(QStringLiteral("isConnected"), connected);
        m.insert(QStringLiteral("isAttached"), attached);
        m.insert(QStringLiteral("isSupported"), supported);
        return m;
    }

private slots:
    void deviceStates_data()
    {
        QTest::addColumn<bool>("bound");
        QTest::addColumn<bool>("connected");
        QTest::addColumn<bool>("attached");
        QTest::addColumn<bool>("supported");
        QTest::addColumn<int>("state");
        QTest::addColumn<bool>("canShare");
        QTest::addColumn<bool>("canStop");
        const auto s = [](DeviceShareState x) { return static_cast<int>(x); };
        QTest::newRow("plugged in, not shared") << false << true << false << true << s(DeviceShareState::NotShared) << true << false;
        QTest::newRow("shared") << true << true << false << true << s(DeviceShareState::Shared) << false << true;
        QTest::newRow("shared, unplugged") << true << false << false << false << s(DeviceShareState::SharedNotPlugged) << false << true;
        QTest::newRow("in use by a client") << true << true << true << true << s(DeviceShareState::Busy) << false << false;
        QTest::newRow("busy beats everything") << false << true << true << true << s(DeviceShareState::Busy) << false << false;
        QTest::newRow("cannot be shared") << false << true << false << false << s(DeviceShareState::Unsupported) << false << false;
        QTest::newRow("not connected, never shared") << false << false << false << false << s(DeviceShareState::Unsupported) << false << false;
    }
    void deviceStates()
    {
        QFETCH(bool, bound);
        QFETCH(bool, connected);
        QFETCH(bool, attached);
        QFETCH(bool, supported);
        QFETCH(int, state);
        QFETCH(bool, canShare);
        QFETCH(bool, canStop);
        const QVariantMap d = device(bound, connected, attached, supported);
        QCOMPARE(static_cast<int>(deviceShareState(d)), state);
        QCOMPARE(UsbForwarding::Ui::canShare(d), canShare);
        QCOMPARE(UsbForwarding::Ui::canStopSharing(d), canStop);
    }

    void emptyDeviceMapIsNotShareable()
    {
        QCOMPARE(deviceShareState(QVariantMap()), DeviceShareState::Unsupported);
        QVERIFY(!UsbForwarding::Ui::canShare(QVariantMap()));
        QVERIFY(!UsbForwarding::Ui::canStopSharing(QVariantMap()));
    }

    void everyDeviceStateHasItsOwnText()
    {
        QSet<QString> seen;
        for (DeviceShareState s : { DeviceShareState::NotShared, DeviceShareState::Shared, DeviceShareState::SharedNotPlugged,
                                    DeviceShareState::Busy, DeviceShareState::Unsupported }) {
            const QString text = deviceShareStateText(s);
            QVERIFY(!text.isEmpty());
            QVERIFY(!seen.contains(text));
            seen.insert(text);
        }
        QCOMPARE(deviceShareStateText(DeviceShareState::Shared), QStringLiteral("Shared"));
        QCOMPARE(deviceShareStateText(DeviceShareState::NotShared), QStringLiteral("Not shared"));
        QVERIFY(deviceShareStateText(DeviceShareState::Busy).startsWith(QStringLiteral("Busy")));
    }

    void environmentTextCoversEveryState()
    {
        using E = UsbForwardingEnvironment;
        QSet<QString> seen;
        for (E::State s : { E::Checking, E::NotInstalled, E::ServiceStopped, E::Ready, E::DriverStopped,
                            E::CheckFailed, E::Unsupported }) {
            const QString text = environmentStatusText(s, QString());
            QVERIFY(!text.isEmpty());
            QVERIFY(!seen.contains(text));
            seen.insert(text);
            // Plain text only: nothing that looks like a link or a command to run.
            QVERIFY(!text.contains(QStringLiteral("http")));
            QVERIFY(!text.contains(QStringLiteral("://")));
            QVERIFY(!text.contains(QStringLiteral("winget")));
            QVERIFY(!text.contains(QStringLiteral("<")));
        }
        QVERIFY(environmentStatusText(E::NotInstalled, QString()).contains(QStringLiteral("Install usbipd-win yourself")));
        QVERIFY(environmentStatusText(E::ServiceStopped, QString()).contains(QStringLiteral("service is not running")));
    }

    void readyTextShowsTheVersionOnlyWhenKnown()
    {
        using E = UsbForwardingEnvironment;
        QVERIFY(environmentStatusText(E::Ready, QStringLiteral("5.1.0")).contains(QStringLiteral("5.1.0")));
        QVERIFY(!environmentStatusText(E::Ready, QString()).contains(QStringLiteral("%1")));
        // Only the Ready state may claim that usbipd-win is installed and running.
        QVERIFY(!environmentStatusText(E::NotInstalled, QStringLiteral("5.1.0")).contains(QStringLiteral("5.1.0")));
    }

    void addingADeviceKeepsOrderAndAddsOnce()
    {
        const QStringList base = { QStringLiteral("aaaa:0001"), QStringLiteral("bbbb:0002#x") };
        QStringList list = autoDevicesWith(base, QStringLiteral("cccc:0003"), true);
        QCOMPARE(list, (QStringList{ "aaaa:0001", "bbbb:0002#x", "cccc:0003" }));
        list = autoDevicesWith(list, QStringLiteral("cccc:0003"), true);
        QCOMPARE(list.count(QStringLiteral("cccc:0003")), 1);
        QCOMPARE(list.size(), 3);
        QCOMPARE(base.size(), 2);   // the input is a value, never changed
    }

    void removingADeviceRemovesEveryCopyAndOnlyThat()
    {
        const QStringList base = { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("a#1") };
        QCOMPARE(autoDevicesWith(base, QStringLiteral("a"), false), (QStringList{ "b", "a#1" }));
        QCOMPARE(autoDevicesWith(base, QStringLiteral("zzz"), false), base);
    }

    void emptyIdentityIsNeverStored()
    {
        const QStringList base = { QStringLiteral("a") };
        QCOMPARE(autoDevicesWith(base, QString(), true), base);
        QCOMPARE(autoDevicesWith(base, QString(), false), base);
        QVERIFY(!autoDevicesContain(QStringList{ QString() }, QString()));
    }

    void hotkeyIsOnlyEnabledOnWindowsWithThePreferenceOn_data()
    {
        QTest::addColumn<bool>("windows");
        QTest::addColumn<bool>("preference");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("windows, on") << true << true << true;
        QTest::newRow("windows, off") << true << false << false;
        QTest::newRow("other platform, on") << false << true << false;
        QTest::newRow("other platform, off") << false << false << false;
    }
    void hotkeyIsOnlyEnabledOnWindowsWithThePreferenceOn()
    {
        QFETCH(bool, windows);
        QFETCH(bool, preference);
        QFETCH(bool, enabled);
        QCOMPARE(usbForwardingHotkeyEnabled(windows, preference), enabled);
    }

    void containsIsExact()
    {
        const QStringList base = { QStringLiteral("054c:0ce6#S1") };
        QVERIFY(autoDevicesContain(base, QStringLiteral("054c:0ce6#S1")));
        QVERIFY(!autoDevicesContain(base, QStringLiteral("054c:0ce6")));
        QVERIFY(!autoDevicesContain(base, QStringLiteral("054c:0ce6#S2")));
    }
};

QTEST_GUILESS_MAIN(UiLogicTest)
#include "main.moc"
