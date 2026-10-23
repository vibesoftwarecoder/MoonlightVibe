#pragma once

// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include <QObject>
#include <QString>
#include <QVariantList>

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
    explicit UsbForwardingBackend(QObject *parent = nullptr);

    void setBusy(bool busy);
    void setError(const QString &error);
    // Runs the validated usbipd.exe elevated with the given fixed arguments.
    void runElevated(const QString &arguments, const QString &successMessage);

    QVariantList m_Devices;
    bool m_Busy = false;
    QString m_Error;
};
