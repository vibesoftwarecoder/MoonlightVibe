#pragma once

#include "usbforwardingmanager.h"

#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>

/*
 * Decides, from plain values, whether a streaming session should start USB
 * forwarding and with which configuration. It knows nothing about Session,
 * NvComputer or the preference object, so it can be unit tested alone.
 *
 * Nothing here opens a socket, starts a thread or writes to a log. The caller
 * logs SessionPlan::reason (a short developer-facing line that never holds the
 * certificate, the key or an address).
 */
namespace UsbForwarding {

struct SessionInputs
{
    /* True only on platforms that support the feature (Windows). */
    bool platformSupported = false;
    /* StreamingPreferences::usbForwardingEnabled / usbForwardingAutoDevices. */
    bool enabled = false;
    QStringList autoDevices;
    /* The address the stream is connected to, and the address of the paired
     * computer record. Spelling differences are normalised here. */
    QString streamAddress;
    QString pairedAddress;
    /* NvComputer::serverCert (the pinned certificate). */
    QSslCertificate pinnedServerCertificate;
    /* IdentityManager::getSslConfig(): the client certificate and key. */
    QSslConfiguration clientSsl;
};

enum class PlanKind
{
    NotApplicable, // wrong platform: do nothing, touch nothing
    Disabled,      // switched off in the preferences (the default): do nothing
    Refused,       // switched on, but the inputs are not safe to use; see reason
    Start,         // config is ready for Manager::start()
};

struct SessionPlan
{
    PlanKind kind = PlanKind::NotApplicable;
    ManagerConfig config;
    QString reason;
};

SessionPlan planSession(const SessionInputs &inputs);

} // namespace UsbForwarding
