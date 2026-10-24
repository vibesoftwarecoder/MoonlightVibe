// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include "usbforwardingbackend.h"
#include "usbforwardingcapability.h"
#include "usbforwardingenvironment.h"
#include "usbforwardinguilogic.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <mutex>
#include <thread>

namespace {

// Elevation can sit behind a UAC prompt for a long time; the wait is on a
// worker thread, so a generous bound costs nothing.
constexpr int kElevationTimeoutMs = 2 * 60 * 1000;

constexpr int kElevationHardLimitMs = 10 * 60 * 1000;

#ifdef USBFWD_TESTING
std::mutex s_HooksMutex;
UsbForwardingBackend::TestHooks s_Hooks;

UsbForwardingBackend::TestHooks currentHooks()
{
    std::lock_guard<std::mutex> lock(s_HooksMutex);
    return s_Hooks;
}
#endif

QString locateUsbipdOrHook()
{
#ifdef USBFWD_TESTING
    const auto hooks = currentHooks();
    if (hooks.locateUsbipd) {
        return hooks.locateUsbipd();
    }
#endif
    return UsbForwardingEnvironment::locateUsbipd();
}

// Parses "USB\VID_054C&PID_0CE6\..." into "054c:0ce6".
QString vidPidFromInstanceId(const QString &instanceId)
{
    static const QRegularExpression vidRe(QStringLiteral("VID_([0-9A-Fa-f]{4})"));
    static const QRegularExpression pidRe(QStringLiteral("PID_([0-9A-Fa-f]{4})"));
    const QRegularExpressionMatch vidMatch = vidRe.match(instanceId);
    const QRegularExpressionMatch pidMatch = pidRe.match(instanceId);
    if (!vidMatch.hasMatch() || !pidMatch.hasMatch()) {
        return QString();
    }
    return vidMatch.captured(1).toLower() + QLatin1Char(':') + pidMatch.captured(1).toLower();
}
} // namespace

#ifdef USBFWD_TESTING
void UsbForwardingBackend::setTestHooks(TestHooks hooks)
{
    std::lock_guard<std::mutex> lock(s_HooksMutex);
    s_Hooks = std::move(hooks);
}
#endif

struct UsbForwardingBackend::DeliveryLink
{
    std::mutex mutex;
    UsbForwardingBackend *backend = nullptr;
};

// Identity of a device across ports and reboots. A device with a real serial
// number gets "vid:pid#serial". A device WITHOUT one gets plain "vid:pid", so
// serial-less twins of the same model are indistinguishable and share it.
QString UsbForwardingBackend::identityFromInstanceId(const QString &instanceId)
{
    const QString vidPid = vidPidFromInstanceId(instanceId);
    if (vidPid.isEmpty()) {
        return QString();
    }
    // "USB\VID_xxxx&PID_yyyy\<third>": a real serial number has no '&'.
    // Windows invents ids such as "5&1a2b&0&2" for devices without a serial;
    // those depend on the port, so they are not an identity.
    const QStringList parts = instanceId.split(QLatin1Char('\\'));
    if (parts.size() >= 3) {
        static const QRegularExpression serialRe(QStringLiteral("\\A[0-9A-Za-z._-]{1,64}\\z"));
        const QString serial = parts.at(2);
        if (serialRe.match(serial).hasMatch()) {
            return vidPid + QLatin1Char('#') + serial.toLower();
        }
    }
    return vidPid;
}

UsbForwardingBackend::UsbForwardingBackend(QObject *parent)
    : QObject(parent), m_Link(std::make_shared<DeliveryLink>())
{
    m_Link->backend = this;
}

UsbForwardingBackend::~UsbForwardingBackend()
{
    shutdown();
    std::lock_guard<std::mutex> lock(m_Link->mutex);
    m_Link->backend = nullptr;
}

void UsbForwardingBackend::shutdown()
{
    const QList<QProcess *> probes = findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly);
    for (QProcess *probe : probes) {
        probe->disconnect();
        if (probe->state() != QProcess::NotRunning) {
            probe->kill();
            probe->waitForFinished(1000);
        }
        delete probe;
    }
    if (!probes.isEmpty()) {
        m_Busy = false;
    }
}

UsbForwardingBackend *UsbForwardingBackend::get()
{
    // A function-local static is destroyed after the application object is gone,
    // so a probe still running at exit is stopped from aboutToQuit instead.
    static UsbForwardingBackend backend;
    static const bool wired = [] {
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QObject::connect(app, &QCoreApplication::aboutToQuit, &backend, [] { backend.shutdown(); });
        }
        return true;
    }();
    Q_UNUSED(wired)
    return &backend;
}

bool UsbForwardingBackend::isShareableBusId(const QString &busId)
{
    static const QRegularExpression realRe(QStringLiteral("\\A[1-9][0-9]*-[1-9][0-9]*\\z"));
    // Check the shared charset and length rule first, then the stricter shape.
    return UsbForwarding::isValidBusId(busId.toUtf8()) && realRe.match(busId).hasMatch();
}

