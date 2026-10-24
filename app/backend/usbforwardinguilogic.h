#pragma once

#include "usbforwardingenvironment.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// Pure helpers behind the USB forwarding settings section. No I/O, no state,
// so they can be unit tested without a window.
namespace UsbForwarding {
namespace Ui {

enum class DeviceShareState
{
    NotShared,
    Shared,
    SharedNotPlugged,
    Busy,
    Unsupported,
};

// What the settings list shows for one entry of UsbForwardingBackend::devices().
inline DeviceShareState deviceShareState(const QVariantMap &device)
{
    const bool bound = device.value(QStringLiteral("isBound")).toBool();
    const bool connected = device.value(QStringLiteral("isConnected")).toBool();
    const bool attached = device.value(QStringLiteral("isAttached")).toBool();
    const bool supported = device.value(QStringLiteral("isSupported")).toBool();
    if (attached) {
        return DeviceShareState::Busy;
    }
    if (bound) {
        return connected ? DeviceShareState::Shared : DeviceShareState::SharedNotPlugged;
    }
    return supported ? DeviceShareState::NotShared : DeviceShareState::Unsupported;
}

inline QString deviceShareStateText(DeviceShareState state)
{
    switch (state) {
    case DeviceShareState::Shared:
        return QCoreApplication::translate("UsbForwardingUi", "Shared");
    case DeviceShareState::SharedNotPlugged:
        return QCoreApplication::translate("UsbForwardingUi", "Shared (not plugged in)");
    case DeviceShareState::Busy:
        return QCoreApplication::translate("UsbForwardingUi", "Busy (in use right now)");
    case DeviceShareState::Unsupported:
        return QCoreApplication::translate("UsbForwardingUi", "Cannot be shared");
    case DeviceShareState::NotShared:
    default:
        return QCoreApplication::translate("UsbForwardingUi", "Not shared");
    }
}

inline bool canShare(const QVariantMap &device)
{
    return deviceShareState(device) == DeviceShareState::NotShared;
}

inline bool canStopSharing(const QVariantMap &device)
{
    const DeviceShareState state = deviceShareState(device);
    return state == DeviceShareState::Shared || state == DeviceShareState::SharedNotPlugged;
}

// Plain text for the environment check. Never contains a link or a command.
inline QString environmentStatusText(UsbForwardingEnvironment::State state, const QString &version)
{
    using E = UsbForwardingEnvironment;
    switch (state) {
    case E::Checking:
        return QCoreApplication::translate("UsbForwardingUi", "Checking for usbipd-win...");
    case E::NotInstalled:
        return QCoreApplication::translate("UsbForwardingUi",
            "usbipd-win was not found. Install usbipd-win yourself to share USB devices; MoonlightVibe does not install it.");
    case E::ServiceStopped:
        return QCoreApplication::translate("UsbForwardingUi",
            "usbipd-win is installed, but its service is not running. Start the usbipd service, then press Refresh.");
    case E::DriverStopped:
        return QCoreApplication::translate("UsbForwardingUi",
            "usbipd-win is installed, but its driver is not running. Restart Windows or start the driver as administrator, then press Refresh.");
    case E::CheckFailed:
        return QCoreApplication::translate("UsbForwardingUi",
            "Could not check usbipd-win. Make sure it is installed correctly, then press Refresh.");
    case E::Unsupported:
        return QCoreApplication::translate("UsbForwardingUi", "USB forwarding is not supported on this platform.");
    case E::Ready:
    default:
        return version.isEmpty()
            ? QCoreApplication::translate("UsbForwardingUi", "usbipd-win is installed and its service is running.")
            : QCoreApplication::translate("UsbForwardingUi", "usbipd-win %1 is installed and its service is running.").arg(version);
    }
}

// Whether Ctrl+Alt+Shift+U is active. Only on Windows with USB forwarding turned
// on; in every other case the key combo is not intercepted at all.
inline bool usbForwardingHotkeyEnabled(bool platformIsWindows, bool preferenceOn)
{
    return platformIsWindows && preferenceOn;
}

// The auto-forward list stores stable device identities ("vid:pid" or
// "vid:pid#serial"). Empty identities are never stored.
inline bool autoDevicesContain(const QStringList &list, const QString &identity)
{
    return !identity.isEmpty() && list.contains(identity);
}

// Returns the list with identity added (once) or removed (every copy). The
// input order is kept; other entries are never touched.
inline QStringList autoDevicesWith(const QStringList &list, const QString &identity, bool on)
{
    if (identity.isEmpty()) {
        return list;
    }
    QStringList result = list;
    if (on) {
        if (!result.contains(identity)) {
            result.append(identity);
        }
    } else {
        result.removeAll(identity);
    }
    return result;
}

} // namespace Ui
} // namespace UsbForwarding
