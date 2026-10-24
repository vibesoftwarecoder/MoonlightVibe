#pragma once

#include "nvaddress.h"
#include "usbforwardingmanager.h"

#include <QSslCertificate>

namespace UsbForwarding {

/*
 * The real seams for a streaming session. Everything is copied in, so the
 * callbacks own what they use and never touch the NvComputer after this
 * returns. Both callbacks run off the stream thread and may block briefly.
 *
 *   fetchCapability  GET /api/v1/usb-forwarding on the paired host, pinned to
 *                    the paired certificate (throws on failure)
 *   listDevices      devices usbipd is sharing right now (empty on any error);
 *                    runs `usbipd state` itself and never uses the UI backend
 */
ManagerSeams makeSessionSeams(const NvAddress &address, uint16_t httpsPort,
                              const QSslCertificate &pinnedServerCertificate, bool useTrueUid);

} // namespace UsbForwarding