bool UsbForwardingBackend::isValidPersistedGuid(const QString &guid)
{
    static const QRegularExpression guidRe(QStringLiteral(
        "\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
    return guidRe.match(guid).hasMatch();
}

QString UsbForwardingBackend::bindArguments(const QString &busId)
{
    if (!isShareableBusId(busId)) {
        return QString();
    }
    return QStringLiteral("bind --busid ") + busId;
}

QString UsbForwardingBackend::unbindArguments(const QString &busId, const QString &persistedGuid)
{
    if (isShareableBusId(busId)) {
        return QStringLiteral("unbind --busid ") + busId;
    }
    if (isValidPersistedGuid(persistedGuid)) {
        return QStringLiteral("unbind --guid ") + persistedGuid;
    }
    return QString();
}

QVariantList UsbForwardingBackend::parseUsbipdState(const QByteArray &stateJson, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(stateJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
        !doc.object().value(QLatin1String("Devices")).isArray()) {
        if (error != nullptr) {
            *error = tr("Could not parse usbipd state output.");
        }
        return {};
    }

    QVariantList devices;
    const QJsonArray array = doc.object().value(QLatin1String("Devices")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject o = value.toObject();
        const QString busId = o.value(QLatin1String("BusId")).toString();
        const QString clientIp = o.value(QLatin1String("ClientIPAddress")).toString();
        const QString description = o.value(QLatin1String("Description")).toString();
        const QString instanceId = o.value(QLatin1String("InstanceId")).toString();
        const bool isForced = o.value(QLatin1String("IsForced")).toBool();
        const QString persistedGuid = o.value(QLatin1String("PersistedGuid")).toString();

        const bool isBound = !persistedGuid.isEmpty();
        const bool isConnected = !busId.isEmpty();
        const bool isAttached = !clientIp.isEmpty();
        const bool supported = isConnected && isShareableBusId(busId);

        QVariantMap device;
        device.insert(QStringLiteral("busId"), busId);
        device.insert(QStringLiteral("description"), description);
        device.insert(QStringLiteral("instanceId"), instanceId);
        device.insert(QStringLiteral("vidPid"), vidPidFromInstanceId(instanceId));
        device.insert(QStringLiteral("identity"), identityFromInstanceId(instanceId));
        device.insert(QStringLiteral("isBound"), isBound);
        device.insert(QStringLiteral("isConnected"), isConnected);
        device.insert(QStringLiteral("isAttached"), isAttached);
        device.insert(QStringLiteral("isSupported"), supported);
        device.insert(QStringLiteral("isForced"), isForced);
        device.insert(QStringLiteral("persistedGuid"), persistedGuid);
        devices.append(device);
    }
    return devices;
}

QString UsbForwardingBackend::shareStateText(const QVariantMap &device) const
{
    return UsbForwarding::Ui::deviceShareStateText(UsbForwarding::Ui::deviceShareState(device));
}

bool UsbForwardingBackend::canShare(const QVariantMap &device) const
{
    return UsbForwarding::Ui::canShare(device);
}

bool UsbForwardingBackend::canStopSharing(const QVariantMap &device) const
{
    return UsbForwarding::Ui::canStopSharing(device);
}

bool UsbForwardingBackend::autoDevicesContain(const QStringList &list, const QString &identity) const
{
    return UsbForwarding::Ui::autoDevicesContain(list, identity);
}

QStringList UsbForwardingBackend::autoDevicesWith(const QStringList &list, const QString &identity, bool on) const
{
    return UsbForwarding::Ui::autoDevicesWith(list, identity, on);
}

void UsbForwardingBackend::setBusy(bool busy)
{
    if (m_Busy == busy) {
        return;
    }
    m_Busy = busy;
    emit busyChanged();
}

void UsbForwardingBackend::setError(const QString &error)
{
    if (m_Error == error) {
        return;
    }
    m_Error = error;
    emit errorChanged();
}

void UsbForwardingBackend::refresh()
{
    if (m_Busy) {
        return;
    }
#ifdef USBFWD_TESTING
    {
        const auto hooks = currentHooks();
        if (hooks.refresh) {
            hooks.refresh();
            return;
        }
    }
#endif
#ifdef Q_OS_WIN
    const QString exe = locateUsbipdOrHook();
    if (exe.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("usbipd-win is not installed."));
        return;
    }
    setBusy(true);
    setError(QString());

    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; without this the
     * busy flag would stick and the device list would go stale. */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                probe->deleteLater();
                setBusy(false);
                m_Devices.clear();
                emit devicesChanged();
                setError(tr("usbipd could not be started. Requires usbipd-win 2.2.0+."));
            });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        setBusy(false);

        if (exitCode != 0) {
            const QString detail =
                QString::fromLocal8Bit(probe->readAllStandardError()).simplified();
            m_Devices.clear();
            emit devicesChanged();
            setError(
                detail.isEmpty()
                    ? tr("usbipd state failed (exit %1). Requires usbipd-win 2.2.0+.").arg(exitCode)
                    : detail);
            return;
        }

        QString parseError;
        m_Devices = parseUsbipdState(probe->readAllStandardOutput(), &parseError);
        emit devicesChanged();
        if (!parseError.isEmpty()) {
            setError(parseError);
        }
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->setWorkingDirectory(QFileInfo(exe).absolutePath());
    probe->start(exe, { QStringLiteral("state") });
