#include "usbforwardingsessionglue.h"

#include <QSslKey>

namespace UsbForwarding {

SessionPlan planSession(const SessionInputs &inputs)
{
    SessionPlan plan;
    if (!inputs.platformSupported) {
        plan.kind = PlanKind::NotApplicable;
        return plan;
    }
    if (!inputs.enabled) {
        plan.kind = PlanKind::Disabled;
        return plan;
    }

    plan.kind = PlanKind::Refused;

    /* Both addresses go through the same normaliser, so a legitimate stream is
     * never refused for a spelling difference (case, trailing dot, brackets). */
    const QString stream = ManagerConfig::canonicalHostAddress(inputs.streamAddress);
    const QString paired = ManagerConfig::canonicalHostAddress(inputs.pairedAddress);
    if (stream.isEmpty() || paired.isEmpty()) {
        plan.reason = QStringLiteral("the host address is missing or is not a host address");
        return plan;
    }
    if (stream != paired) {
        plan.reason = QStringLiteral("the stream address is not the paired host's address");
        return plan;
    }
    if (inputs.pinnedServerCertificate.isNull()) {
        plan.reason = QStringLiteral("the paired server certificate is missing");
        return plan;
    }
    if (inputs.clientSsl.localCertificate().isNull() || inputs.clientSsl.privateKey().isNull()) {
        plan.reason = QStringLiteral("the client certificate or key is missing");
        return plan;
    }

    plan.config.enabled = true;
    plan.config.autoDevices = inputs.autoDevices;
    plan.config.streamAddress = stream;
    plan.config.pairedAddress = paired;
    plan.config.pinnedServerCertificate = inputs.pinnedServerCertificate;
    plan.config.clientSsl = inputs.clientSsl;
    plan.kind = PlanKind::Start;
    return plan;
}

} // namespace UsbForwarding
