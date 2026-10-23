// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include "usbforwardingenvironment.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <string>
#endif

namespace {

#ifdef Q_OS_WIN

// "C:\..." or "C:/...". Rejects UNC paths, \\?\ paths, relative paths and
// drive-relative paths such as "C:usbipd.exe".
bool isDriveLetterAbsolute(const QString &path)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z]:[\\\\/]"));
    return re.match(path).hasMatch();
}

bool hasParentReference(const QString &path)
{
    const QStringList parts = path.split(QRegularExpression(QStringLiteral("[\\\\/]")));
    return parts.contains(QStringLiteral(".."));
}

// Rights that let a principal change a file or what is in a directory. The
// directory-only bits share values with the file ones (FILE_ADD_FILE is
// FILE_WRITE_DATA, FILE_ADD_SUBDIRECTORY is FILE_APPEND_DATA).
constexpr DWORD kFileModifyMask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                  FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
constexpr DWORD kDirectoryModifyMask = kFileModifyMask | FILE_DELETE_CHILD;
// The drive root: every user may add new folders there by default, which does
// not let them replace an existing one. Only rights that can swap or
// re-permission existing children matter.
constexpr DWORD kRootModifyMask = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

class HandleCloser
{
public:
    explicit HandleCloser(HANDLE handle = nullptr) : m_Handle(handle) {}
    ~HandleCloser() { if (m_Handle) CloseHandle(m_Handle); }
    HandleCloser(const HandleCloser &) = delete;
    HandleCloser &operator=(const HandleCloser &) = delete;
    HANDLE get() const { return m_Handle; }
    void reset(HANDLE handle) { if (m_Handle) CloseHandle(m_Handle); m_Handle = handle; }
private:
    HANDLE m_Handle;
};

// The process's own token as a standard user would hold it, duplicated as an
// impersonation token for AccessCheck. If the process is elevated, the
// limited (linked) token is used instead, so Program Files is judged the way
// an ordinary user sees it. Returns nullptr on any failure (callers fail closed).
HANDLE openStandardUserToken()
{
    HANDLE process = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process)) {
        return nullptr;
    }
    HandleCloser processCloser(process);
    HANDLE source = process;
    HandleCloser linkedCloser;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD returned = 0;
    if (GetTokenInformation(process, TokenElevationType, &type, sizeof(type), &returned) &&
        type == TokenElevationTypeFull) {
        TOKEN_LINKED_TOKEN linked {};
        if (!GetTokenInformation(process, TokenLinkedToken, &linked, sizeof(linked), &returned) ||
            linked.LinkedToken == nullptr) {
            return nullptr;
        }
        linkedCloser.reset(linked.LinkedToken);
        source = linked.LinkedToken;
    }
    HANDLE impersonation = nullptr;
    if (DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr,
                         SecurityImpersonation, TokenImpersonation, &impersonation)) {
        return impersonation;
    }
    // A linked token can already be impersonation-class and refuse a duplicate;
    // use a copy of the handle then.
    if (source != process &&
        DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &impersonation, 0, FALSE,
                        DUPLICATE_SAME_ACCESS)) {
        return impersonation;
    }
    return nullptr;
}

// Owner rights are implicit (WRITE_DAC), so an owner SID that this token
// carries as user or as an enabled group counts as full control.
bool ownerIsInToken(HANDLE token, PSID owner)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return true; // cannot tell: fail closed
    }
    QByteArray userBuffer(static_cast<qsizetype>(size), 0);
    if (!GetTokenInformation(token, TokenUser, userBuffer.data(), size, &size)) {
        return true;
    }
    if (EqualSid(reinterpret_cast<TOKEN_USER *>(userBuffer.data())->User.Sid, owner)) {
        return true;
    }
    size = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    if (size == 0) {
        return true;
    }
    QByteArray groupBuffer(static_cast<qsizetype>(size), 0);
    if (!GetTokenInformation(token, TokenGroups, groupBuffer.data(), size, &size)) {
        return true;
    }
    const auto *groups = reinterpret_cast<TOKEN_GROUPS *>(groupBuffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        const DWORD attributes = groups->Groups[i].Attributes;
        if ((attributes & SE_GROUP_ENABLED) && !(attributes & SE_GROUP_USE_FOR_DENY_ONLY) &&
            EqualSid(groups->Groups[i].Sid, owner)) {
            return true;
        }
    }
    return false;
}