#else
    m_Devices.clear();
    emit devicesChanged();
    setError(tr("USB forwarding is not supported on this platform."));
#endif
}

void UsbForwardingBackend::runElevated(const QString &arguments, const QString &successMessage)
{
#ifdef Q_OS_WIN
    if (m_Busy) {
        emit operationFinished(false, tr("Another USB operation is still running."));
        return;
    }
    const QString exe = locateUsbipdOrHook();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
    // "exe" is the validated absolute path; "arguments" came from
    // bindArguments()/unbindArguments(). Nothing else reaches the UAC prompt.
    const QString workDir = QFileInfo(exe).absolutePath();
    UsbForwarding::ElevatedLauncher launcher = UsbForwarding::launchElevated;
    int timeoutMs = kElevationTimeoutMs;
    int hardLimitMs = kElevationHardLimitMs;
#ifdef USBFWD_TESTING
    {
        const auto hooks = currentHooks();
        if (hooks.launcher) launcher = hooks.launcher;
        if (hooks.elevationTimeoutMs > 0) timeoutMs = hooks.elevationTimeoutMs;
        if (hooks.elevationHardLimitMs > 0) hardLimitMs = hooks.elevationHardLimitMs;
    }
#endif

    setBusy(true);
    m_SlowReported = false;
    // A plain detached thread, not a thread pool: the application must be able
    // to quit while a UAC prompt is still open, and a pool would make exit wait
    // for it. Results come back through the link, which the destructor clears
    // under the same lock, so delivery can never touch a deleted backend.
    const std::shared_ptr<DeliveryLink> link = m_Link;
    const auto deliver = [link](std::function<void(UsbForwardingBackend *)> call) {
        std::lock_guard<std::mutex> lock(link->mutex);
        if (UsbForwardingBackend *backend = link->backend) {
            QMetaObject::invokeMethod(backend, [backend, call] { call(backend); }, Qt::QueuedConnection);
        }
    };
    std::thread([deliver, launcher, exe, arguments, workDir, timeoutMs, hardLimitMs, successMessage] {
        // Blocks this thread for as long as the UAC prompt and usbipd take.
        const UsbForwarding::ElevationResult result = launcher(
            exe, arguments, workDir, timeoutMs, hardLimitMs,
            [deliver] { deliver([](UsbForwardingBackend *b) { b->reportSlow(); }); });
        deliver([result, successMessage](UsbForwardingBackend *b) { b->finishElevated(result, successMessage); });
    }).detach();
#else
    Q_UNUSED(arguments);
    Q_UNUSED(successMessage);
    emit operationFinished(false, tr("USB forwarding is not supported on this platform."));
#endif
}

void UsbForwardingBackend::reportSlow()
{
    // The process may still change usbipd's state: stay busy (no second
    // elevated operation can start) until it really exits.
    m_SlowReported = true;
    emit operationFinished(false, tr("usbipd is still running; the device list will update when it finishes."));
}

void UsbForwardingBackend::finishElevated(const UsbForwarding::ElevationResult &result,
                                          const QString &successMessage)
{
    const bool slowReported = m_SlowReported;
    m_SlowReported = false;
    setBusy(false);
    if (result.timedOut) {
        if (!slowReported) {
            emit operationFinished(false, tr("usbipd is still running; the device list will update when it finishes."));
        }
        // It exited late, or we gave up waiting: either way look at the real state now.
        refresh();
    } else if (result.succeeded()) {
        emit operationFinished(true, successMessage);
        // The list reflects what usbipd now says, not a guess at how long it takes.
        refresh();
    } else if (result.launched) {
        emit operationFinished(false, tr("usbipd failed (exit %1).").arg(result.exitCode));
    } else if (result.cancelled) {
        emit operationFinished(false, tr("The elevation was cancelled."));
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
}

void UsbForwardingBackend::bind(const QString &busId)
{
    if (busId.isEmpty()) {
        return;
    }
    const QString arguments = bindArguments(busId);
    if (arguments.isEmpty()) {
        emit operationFinished(false, tr("This USB device cannot be shared."));
        return;
    }
    runElevated(arguments, tr("The device is now shared."));
}

void UsbForwardingBackend::unbind(const QString &busId, const QString &persistedGuid)
{
    if (busId.isEmpty() && persistedGuid.isEmpty()) {
        return;
    }
    const QString arguments = unbindArguments(busId, persistedGuid);
    if (arguments.isEmpty()) {
        emit operationFinished(false, tr("This USB device cannot be unshared."));
        return;
    }
    runElevated(arguments, tr("The device is no longer shared."));
}
