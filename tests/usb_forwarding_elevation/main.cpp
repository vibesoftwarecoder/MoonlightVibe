// Asynchronous elevation: the UAC prompt and the wait for usbipd never block
// the caller, and the REAL result is reported. The elevated launch itself is a
// seam (nothing in here ever elevates); the real wait/exit-code code is
// exercised with the non-elevating "open" verb on cmd.exe.
#include "../../app/backend/usbforwardingbackend.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>
#include <atomic>
#include <condition_variable>
#include <mutex>

using UsbForwarding::ElevationResult;
using UsbForwarding::SlowCallback;

namespace {

// A launcher that blocks until the test releases it, and records its call.
struct FakeLauncher
{
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    bool slowRequested = false;
    ElevationResult result;
    std::atomic<int> calls{0};
    QString exe, arguments, workDir;
    int timeoutMs = 0;
    int hardLimitMs = 0;
    Qt::HANDLE thread = nullptr;

    ElevationResult run(const QString &e, const QString &a, const QString &w, int t, int hard,
                        const SlowCallback &onSlow)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            exe = e;
            arguments = a;
            workDir = w;
            timeoutMs = t;
            hardLimitMs = hard;
            thread = QThread::currentThreadId();
        }
        ++calls;
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10), [this] { return released || slowRequested; });
        if (!released && slowRequested) {
            // Like the real launcher: tell the caller the process is slow, then keep waiting.
            lock.unlock();
            onSlow();
            lock.lock();
            cv.wait_for(lock, std::chrono::seconds(10), [this] { return released; });
        }
        return result;
    }
    void triggerSlow()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            slowRequested = true;
        }
        cv.notify_all();
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};

struct Hooked
{
    std::shared_ptr<FakeLauncher> launcher = std::make_shared<FakeLauncher>();
    std::shared_ptr<std::atomic<int>> refreshes = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> busyAtRefresh = std::make_shared<std::atomic<bool>>(true);

    explicit Hooked(const QString &located = QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe"),
                    int timeoutMs = 0, int hardLimitMs = 0)
    {
        UsbForwardingBackend::TestHooks hooks;
        hooks.locateUsbipd = [located] { return located; };
        auto l = launcher;
        hooks.launcher = [l](const QString &e, const QString &a, const QString &w, int t, int hard,
                             const SlowCallback &onSlow) { return l->run(e, a, w, t, hard, onSlow); };
        auto count = refreshes;
        auto busy = busyAtRefresh;
        hooks.refresh = [count, busy] {
            ++*count;
            busy->store(UsbForwardingBackend::get()->busy());
        };
        hooks.elevationTimeoutMs = timeoutMs;
        hooks.elevationHardLimitMs = hardLimitMs;
        UsbForwardingBackend::setTestHooks(hooks);
    }
    ~Hooked()
    {
        launcher->release();
        UsbForwardingBackend::setTestHooks(UsbForwardingBackend::TestHooks{});
    }
};

} // namespace

class ElevationTest : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { UsbForwardingBackend::setTestHooks(UsbForwardingBackend::TestHooks{}); }