// True if the token can exercise any right in mask on one object.
// Fails closed: anything unreadable or unexpected counts as writable.
bool tokenCanModify(HANDLE token, const QString &path, DWORD mask)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    const DWORD status = GetNamedSecurityInfoW(
        wide.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || descriptor == nullptr) {
        return true;
    }
    bool modifiable = true; // until proven otherwise
    if (dacl != nullptr) { // a NULL DACL grants everyone full access
        PSID owner = nullptr;
        BOOL defaulted = FALSE;
        GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                                    FILE_ALL_ACCESS };
        PRIVILEGE_SET privileges {};
        DWORD privilegesLength = sizeof(privileges);
        DWORD granted = 0;
        BOOL allowed = FALSE;
        if (GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) && owner != nullptr &&
            AccessCheck(descriptor, token, MAXIMUM_ALLOWED, &mapping, &privileges,
                        &privilegesLength, &granted, &allowed)) {
            const DWORD effective = allowed ? granted : 0;
            modifiable = (effective & mask) != 0 || ownerIsInToken(token, owner);
        }
    }
    LocalFree(descriptor);
    return modifiable;
}

// The path the file system really resolves to, with every junction, symbolic
// link, subst drive and 8.3 alias followed. Empty on failure. Qt's
// canonicalFilePath() does not follow junctions, so it cannot be used here.
QString finalPathOf(const QString &path)
{
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return QString();
    }
    HandleCloser closer(handle);
    std::wstring buffer(1024, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        return QString();
    }
    QString result = QString::fromWCharArray(buffer.c_str(), static_cast<qsizetype>(length));
    if (result.startsWith(QLatin1String("\\\\?\\UNC\\"))) {
        return QString(); // network path
    }
    if (result.startsWith(QLatin1String("\\\\?\\"))) {
        result.remove(0, 4);
    }
    return QDir::fromNativeSeparators(result);
}

