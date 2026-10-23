#include "usbforwardingelevation.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#endif

namespace UsbForwarding {

ElevationResult launchAndWait(const QString &verb, const QString &exe, const QString &arguments,
                              const QString &workingDirectory, int timeoutMs, int hardLimitMs,
                              const SlowCallback &onSlow)
{
    ElevationResult result;
#ifdef Q_OS_WIN
    /* ShellExecuteEx wants COM on the calling thread; this runs on a worker. */
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::wstring wideVerb = verb.toStdWString();
    const std::wstring wideExe = QDir::toNativeSeparators(exe).toStdWString();
    const std::wstring wideArgs = arguments.toStdWString();
    const std::wstring wideDir = QDir::toNativeSeparators(workingDirectory).toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    /* NOCLOSEPROCESS: we need the handle to wait on and read the exit code.
     * NOASYNC: the launch completes on this thread, which then waits. */
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = wideVerb.c_str();
    info.lpFile = wideExe.c_str();
    info.lpParameters = wideArgs.c_str();
    info.lpDirectory = wideDir.empty() ? nullptr : wideDir.c_str();
    info.nShow = SW_HIDE;

    if (ShellExecuteExW(&info) && info.hProcess != nullptr) {
        result.launched = true;
        const auto finish = [&](DWORD wait) {
            if (wait != WAIT_OBJECT_0) {
                return false;
            }
            DWORD code = 1;
            result.exitCode = GetExitCodeProcess(info.hProcess, &code) ? static_cast<quint32>(code) : 1;
            result.exited = true;
            return true;
        };
        if (!finish(WaitForSingleObject(info.hProcess, timeoutMs < 0 ? 0 : static_cast<DWORD>(timeoutMs)))) {
            /* Still running (or the wait failed). Say so, then keep the handle
             * and keep waiting up to the hard limit: the caller stays "busy"
             * for as long as the process may still change usbipd's state. */
            result.timedOut = true;
            if (onSlow) {
                onSlow();
            }
            finish(WaitForSingleObject(info.hProcess, hardLimitMs < 0 ? 0 : static_cast<DWORD>(hardLimitMs)));
        }
        CloseHandle(info.hProcess);
    } else {
        result.cancelled = (GetLastError() == ERROR_CANCELLED);
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
#else
    Q_UNUSED(verb);
    Q_UNUSED(exe);
    Q_UNUSED(arguments);
    Q_UNUSED(workingDirectory);
    Q_UNUSED(timeoutMs);
    Q_UNUSED(hardLimitMs);
    Q_UNUSED(onSlow);
#endif
    return result;
}

ElevationResult launchElevated(const QString &exe, const QString &arguments,
                               const QString &workingDirectory, int timeoutMs, int hardLimitMs,
                               const SlowCallback &onSlow)
{
    return launchAndWait(QStringLiteral("runas"), exe, arguments, workingDirectory, timeoutMs, hardLimitMs,
                         onSlow);
}

} // namespace UsbForwarding
