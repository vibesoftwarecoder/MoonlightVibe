#pragma once

#include <QString>
#include <functional>

// Runs one program elevated (UAC) or not, waits for it on the CALLING thread
// and reports the real outcome. Blocking by design: call it from a worker
// thread, never from the GUI thread. Windows only; elsewhere it reports "not
// launched".
namespace UsbForwarding {

struct ElevationResult
{
    bool launched = false;   // the process was started (UAC accepted)
    bool timedOut = false;   // it was still running after the normal timeout
    bool exited = false;     // we saw it exit (possibly after the normal timeout)
    bool cancelled = false;  // the user declined the UAC prompt
    quint32 exitCode = 0;    // valid when exited
    bool succeeded() const { return launched && !timedOut && exited && exitCode == 0; }
};

// Called once, on the waiting thread, when the normal timeout passes and the
// process is still running. The wait then continues up to the hard limit.
using SlowCallback = std::function<void()>;

// verb is "runas" for the real thing; tests pass "open" so nothing elevates.
// exe must be an absolute path; arguments are used verbatim, so callers build
// them from validated input only. The process handle is kept for the whole
// wait (normal timeout plus hard limit) and always closed. A process that is
// still running at the hard limit is left alone: it is not ours to kill.
ElevationResult launchAndWait(const QString &verb, const QString &exe, const QString &arguments,
                              const QString &workingDirectory, int timeoutMs, int hardLimitMs,
                              const SlowCallback &onSlow = SlowCallback());

// Seam used by UsbForwardingBackend so tests never really elevate.
using ElevatedLauncher = std::function<ElevationResult(const QString &exe, const QString &arguments,
                                                       const QString &workingDirectory, int timeoutMs,
                                                       int hardLimitMs, const SlowCallback &onSlow)>;

// The production launcher: launchAndWait("runas", ...).
ElevationResult launchElevated(const QString &exe, const QString &arguments,
                               const QString &workingDirectory, int timeoutMs, int hardLimitMs,
                               const SlowCallback &onSlow);

} // namespace UsbForwarding