#ifdef Q_OS_WIN
    void resultIsDeliveredAsynchronouslyAndTheCallerIsNotBlocked()
    {
        Hooked hooked(QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe"), 4321, 8765);
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);

        // Keep the GUI loop observable: it must keep ticking while the prompt is open.
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&ticks] { ++ticks; });
        ticker.start();

        QElapsedTimer timer;
        timer.start();
        backend->bind(QStringLiteral("1-2"));
        QVERIFY2(timer.elapsed() < 100, "bind() blocked the caller");
        QVERIFY(backend->busy());
        QVERIFY(finished.isEmpty());          // launching is not success

        QTest::qWait(400);                    // "the UAC prompt is open"
        QVERIFY2(ticks >= 15, qPrintable(QStringLiteral("event loop starved: %1 ticks").arg(ticks)));
        QVERIFY(finished.isEmpty());          // still waiting for the real result
        QCOMPARE(hooked.launcher->calls.load(), 1);
        QCOMPARE(hooked.refreshes->load(), 0); // and no fixed timer refreshes behind its back

        hooked.launcher->result.launched = true;   // usbipd ran and exited 0
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), true);
        QVERIFY(!backend->busy());
        QCOMPARE(hooked.refreshes->load(), 1);   // refreshed from the real result...
        QVERIFY(!hooked.busyAtRefresh->load());  // ...after the operation was over

        // The worker, not this thread, ran the launcher, with the fixed arguments.
        QVERIFY(hooked.launcher->thread != QThread::currentThreadId());
        QCOMPARE(hooked.launcher->arguments, QStringLiteral("bind --busid 1-2"));
        QCOMPARE(hooked.launcher->exe, QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe"));
        QCOMPARE(hooked.launcher->timeoutMs, 4321);
        QCOMPARE(hooked.launcher->hardLimitMs, 8765);
        // Nothing else follows (a leftover 1.5 s timer would refresh again).
        QTest::qWait(1800);
        QCOMPARE(hooked.refreshes->load(), 1);
    }

    void unbindUsesTheGuidWhenThereIsNoBusId()
    {
        Hooked hooked;
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        hooked.launcher->result.launched = true;
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        backend->unbind(QString(), QStringLiteral("9b6a62f8-0123-4567-89ab-0123456789ab"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(hooked.launcher->arguments, QStringLiteral("unbind --guid 9b6a62f8-0123-4567-89ab-0123456789ab"));
    }

    void failuresAreReportedForWhatTheyAreAndNeverRefresh_data()
    {
        QTest::addColumn<bool>("launched");
        QTest::addColumn<bool>("timedOut");
        QTest::addColumn<bool>("cancelled");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("expectedPart");
        QTest::newRow("declined") << false << false << true << 0 << "cancelled";
        QTest::newRow("could not start") << false << false << false << 0 << "failed";
        QTest::newRow("usbipd error") << true << false << false << 5 << "exit 5";
        QTest::newRow("timeout") << true << true << false << 0 << "still running";
    }
    void failuresAreReportedForWhatTheyAreAndNeverRefresh()
    {
        QFETCH(bool, launched);
        QFETCH(bool, timedOut);
        QFETCH(bool, cancelled);
        QFETCH(int, exitCode);
        QFETCH(QString, expectedPart);
        Hooked hooked;
        hooked.launcher->result.launched = launched;
        hooked.launcher->result.timedOut = timedOut;
        hooked.launcher->result.exited = launched && !timedOut;
        hooked.launcher->result.cancelled = cancelled;
        hooked.launcher->result.exitCode = static_cast<quint32>(exitCode);
        hooked.launcher->release();
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);
        QVERIFY2(finished.first().at(1).toString().contains(expectedPart),
                 qPrintable(finished.first().at(1).toString()));
        QVERIFY(!backend->busy());
        QTest::qWait(100);
        // A timed-out operation may still have changed usbipd: look at the real state.
        QCOMPARE(hooked.refreshes->load(), timedOut ? 1 : 0);
    }

    void invalidInputNeverReachesTheLauncher()
    {
        Hooked hooked;
        hooked.launcher->release();
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2 & calc.exe"));
        backend->bind(QStringLiteral("IncompatibleHub"));
        backend->unbind(QStringLiteral("x"), QStringLiteral("not-a-guid"));
        QCOMPARE(finished.count(), 3);
        for (const auto &args : finished) {
            QCOMPARE(args.at(0).toBool(), false);
        }
        QTest::qWait(100);
        QCOMPARE(hooked.launcher->calls.load(), 0);
    }

    void aSecondOperationWhileOneIsPendingIsRefused()
    {
        Hooked hooked;
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2"));
        QTRY_COMPARE(hooked.launcher->calls.load(), 1);
        backend->bind(QStringLiteral("1-3"));
        QCOMPARE(finished.count(), 1);              // refused at once
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(hooked.launcher->calls.load(), 1);  // only one prompt
        hooked.launcher->result.launched = true;
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        QTRY_COMPARE(finished.count(), 2);
    }

    void missingUsbipdIsReportedWithoutLaunching()
    {
        Hooked hooked{QString()};
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2"));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(hooked.launcher->calls.load(), 0);
        QVERIFY(!backend->busy());
    }

    void aSlowOperationKeepsTheBackendBusyAndIsRefreshedWhenItReallyExits()
    {
        Hooked hooked;
        auto *backend = UsbForwardingBackend::get();
        QSignalSpy finished(backend, &UsbForwardingBackend::operationFinished);
        backend->bind(QStringLiteral("1-2"));
        QTRY_COMPARE(hooked.launcher->calls.load(), 1);

        hooked.launcher->triggerSlow(); // the normal timeout passes, usbipd still runs
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);
        QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("still running")),
                 qPrintable(finished.first().at(1).toString()));
        QVERIFY(backend->busy());                    // still busy
        QCOMPARE(hooked.refreshes->load(), 0);

        backend->bind(QStringLiteral("1-3"));        // a second elevated operation
        QCOMPARE(finished.count(), 2);
        QVERIFY(finished.at(1).at(1).toString().contains(QStringLiteral("Another")));
        QCOMPARE(hooked.launcher->calls.load(), 1);  // refused: only one process exists

        hooked.launcher->result.launched = true;     // now it really exits
        hooked.launcher->result.timedOut = true;
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        QTRY_VERIFY(!backend->busy());
        QCOMPARE(hooked.refreshes->load(), 1);       // list refreshed from the real state
        QCOMPARE(finished.count(), 2);               // and no second verdict for the same operation
    }

    void theWaitDoesNotUseTheGlobalThreadPoolSoQuittingCannotHang()
    {
        Hooked hooked;
        auto *backend = UsbForwardingBackend::get();
        backend->bind(QStringLiteral("1-2"));
        QTRY_COMPARE(hooked.launcher->calls.load(), 1);
        // QCoreApplication's destructor waits for the global pool; it must be idle.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(200));
        QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), 0);
        hooked.launcher->result.launched = true;
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        QTRY_VERIFY(!backend->busy());
    }

    void aResultArrivingAfterTheBackendIsGoneIsDroppedSafely()
    {
        Hooked hooked;
        auto *backend = new UsbForwardingBackend;
        backend->bind(QStringLiteral("1-2"));
        QTRY_COMPARE(hooked.launcher->calls.load(), 1);
        delete backend;                       // the prompt is still open
        hooked.launcher->result.launched = true;
        hooked.launcher->result.exited = true;
        hooked.launcher->release();
        QTest::qWait(300);                    // delivery must find nobody and do nothing
        QCOMPARE(hooked.refreshes->load(), 0);
    }

    void destroyingTheBackendWithARunningProbeIsQuietAndFast()
    {
        static QStringList warnings;
        warnings.clear();
        const QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &message) { warnings.append(message); });
        auto *backend = new UsbForwardingBackend;
        auto *probe = new QProcess(backend);   // stands in for the `usbipd state` probe
        probe->start(QStringLiteral("C:/Windows/System32/PING.EXE"),
                     { QStringLiteral("-n"), QStringLiteral("60"), QStringLiteral("127.0.0.1") });
        const bool started = probe->waitForStarted(5000);
        QElapsedTimer timer;
        timer.start();
        delete backend;                        // the application is exiting mid-probe
        const qint64 elapsed = timer.elapsed();
        qInstallMessageHandler(previous);
        if (!started) {
            QSKIP("could not start PING.EXE in this environment");
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('|'))));
        QVERIFY2(elapsed < 3000, qPrintable(QString::number(elapsed)));
    }

    // ---- the real wait, with the non-elevating "open" verb ---------------

    void launchAndWaitReadsTheRealExitCode()
    {
        const QString dir = qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32");
        const QString cmd = dir + QStringLiteral("/cmd.exe");
        ElevationResult three = UsbForwarding::launchAndWait(QStringLiteral("open"), cmd, QStringLiteral("/c exit 3"), dir, 10000, 10000);
        if (!three.launched) {
            QSKIP("could not start cmd.exe in this environment");
        }
        QVERIFY(!three.timedOut);
        QCOMPARE(three.exitCode, 3u);
        QVERIFY(!three.succeeded());
        ElevationResult zero = UsbForwarding::launchAndWait(QStringLiteral("open"), cmd, QStringLiteral("/c exit 0"), dir, 10000, 10000);
        QVERIFY(zero.succeeded());
    }

    void launchAndWaitGivesUpAtTheHardLimit()
    {
        const QString dir = qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32");
        const QString cmd = dir + QStringLiteral("/cmd.exe");
        QElapsedTimer timer;
        timer.start();
        int slow = 0;
        const ElevationResult r = UsbForwarding::launchAndWait(
            QStringLiteral("open"), cmd, QStringLiteral("/c ping -n 6 127.0.0.1 >nul"), dir, 300, 300,
            [&slow] { ++slow; });
        if (!r.launched) {
            QSKIP("could not start cmd.exe in this environment");
        }
        QVERIFY(r.timedOut);
        QVERIFY(!r.exited);
        QVERIFY(!r.succeeded());
        QCOMPARE(slow, 1);
        QVERIFY2(timer.elapsed() < 3000, "did not return at the hard limit");
    }

    void launchAndWaitKeepsWaitingAfterTheNormalTimeoutAndReadsTheLateExit()
    {
        const QString dir = qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32");
        const QString cmd = dir + QStringLiteral("/cmd.exe");
        int slow = 0;
        const ElevationResult r = UsbForwarding::launchAndWait(
            QStringLiteral("open"), cmd, QStringLiteral("/c ping -n 3 127.0.0.1 >nul & exit 4"), dir, 200, 15000,
            [&slow] { ++slow; });
        if (!r.launched) {
            QSKIP("could not start cmd.exe in this environment");
        }
        QCOMPARE(slow, 1);       // told once that it is slow...
        QVERIFY(r.timedOut);
        QVERIFY(r.exited);       // ...but the handle was kept, so the real exit was seen
        QCOMPARE(r.exitCode, 4u);
        QVERIFY(!r.succeeded());
    }

    void launchAndWaitReportsAProgramThatCannotStart()
    {
        const ElevationResult missing = UsbForwarding::launchAndWait(
            QStringLiteral("open"), QStringLiteral("C:/definitely/not/here/x.exe"), QString(), QString(), 1000, 1000);
        QVERIFY(!missing.launched);
        QVERIFY(!missing.cancelled);
        QVERIFY(!missing.succeeded());
    }
#endif
};

QTEST_GUILESS_MAIN(ElevationTest)
#include "main.moc"
