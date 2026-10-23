// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include "usbforwardingbackend.h"
#include "usbforwardingcapability.h"
#include "usbforwardingenvironment.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

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

UsbForwardingBackend::UsbForwardingBackend(QObject *parent) : QObject(parent) {}

UsbForwardingBackend *UsbForwardingBackend::get()
{
    static UsbForwardingBackend backend;
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
#ifdef Q_OS_WIN
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
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
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
    // "exe" is the validated absolute path; "arguments" came from
    // bindArguments()/unbindArguments(). Nothing else reaches the UAC prompt.
    const std::wstring wideExe = QDir::toNativeSeparators(exe).toStdWString();
    const std::wstring wideArgs = arguments.toStdWString();
    const std::wstring wideDir = QDir::toNativeSeparators(QFileInfo(exe).absolutePath()).toStdWString();
    const HINSTANCE result = ShellExecuteW(nullptr, L"runas", wideExe.c_str(), wideArgs.c_str(),
                                           wideDir.c_str(), SW_HIDE);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
    if (ok) {
        emit operationFinished(true, successMessage);
        QTimer::singleShot(1500, this, [this] { refresh(); });
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
#else
    Q_UNUSED(arguments);
    Q_UNUSED(successMessage);
    emit operationFinished(false, tr("USB forwarding is not supported on this platform."));
#endif
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
    runElevated(arguments, tr("Requested sharing. Refreshing device list..."));
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
    runElevated(arguments, tr("Requested stop sharing. Refreshing device list..."));
}
