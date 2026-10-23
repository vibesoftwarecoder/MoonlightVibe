#pragma once

// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include "usbforwardingelevation.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>
#include <memory>

// USB device sharing orchestration (Windows only; usbipd-win does the
// sharing). Lists devices from `usbipd state` JSON, and binds or unbinds
// them with an elevated usbipd.exe (UAC prompt). Bound state lives in
// usbipd's own registry data (PersistedGuid).
//
// SECURITY: the executable is always the validated absolute path from
// UsbForwardingEnvironment::locateUsbipd(). Elevated command lines are built
// only from a validated bus id or GUID, in a fixed form.
//
// Division of labour: UsbForwardingEnvironment checks the environment
// (version, service state); this class handles the devices.
//
// Each device in the list is a QVariantMap with these keys:
//   busId         "1-2", "IncompatibleHub", or "" (not connected)
//   description   device description
//   instanceId    Windows instance ID (USB\VID_XXXX&PID_YYYY\...)
//   vidPid        "xxxx:yyyy"
//   identity      stable identity for the settings list: "xxxx:yyyy", or
//                 "xxxx:yyyy#serial" when the device has a real serial number.
//                 Unlike busId it does not change when the device moves port.
//   isBound       bool, PersistedGuid is not empty
//   isConnected   bool, present in the current listing
//   isAttached    bool, in use by a client right now
//   isSupported   bool, connected and the bus id is a real, shareable one
//   isForced      bool
//   persistedGuid GUID of the persisted share, or ""
class UsbForwardingBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    static UsbForwardingBackend* get();

    // Parses the output of `usbipd state` into the device list above. On
    // failure sets *error and returns an empty list. Static and pure so it
    // can be unit tested without a process.
    static QVariantList parseUsbipdState(const QByteArray &stateJson, QString *error);

    // "1-2" style bus ids only: digits-digit, no leading zero. Rejects
    // "IncompatibleHub", the empty string, and anything with other characters.
    static bool isShareableBusId(const QString &busId);
    // 8-4-4-4-12 hex GUID without braces, as usbipd prints PersistedGuid.
    static bool isValidPersistedGuid(const QString &guid);
    // Arguments for the elevated usbipd.exe, built in a fixed form from a
    // validated bus id or GUID. Return an empty string for invalid input.
    static QString bindArguments(const QString &busId);
    static QString unbindArguments(const QString &busId, const QString &persistedGuid);
    // Stable device identity from a Windows instance ID (see "identity" above).
    // Empty when the instance ID has no usable VID/PID.
    // NOTE for the settings UI: two devices of the same model that have no
    // serial number share the identity "vid:pid". Selecting one selects all
    // such twins, and the UI has to say so.
    static QString identityFromInstanceId(const QString &instanceId);

#ifdef USBFWD_TESTING
    // Test seams, compiled only into the unit tests (USBFWD_TESTING is defined
    // in their .pro files, never in the application). Hooks are process wide;
    // pass a default-constructed value to restore production behaviour.
    struct TestHooks
    {
        std::function<QString()> locateUsbipd;            // replaces the usbipd.exe lookup
        UsbForwarding::ElevatedLauncher launcher;         // replaces the elevated launch
        std::function<void()> refresh;                    // replaces refresh()
        int elevationTimeoutMs = 0;                       // 0 = production value
        int elevationHardLimitMs = 0;                     // 0 = production value
    };
    static void setTestHooks(TestHooks hooks);
    // Tests make their own instances (the application uses get()).
    explicit UsbForwardingBackend(QObject *parent = nullptr);
#endif
    ~UsbForwardingBackend() override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void bind(const QString &busId);
    Q_INVOKABLE void unbind(const QString &busId, const QString &persistedGuid);

    QVariantList devices() const { return m_Devices; }
    bool busy() const { return m_Busy; }
    QString error() const { return m_Error; }

signals:
    void devicesChanged();
    void busyChanged();
    void errorChanged();
    void operationFinished(bool success, const QString &message);

private:
#ifndef USBFWD_TESTING
    explicit UsbForwardingBackend(QObject *parent = nullptr);
#endif
    struct DeliveryLink;
    void reportSlow();

    void setBusy(bool busy);
    void setError(const QString &error);
    // Runs the validated usbipd.exe elevated with the given fixed arguments on
    // a worker thread (the UAC prompt and the wait never block the caller) and
    // reports the real result through operationFinished(). On success the
    // device list is refreshed from usbipd; on failure it is left alone.
    void runElevated(const QString &arguments, const QString &successMessage);

    void finishElevated(const UsbForwarding::ElevationResult &result, const QString &successMessage);

    QVariantList m_Devices;
    bool m_Busy = false;
    bool m_SlowReported = false;
    QString m_Error;
    // Lets the elevation worker post its result without racing our deletion.
    std::shared_ptr<DeliveryLink> m_Link;
};
