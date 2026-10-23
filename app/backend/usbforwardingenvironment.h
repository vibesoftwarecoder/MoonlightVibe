#pragma once

// Derived from qiin2333/moonlight-qt (GPL-3.0).

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

// Read-only health check of the local USB sharing stack (usbipd-win and its
// driver). Windows only; on other platforms every query reports Unsupported.
//
// SECURITY: this class is where the client decides which usbipd.exe it will
// run, sometimes elevated. It never searches PATH or the current directory.
// The executable must come from the usbipd Windows service's registered
// binary path or from %ProgramFiles%\usbipd-win, and must pass
// isTrustedUsbipdPath() (absolute, named usbipd.exe, not reachable through
// a link, and not writable by standard users anywhere along its path).
class UsbForwardingEnvironment : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString usbipdVersion READ usbipdVersion NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    enum State
    {
        Checking,
        NotInstalled,
        ServiceStopped,
        Ready,
        DriverStopped,
        CheckFailed,
        Unsupported,
    };
    Q_ENUM(State)

    static UsbForwardingEnvironment* get();

    // Absolute, validated path of usbipd.exe, or an empty string.
    static QString locateUsbipd();
    // The only places usbipd.exe is ever looked for (service binary path,
    // then Program Files from the registry). Exposed so tests can prove PATH,
    // the working directory and environment variables are never candidates.
    static QStringList candidateUsbipdPaths();
    // First candidate the predicate accepts. locateUsbipd() uses
    // isTrustedUsbipdPath as the predicate; tests pass an accept-all one.
    static QString firstAccepted(const QStringList &candidates,
                                 const std::function<bool(const QString &)> &accept);
    // Pulls the executable out of a service ImagePath such as
    // "C:\Program Files\usbipd-win\usbipd.exe" server. Returns an empty
    // string for anything that is not a plain absolute path (relative paths,
    // %VARIABLES%, empty input). Pure string work, no file access.
    static QString parseServiceBinaryPath(const QString &imagePath);
    // True only if path is an absolute drive-letter path to a real file named
    // usbipd.exe, with no links in the way, that standard users cannot
    // replace. On failure reason (optional) says why. Always false off Windows.
    static bool isTrustedUsbipdPath(const QString &path, QString *reason = nullptr);
    // The same checks for any expected file name. Exposed so tests can run the
    // positive case against a real system file; production code only calls
    // isTrustedUsbipdPath().
    static bool isTrustedExecutablePath(const QString &path, const QString &expectedFileName,
                                        QString *reason = nullptr);
    // True if this process's own standard-user token (the limited token when
    // the process is elevated) could modify the file, or any directory above
    // it up to and including the drive root. Decided with a real AccessCheck,
    // so group membership and ownership count. At the drive root only rights
    // that can replace or re-permission existing children count, because the
    // default root ACL lets any user add new folders. Any API error, NULL
    // DACL or unreadable ACL counts as writable. Always true off Windows.
    static bool isWritableByStandardUsers(const QString &path);

    // Test seam: runs `program arguments` as the version probe with the given
    // timeout, exactly as refresh() does with usbipd.exe --version.
    void probeWith(const QString &program, const QStringList &arguments, int timeoutMs);

    // Read-only SCM queries; also used immediately before a tunnel attempt.
    static State probeServices();
    static QString readinessError(State state);

    Q_INVOKABLE void refresh();

    State state() const { return m_State; }
    QString usbipdVersion() const { return m_Version; }
    bool checking() const { return m_Checking; }

signals:
    void stateChanged();
    void checkingChanged();

private:
    explicit UsbForwardingEnvironment(QObject *parent = nullptr);

    void startServiceProbe();
    void finish(State state);

    State m_State = Checking;
    QString m_Version;
    bool m_Checking = false;
};