QString queryUsbipdServiceBinaryPath()
{
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return QString();
    }
    QString result;
    const SC_HANDLE service = OpenServiceW(manager, L"usbipd", SERVICE_QUERY_CONFIG);
    if (service) {
        DWORD needed = 0;
        QueryServiceConfigW(service, nullptr, 0, &needed);
        if (needed > 0 && needed <= 64 * 1024) {
            QByteArray buffer(static_cast<qsizetype>(needed), 0);
            auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
            if (QueryServiceConfigW(service, config, needed, &needed) &&
                config->lpBinaryPathName != nullptr) {
                result = QString::fromWCharArray(config->lpBinaryPathName);
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return result;
}

// Program Files from the registry (admin-only writable), never from the
// process environment, which any user can poison for a child launch.
QStringList programFilesDirectories()
{
    QStringList dirs;
    QSettings settings(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"),
                       QSettings::NativeFormat);
    for (const QString &key : { QStringLiteral("ProgramW6432Dir"), QStringLiteral("ProgramFilesDir") }) {
        const QString dir = settings.value(key).toString();
        if (!dir.isEmpty() && !dirs.contains(dir, Qt::CaseInsensitive)) {
            dirs.append(dir);
        }
    }
    return dirs;
}

#endif // Q_OS_WIN

} // namespace

UsbForwardingEnvironment::UsbForwardingEnvironment(QObject *parent)
    : QObject(parent)
{
}

UsbForwardingEnvironment* UsbForwardingEnvironment::get()
{
    static UsbForwardingEnvironment environment;
    return &environment;
}

QString UsbForwardingEnvironment::parseServiceBinaryPath(const QString &imagePath)
{
#ifdef Q_OS_WIN
    const QString text = imagePath.trimmed();
    QString path;
    if (text.startsWith(QLatin1Char('"'))) {
        const qsizetype end = text.indexOf(QLatin1Char('"'), 1);
        if (end < 2) {
            return QString();
        }
        path = text.mid(1, end - 1);
    } else {
        // Unquoted paths may contain spaces: the program ends at the first ".exe".
        const qsizetype idx = text.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
        if (idx < 0) {
            return QString();
        }
        const qsizetype end = idx + 4;
        if (end < text.size() && !text.at(end).isSpace()) {
            return QString();
        }
        path = text.left(end);
    }
    if (path.contains(QLatin1Char('%')) || !isDriveLetterAbsolute(path)) {
        return QString();
    }
    return QDir::fromNativeSeparators(path);
#else
    Q_UNUSED(imagePath);
    return QString();
#endif
}

bool UsbForwardingEnvironment::isWritableByStandardUsers(const QString &path)
{
#ifdef Q_OS_WIN
    const QFileInfo info(path);
    if (!info.exists()) {
        return true;
    }
    HandleCloser token(openStandardUserToken());
    if (token.get() == nullptr) {
        return true;
    }
    // The object itself, then every directory above it, including the drive root.
    QString current = QDir::cleanPath(info.absoluteFilePath());
    bool first = true;
    while (true) {
        const QFileInfo currentInfo(current);
        const bool isRoot = currentInfo.isRoot();
        const DWORD mask = isRoot ? kRootModifyMask
                                  : ((first && !currentInfo.isDir()) ? kFileModifyMask
                                                                     : kDirectoryModifyMask);
        if (tokenCanModify(token.get(), current, mask)) {
            return true;
        }
        if (isRoot) {
            return false;
        }
        const QString parent = currentInfo.dir().absolutePath();
        if (parent == current) {
            return false;
        }
        current = parent;
        first = false;
    }
#else
    Q_UNUSED(path);
    return true;
#endif
}

bool UsbForwardingEnvironment::isTrustedUsbipdPath(const QString &path, QString *reason)
{
    return isTrustedExecutablePath(path, QStringLiteral("usbipd.exe"), reason);
}

bool UsbForwardingEnvironment::isTrustedExecutablePath(const QString &path, const QString &expectedFileName,
                                                       QString *reason)
{
    const auto reject = [reason](const char *why) {
        if (reason != nullptr) {
            *reason = QString::fromLatin1(why);
        }
        return false;
    };
#ifdef Q_OS_WIN
    if (path.isEmpty() || path.contains(QLatin1Char('%')) || path.contains(QLatin1Char('"')) ||
        path.contains(QChar(0))) {
        return reject("empty or contains unsafe characters");
    }
    if (!isDriveLetterAbsolute(path)) {
        return reject("not an absolute drive-letter path");
    }
    if (hasParentReference(path)) {
        return reject("contains a parent directory reference");
    }
    const QString cleaned = QDir::cleanPath(path);
    const QFileInfo info(cleaned);
    if (info.fileName().compare(expectedFileName, Qt::CaseInsensitive) != 0) {
        return reject("file name is not the expected one");
    }
    if (!info.exists() || !info.isFile()) {
        return reject("file does not exist");
    }
    if (finalPathOf(cleaned).compare(cleaned, Qt::CaseInsensitive) != 0) {
        return reject("path goes through a link or alias");
    }
    if (isWritableByStandardUsers(cleaned)) {
        return reject("location is writable by standard users");
    }
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(expectedFileName);
    return reject("unsupported platform");
#endif
}

QStringList UsbForwardingEnvironment::candidateUsbipdPaths()
{
    // Never QStandardPaths::findExecutable(), PATH, the current directory or
    // environment variables: the result may be launched elevated.
    QStringList candidates;
#ifdef Q_OS_WIN
    const QString servicePath = parseServiceBinaryPath(queryUsbipdServiceBinaryPath());
    if (!servicePath.isEmpty()) {
        candidates.append(servicePath);
    }
    for (const QString &dir : programFilesDirectories()) {
        candidates.append(QDir::fromNativeSeparators(dir) + QStringLiteral("/usbipd-win/usbipd.exe"));
    }
#endif
    return candidates;
}

QString UsbForwardingEnvironment::firstAccepted(const QStringList &candidates,
                                                const std::function<bool(const QString &)> &accept)
{
    for (const QString &candidate : candidates) {
        if (accept(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    return QString();
}

QString UsbForwardingEnvironment::locateUsbipd()
{
    return firstAccepted(candidateUsbipdPaths(),
                         [](const QString &path) { return isTrustedUsbipdPath(path); });
}

void UsbForwardingEnvironment::refresh()
{
    if (m_Checking) {
        return;
    }
#ifdef Q_OS_WIN
    const QString usbipdExe = locateUsbipd();
    if (usbipdExe.isEmpty()) {
        m_Version.clear();
        finish(NotInstalled);
        return;
    }
    probeWith(usbipdExe, { QStringLiteral("--version") }, 8000);
#else
    m_Version.clear();
    finish(Unsupported);
#endif
}

void UsbForwardingEnvironment::probeWith(const QString &program, const QStringList &arguments,
                                         int timeoutMs)
{
    if (m_Checking) {
        return;
    }
    m_Checking = true;
    emit checkingChanged();
    m_State = Checking;
    emit stateChanged();

    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; handle it so a
     * missing executable cannot wedge the probe. */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        probe->deleteLater();
        finish(ServiceStopped);
    });
    connect(probe, &QProcess::finished, this,
            [this, probe](int exitCode, QProcess::ExitStatus exitStatus) {
        probe->deleteLater();
        // A probe we had to kill, or one that crashed, tells us nothing about
        // the service: report that the check failed, not that it is stopped.
        if (probe->property("timedOut").toBool() || exitStatus != QProcess::NormalExit) {
            finish(CheckFailed);
            return;
        }
        if (exitCode != 0) {
            finish(ServiceStopped);
            return;
        }
        const QString output =
            QString::fromLocal8Bit(probe->readAllStandardOutput());
        const QString firstLine = output.section(QLatin1Char('\n'), 0, 0).simplified();
        // "usbipd-win 4.2.0" -> "4.2.0"
        QString version = firstLine;
        if (version.startsWith(QLatin1String("usbipd-win"), Qt::CaseInsensitive)) {
            version.remove(0, 10);
        }
        version = version.trimmed();
        m_Version = version;
        startServiceProbe();
    });
    QTimer::singleShot(timeoutMs, probe, [probe] {
        if (probe->state() != QProcess::NotRunning) {
            probe->setProperty("timedOut", true);
            probe->kill();
        }
    });
    // Run from the program's own directory, never from the caller's cwd.
    probe->setWorkingDirectory(QFileInfo(program).absolutePath());
    probe->start(program, arguments);
}

void UsbForwardingEnvironment::startServiceProbe()
{
    finish(probeServices());
}

UsbForwardingEnvironment::State UsbForwardingEnvironment::probeServices()
{
#ifdef Q_OS_WIN
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return CheckFailed;
    State result = Ready;
    const struct { const wchar_t* name; State stopped; } services[] = {
        {L"usbipd", ServiceStopped}, {L"VBoxUSBMon", DriverStopped}
    };
    for (const auto& entry : services) {
        const SC_HANDLE service = OpenServiceW(manager, entry.name, SERVICE_QUERY_STATUS);
        if (!service) {
            result = CheckFailed;
            break;
        }
        SERVICE_STATUS status {};
        const bool queried = QueryServiceStatus(service, &status) != FALSE;
        CloseServiceHandle(service);
        if (!queried || status.dwCurrentState != SERVICE_RUNNING) {
            result = queried ? entry.stopped : CheckFailed;
            break;
        }
    }
    CloseServiceHandle(manager);
    return result;
#else
    return Unsupported;
#endif
}

QString UsbForwardingEnvironment::readinessError(State state)
{
    switch (state) {
    case Ready:
        return {};
    case Unsupported:
        return tr("USB forwarding is not supported on this platform.");
    case DriverStopped:
        return tr("The USB forwarding driver is not running. Start VBoxUSBMon as administrator, or restart Windows.");
    case ServiceStopped:
        return tr("The usbipd service is not running. Start the service and retry.");
    case Checking:
    case NotInstalled:
    case CheckFailed:
    default:
        return tr("Could not verify the local USB service and driver. Check the usbipd-win installation.");
    }
}

void UsbForwardingEnvironment::finish(State state)
{
    m_State = state;
    m_Checking = false;
    emit stateChanged();
    emit checkingChanged();
}
