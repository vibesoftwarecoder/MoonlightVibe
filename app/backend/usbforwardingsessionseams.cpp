#include "usbforwardingsessionseams.h"

#include "nvhttp.h"
#include "usbforwardingbackend.h"
#include "usbforwardingenvironment.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <atomic>
#include <memory>

namespace UsbForwarding {

#ifdef Q_OS_WIN
/* Runs `usbipd state` on the calling (pool) thread. It gives up as soon as
 * `abandon` is set, killing the child, so a stop never waits for it. */
static QList<SharedDevice> listSharedDevicesBlocking(const std::atomic<bool> &abandon)
{
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
    if (exe.isEmpty() || abandon.load()) {
        return {};
    }
    QProcess process;
    process.setWorkingDirectory(QFileInfo(exe).absolutePath());
    process.start(exe, { QStringLiteral("state") });

    /* Poll in short steps so an abandon is noticed within about 100 ms. */
    QElapsedTimer clock;
    clock.start();
    bool started = false;
    while (!abandon.load() && clock.elapsed() < 5000) {
        if (process.waitForStarted(100)) {
            started = true;
            break;
        }
        if (process.error() == QProcess::FailedToStart) {
            return {};
        }
    }
    bool finished = false;
    if (started) {
        clock.restart();
        while (!abandon.load() && clock.elapsed() < 8000) {
            if (process.waitForFinished(100)) {
                finished = true;
                break;
            }
        }
    }
    if (!finished) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(500);
        }
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {};
    }
    QString error;
    const QVariantList devices = UsbForwardingBackend::parseUsbipdState(process.readAllStandardOutput(), &error);
    if (!error.isEmpty()) {
        return {};
    }
    return sharedDevicesFromBackendList(devices);
}
#endif

ManagerSeams makeSessionSeams(const NvAddress &address, uint16_t httpsPort,
                              const QSslCertificate &pinnedServerCertificate, bool useTrueUid)
{
    ManagerSeams seams;
    seams.fetchCapability = [address, httpsPort, pinnedServerCertificate, useTrueUid]() {
        NvHTTP http(address, httpsPort, pinnedServerCertificate, useTrueUid);
        return http.getUsbForwardingCapability();
    };
#ifdef Q_OS_WIN
    /* One flag per session's seams: set by Manager::stop(), never reset. */
    const auto abandon = std::make_shared<std::atomic<bool>>(false);
    seams.listDevices = [abandon]() { return listSharedDevicesBlocking(*abandon); };
    seams.abandonListing = [abandon]() { abandon->store(true); };
#else
    seams.listDevices = []() { return QList<SharedDevice>(); };
#endif
    return seams;
}

} // namespace UsbForwarding
