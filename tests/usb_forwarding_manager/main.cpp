// USB forwarding session manager: lifecycle, trust boundary, retries, snapshot.
// Everything runs with fakes (capability, device list, tunnel) except the two
// end-to-end tests, which use loopback sockets and throwaway certificates.
#include "../../app/backend/usbforwardingmanager.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace UsbForwarding;

namespace {

std::mutex g_LogMutex;
QStringList g_Log;
void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    g_Log.append(message);
}
QStringList logLines()
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    return g_Log;
}

const QByteArray kToken = QByteArray("a1b2c3d4").repeated(8);  // 64 hex characters
const QByteArray kToken2 = QByteArray("0f1e2d3c").repeated(8);
const quint16 kTunnelPort = 40123;

enum class Act { Forward, Fail, Hang, ForwardThenFail, ForwardThenClose };

// Everything the fakes record, shared with the worker and pool threads.
struct Env
{
    Env() { clock.start(); }

    std::mutex mutex;
    QList<TunnelConfig> started;
    QList<qint64> startedAt;
    std::atomic<int> alive{0};
    std::atomic<int> stops{0};
    std::atomic<int> fetches{0};
    std::atomic<int> lists{0};
    std::atomic<bool> holdFetch{false};
    QElapsedTimer clock;

    int afterMs = 30;
    std::function<void()> onStart;   // runs inside FakeTunnel::start, on the worker
    std::function<Act(int index)> act = [](int) { return Act::Forward; };
    std::function<Capability(int call)> capability = [](int) {
        Capability c;
        c.available = true;
        c.reason = QStringLiteral("ready");
        c.port = kTunnelPort;
        c.token = kToken;
        return c;
    };
    std::function<QList<SharedDevice>()> devices = [] {
        return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}};
    };

    int startedCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(started.size());
    }
    TunnelConfig startedConfig(int i)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return started.at(i);
    }
    QList<qint64> startTimes()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return startedAt;
    }
};

class FakeTunnel final : public TunnelHandle
{
public:
    FakeTunnel(const TunnelConfig &config, std::shared_ptr<Env> env) : m_Config(config), m_Env(std::move(env))
    {
        ++m_Env->alive;
    }
    ~FakeTunnel() override { --m_Env->alive; }

    bool start(QString *) override
    {
        int index;
        {
            std::lock_guard<std::mutex> lock(m_Env->mutex);
            index = static_cast<int>(m_Env->started.size());
            m_Env->started.append(m_Config);
            m_Env->startedAt.append(m_Env->clock.elapsed());
        }
        if (m_Env->onStart) {
            m_Env->onStart();
        }
        const Act act = m_Env->act(index);
        switch (act) {
        case Act::Forward:
            QTimer::singleShot(0, this, [this] { emitForwarding(); });
            break;
        case Act::Fail:
            QTimer::singleShot(0, this, [this] { emitFinished(QStringLiteral("boom")); });
            break;
        case Act::Hang:
            break;
        case Act::ForwardThenFail:
            QTimer::singleShot(0, this, [this] { emitForwarding(); });
            QTimer::singleShot(m_Env->afterMs, this, [this] { emitFinished(QStringLiteral("lost")); });
            break;
        case Act::ForwardThenClose:
            QTimer::singleShot(0, this, [this] { emitForwarding(); });
            QTimer::singleShot(m_Env->afterMs, this, [this] { emitFinished(QString()); });
            break;
        }
        return true;
    }
    void stop() noexcept override
    {
        m_Stopped = true;
        ++m_Env->stops;
    }

private:
    void emitForwarding()
    {
        if (!m_Stopped) emit forwarding();
    }
    void emitFinished(const QString &message)
    {
        if (!m_Stopped) emit finished(message);
    }
    TunnelConfig m_Config;
    std::shared_ptr<Env> m_Env;
    bool m_Stopped = false;
};

ManagerSeams fakeSeams(const std::shared_ptr<Env> &env)
{
    ManagerSeams seams;
    seams.fetchCapability = [env] {
        const int call = env->fetches++;
        while (env->holdFetch) {
            QThread::msleep(10);
        }
        return env->capability(call);
    };
    seams.listDevices = [env] {
        ++env->lists;
        return env->devices();
    };
    seams.createTunnel = [env](const TunnelConfig &config) -> std::unique_ptr<TunnelHandle> {
        return std::make_unique<FakeTunnel>(config, env);
    };
    return seams;
}

ManagerOptions fastOptions(QList<int> backoff = {30, 60, 120})
{
    ManagerOptions options;
    options.retryBackoffMs = backoff;
    options.stableAfterMs = 10 * 1000;
    return options;
}

template <typename F>
bool waitUntil(F condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QTest::qWait(5);
    }
    return true;
}

DeviceState deviceState(const Snapshot &snap, const QByteArray &busId)
{
    for (const DeviceStatus &d : snap.devices) {
        if (d.busId == busId) {
            return d.state;
        }
    }
    return DeviceState::NotShared;
}

} // namespace

class ManagerTest : public QObject
{
    Q_OBJECT

    TestIdentity a, b;

    // a is the client's paired identity, b is the paired host's certificate.
    ManagerConfig config(const QStringList &devices = {QStringLiteral("054c:0ce6")}) const
    {
        ManagerConfig c;
        c.enabled = true;
        c.autoDevices = devices;
        c.streamAddress = QStringLiteral("192.168.50.20");
        c.pairedAddress = QStringLiteral("192.168.50.20");
        c.pinnedServerCertificate = b.certificate;
        c.clientSsl.setLocalCertificate(a.certificate);
        c.clientSsl.setPrivateKey(a.key);
        return c;
    }

private slots:
    void initTestCase()
    {
        a = loadTestIdentity(QStringLiteral("a"));
        b = loadTestIdentity(QStringLiteral("b"));
        QVERIFY2(a.valid() && b.valid(),
                 "test certificates missing: run tests/run_usb_forwarding_tests.ps1");
        qInstallMessageHandler(captureMessages);
    }

    void cleanup()
    {
        // The token is a credential: it must not appear in any log line, in any test.
        for (const QString &line : logLines()) {
            QVERIFY2(!line.contains(QString::fromLatin1(kToken)), "token was logged");
            QVERIFY2(!line.contains(QString::fromLatin1(kToken2)), "token was logged");
        }
    }

    // ---- turned off -------------------------------------------------------

    void disabledInThePreferencesDoesNothingAtAll()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        ManagerConfig c = config();
        c.enabled = false;
        QVERIFY(manager.start(c));
        QTest::qWait(300);
        QCOMPARE(env->fetches.load(), 0);
        QCOMPARE(env->lists.load(), 0);
        QCOMPARE(env->startedCount(), 0);
        QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), 0);
        QCOMPARE(manager.snapshot().state, State::Off);
        QVERIFY(!manager.snapshot().status.isEmpty());
        QVERIFY(!manager.toggle()); // nothing to toggle while off in the preferences
        manager.stop();
        QCOMPARE(manager.snapshot().state, State::Off);
    }

    void neverStartedHasNothingToToggleOrStop()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(!manager.toggle());
        manager.stop();
        manager.stop();
        QCOMPARE(manager.snapshot().state, State::Off);
        QCOMPARE(env->fetches.load(), 0);
    }

    // ---- what the host says ----------------------------------------------

    void hostThatSaysNoLeavesThingsQuiet_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("expectedState");
        QTest::newRow("disabled") << "disabled" << int(State::HostDisabled);
        QTest::newRow("forbidden") << "forbidden" << int(State::HostForbidden);
        QTest::newRow("unauthorized") << "unauthorized" << int(State::HostForbidden);
    }
    void hostThatSaysNoLeavesThingsQuiet()
    {
        QFETCH(QString, reason);
        QFETCH(int, expectedState);
        auto env = std::make_shared<Env>();
        env->capability = [reason](int) {
            Capability c;
            c.available = false;
            c.reason = reason;
            return c;
        };
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return int(manager.snapshot().state) == expectedState; }));
        QTest::qWait(400); // far longer than every backoff: nothing may happen
        QCOMPARE(env->fetches.load(), 1);                 // asked once, never again
        QCOMPARE(env->startedCount(), 0);
        QCOMPARE(env->lists.load(), 0);
        QCOMPARE(int(manager.snapshot().state), expectedState);
        QVERIFY(manager.snapshot().devices.isEmpty());
    }

    void unavailableHostIsRecheckedAndThenStartsTunnels()
    {
        auto env = std::make_shared<Env>();
        const auto ready = env->capability;
        env->capability = [ready](int call) {
            if (call < 2) {
                Capability c;
                c.reason = QStringLiteral("unavailable");  // e.g. the host has not seen the stream yet
                return c;
            }
            return ready(call);
        };
        Manager manager(fakeSeams(env), fastOptions({20, 20}));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
        QCOMPARE(env->fetches.load(), 3);
        QCOMPARE(manager.snapshot().state, State::Active);
    }

    void unavailableHostIsRecheckedOnlyABoundedNumberOfTimes()
    {
        auto env = std::make_shared<Env>();
        env->capability = [](int) {
            Capability c;
            c.reason = QStringLiteral("unavailable");
            return c;
        };
        Manager manager(fakeSeams(env), fastOptions({20, 20}));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return env->fetches.load() == 3; }));
        QTest::qWait(300);
        QCOMPARE(env->fetches.load(), 3); // 1 + two rechecks, then quiet
        QCOMPARE(env->startedCount(), 0);
        QCOMPARE(manager.snapshot().state, State::HostUnavailable);
    }

    void unreachableHostRetriesWithBackoffThenGivesUp()
    {
        auto env = std::make_shared<Env>();
        env->capability = [](int) -> Capability { throw std::runtime_error("network down"); };
        Manager manager(fakeSeams(env), fastOptions({20, 20}));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().state == State::Failed; }));
        QTest::qWait(300);
        QCOMPARE(env->fetches.load(), 3);
        QCOMPARE(env->startedCount(), 0);
        QCOMPARE(manager.snapshot().state, State::Failed);
    }

    // ---- which devices ----------------------------------------------------

    void readyStartsOnlyAutoDevicesThatAreShared()
    {
        auto env = std::make_shared<Env>();
        env->devices = [] {
            return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"},
                                       {QStringLiteral("dead:beef"), "1-2"},        // shared but not selected
                                       {QStringLiteral("0781:5581#abc123"), "1-3"}};
        };
        Manager manager(fakeSeams(env), fastOptions());
        // Selection is case-insensitive; 1111:2222 is selected but not shared.
        QVERIFY(manager.start(config({QStringLiteral("054C:0CE6"), QStringLiteral("0781:5581#ABC123"),
                                       QStringLiteral("1111:2222"), QStringLiteral("054c:0ce6")})));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 2; }));
        QTest::qWait(150);
        QCOMPARE(env->startedCount(), 2);
        QStringList busIds;
        for (int i = 0; i < 2; ++i) {
            busIds << QString::fromLatin1(env->startedConfig(i).busId);
        }
        busIds.sort();
        QCOMPARE(busIds, (QStringList{QStringLiteral("1-1"), QStringLiteral("1-3")}));
        const Snapshot snap = manager.snapshot();
        QCOMPARE(deviceState(snap, "1-1"), DeviceState::Forwarding);
        QCOMPARE(deviceState(snap, "1-3"), DeviceState::Forwarding);
        QCOMPARE(deviceState(snap, "1-2"), DeviceState::NotShared); // never tracked
        bool sawNotShared = false;
        for (const DeviceStatus &d : snap.devices) {
            QVERIFY(d.busId != "1-2");
            if (d.identity == QLatin1String("1111:2222")) {
                QCOMPARE(d.state, DeviceState::NotShared);
                sawNotShared = true;
            }
        }
        QVERIFY(sawNotShared);
        QCOMPARE(snap.devices.size(), 3); // duplicate selection collapsed
        QVERIFY(snap.status.contains(QStringLiteral("2")));
    }

    void noSelectedDevicesStartsNothingAndSaysSo()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config(QStringList())));
        QVERIFY(waitUntil([&] { return manager.snapshot().state == State::Active; }));
        QTest::qWait(100);
        QCOMPARE(env->startedCount(), 0);
        QVERIFY(!manager.snapshot().status.isEmpty());
    }

    void backendListFilterKeepsOnlySharedConnectedSupportedDevices()
    {
        const auto device = [](const QString &identity, const QString &busId, bool connected, bool bound,
                               bool supported) {
            QVariantMap m;
            m.insert(QStringLiteral("identity"), identity);
            m.insert(QStringLiteral("busId"), busId);
            m.insert(QStringLiteral("isConnected"), connected);
            m.insert(QStringLiteral("isBound"), bound);
            m.insert(QStringLiteral("isSupported"), supported);
            return QVariant(m);
        };
        const QList<SharedDevice> result = sharedDevicesFromBackendList(
            {device(QStringLiteral("054c:0ce6"), QStringLiteral("1-2"), true, true, true),
             device(QStringLiteral("054c:0ce6"), QStringLiteral("1-3"), true, false, true),   // not shared
             device(QStringLiteral("054c:0ce6"), QStringLiteral("1-4"), false, true, true),   // unplugged
             device(QStringLiteral("054c:0ce6"), QStringLiteral("1-5"), true, true, false),   // not supported
             device(QString(), QStringLiteral("1-6"), true, true, true),                      // no identity
             device(QStringLiteral("0001:0002"), QStringLiteral("1-2; calc"), true, true, true)});  // bad bus id
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().busId, QByteArray("1-2"));
    }

    // ---- trust boundary ---------------------------------------------------

    void tunnelConfigCarriesThePinnedCertificateAndTheClientIdentity()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        const ManagerConfig c = config();
        QVERIFY(manager.start(c));
        QVERIFY(waitUntil([&] { return env->startedCount() == 1; }));
        const TunnelConfig tc = env->startedConfig(0);
        QCOMPARE(tc.host, ManagerConfig::canonicalHostAddress(c.streamAddress)); // the stream's own address
        QCOMPARE(tc.port, kTunnelPort);
        QCOMPARE(tc.sessionToken, kToken);
        QCOMPARE(tc.busId, QByteArray("1-1"));
        QVERIFY(!tc.pinnedServerCertificate.isNull());
        QCOMPARE(tc.pinnedServerCertificate.toDer(), b.certificate.toDer());
        QCOMPARE(tc.sslConfiguration.localCertificate().toDer(), a.certificate.toDer());
        QCOMPARE(tc.sslConfiguration.privateKey().toDer(), a.key.toDer());
        QVERIFY(tc.valid()); // and the real tunnel would accept it
    }

    void mismatchedOrUserSuppliedAddressesAreRefused_data()
    {
        QTest::addColumn<QString>("stream");
        QTest::addColumn<QString>("paired");
        const QString paired = QStringLiteral("192.168.50.20");
        QTest::newRow("different host") << "203.0.113.9" << paired;
        QTest::newRow("typed host name") << "evil.example.com" << paired;
        QTest::newRow("empty stream") << "" << paired;
        QTest::newRow("empty paired") << paired << "";
        QTest::newRow("both empty") << "" << "";
        QTest::newRow("scheme") << "https://192.168.50.20" << "https://192.168.50.20";
        QTest::newRow("port") << "192.168.50.20:47984" << "192.168.50.20:47984";
        QTest::newRow("path") << "192.168.50.20/x" << "192.168.50.20/x";
        QTest::newRow("user info") << "me@192.168.50.20" << "me@192.168.50.20";
        QTest::newRow("trailing space") << "192.168.50.20 " << "192.168.50.20 ";
        QTest::newRow("inner space") << "evil host" << "evil host";
        QTest::newRow("bracketed v4") << "[10.0.0.5]" << "[10.0.0.5]";
        QTest::newRow("bracketed name") << "[pc.local]" << "[pc.local]";
        QTest::newRow("empty label") << "pc..local" << "pc..local";
        QTest::newRow("leading dot") << ".pc.local" << ".pc.local";
        QTest::newRow("ip with trailing dot") << "10.0.0.5." << "10.0.0.5.";
        QTest::newRow("tab") << "pc\tlocal" << "pc\tlocal";
        QTest::newRow("malformed ip") << "999.1.1.1" << "999.1.1.1";
        QTest::newRow("stream is name, paired is its ip") << "pc.local" << paired;
    }
    void mismatchedOrUserSuppliedAddressesAreRefused()
    {
        QFETCH(QString, stream);
        QFETCH(QString, paired);
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        ManagerConfig c = config();
        c.streamAddress = stream;
        c.pairedAddress = paired;
        QVERIFY(!manager.start(c));
        QTest::qWait(150);
        QCOMPARE(manager.snapshot().state, State::InvalidConfig);
        QCOMPARE(env->fetches.load(), 0);   // no request, so no token
        QCOMPARE(env->startedCount(), 0);
        QVERIFY(!manager.toggle());
    }

    void missingCertificatesAreRefused_data()
    {
        QTest::addColumn<int>("what");
        QTest::newRow("no pinned certificate") << 0;
        QTest::newRow("no client certificate") << 1;
        QTest::newRow("no client key") << 2;
    }
    void missingCertificatesAreRefused()
    {
        QFETCH(int, what);
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        ManagerConfig c = config();
        if (what == 0) c.pinnedServerCertificate = QSslCertificate();
        if (what == 1) c.clientSsl.setLocalCertificate(QSslCertificate());
        if (what == 2) c.clientSsl.setPrivateKey(QSslKey());
        QVERIFY(!manager.start(c));
        QCOMPARE(manager.snapshot().state, State::InvalidConfig);
        QCOMPARE(env->fetches.load(), 0);
    }

    void equivalentSpellingsOfTheSameHostAreAccepted_data()
    {
        QTest::addColumn<QString>("stream");
        QTest::addColumn<QString>("paired");
        QTest::newRow("name case") << "Gaming-PC.local" << "gaming-pc.local";
        QTest::newRow("ipv6 spelling") << "::1" << "0:0:0:0:0:0:0:1";
        QTest::newRow("ipv4") << "10.0.0.5" << "10.0.0.5";
        QTest::newRow("bracketed ipv6") << "[::1]" << "::1";
        QTest::newRow("bracketed long ipv6") << "[0:0:0:0:0:0:0:1]" << "[::1]";
        QTest::newRow("trailing dot") << "pc.local." << "pc.local";
        QTest::newRow("idn") << QStringLiteral("b\u00fccher.example") << "xn--bcher-kva.example";
        QTest::newRow("idn upper") << QStringLiteral("B\u00dcCHER.example.") << "xn--bcher-kva.example";
        QTest::newRow("scoped, same scope") << "fe80::1%12" << "[fe80::1%12]";
    }
    void equivalentSpellingsOfTheSameHostAreAccepted()
    {
        QFETCH(QString, stream);
        QFETCH(QString, paired);
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        ManagerConfig c = config();
        c.streamAddress = stream;
        c.pairedAddress = paired;
        QVERIFY(manager.start(c));
        QVERIFY(waitUntil([&] { return env->startedCount() == 1; }));
        // Always the stream's own address, in its canonical spelling.
        QCOMPARE(env->startedConfig(0).host, ManagerConfig::canonicalHostAddress(stream));
    }

    void canonicalFormIsTheSameFromEitherSide_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::addColumn<bool>("same");
        QTest::newRow("brackets") << "[::1]" << "::1" << true;
        QTest::newRow("case") << "PC.Local" << "pc.local" << true;
        QTest::newRow("trailing dot") << "pc.local." << "pc.local" << true;
        QTest::newRow("idn") << QStringLiteral("b\u00fccher.example") << "xn--bcher-kva.example" << true;
        QTest::newRow("scope differs") << "fe80::1%12" << "fe80::1" << false;
        QTest::newRow("other scope") << "fe80::1%12" << "fe80::1%13" << false;
        QTest::newRow("name vs ip") << "pc.local" << "10.0.0.5" << false;
        QTest::newRow("two trailing dots") << "pc.local.." << "pc.local" << false;
        QTest::newRow("different hosts") << "10.0.0.5" << "10.0.0.6" << false;
    }
    void canonicalFormIsTheSameFromEitherSide()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QFETCH(bool, same);
        for (int swap = 0; swap < 2; ++swap) {  // both orders: stream/paired
            const QString x = swap ? b : a;
            const QString y = swap ? a : b;
            auto env = std::make_shared<Env>();
            Manager manager(fakeSeams(env), fastOptions());
            ManagerConfig c = config();
            c.streamAddress = x;
            c.pairedAddress = y;
            QCOMPARE(manager.start(c), same);
            if (same) {
                QCOMPARE(ManagerConfig::canonicalHostAddress(x), ManagerConfig::canonicalHostAddress(y));
            }
            manager.stop();
        }
    }

    void canonicalFormOutputs()
    {
        QCOMPARE(ManagerConfig::canonicalHostAddress(QStringLiteral("[0:0:0:0:0:0:0:1]")), QStringLiteral("::1"));
        QCOMPARE(ManagerConfig::canonicalHostAddress(QStringLiteral("PC.Local.")), QStringLiteral("pc.local"));
        QCOMPARE(ManagerConfig::canonicalHostAddress(QStringLiteral("B\u00dcCHER.example")),
                 QStringLiteral("xn--bcher-kva.example"));
        QCOMPARE(ManagerConfig::canonicalHostAddress(QStringLiteral("fe80::1%12")), QStringLiteral("fe80::1%12"));
        QVERIFY(ManagerConfig::canonicalHostAddress(QStringLiteral("[10.0.0.5]")).isEmpty());
        QVERIFY(ManagerConfig::canonicalHostAddress(QStringLiteral(" pc.local")).isEmpty());
    }

    // ---- stop() from inside the manager's own callbacks ---------------------

    void stopFromInsideTheTunnelFactoryIsSafe()
    {
        for (int i = 0; i < 200; ++i) {
            auto env = std::make_shared<Env>();
            env->devices = [] {
                return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
            };
            ManagerSeams seams = fakeSeams(env);
            Manager *self = nullptr;
            std::atomic<int> calls{0};
            seams.createTunnel = [&, env](const TunnelConfig &config) -> std::unique_ptr<TunnelHandle> {
                if (calls++ == 0) {
                    self->stop();   // on the worker, while startTunnel holds a Dev
                }
                return std::make_unique<FakeTunnel>(config, env);
            };
            Manager manager(seams, fastOptions());
            self = &manager;
            QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
            QVERIFY2(waitUntil([&] { return manager.snapshot().state == State::Stopped; }), "never stopped");
            manager.stop();
            QCOMPARE(env->alive.load(), 0);
            QCOMPARE(env->startedCount(), 0);   // nothing is started after the stop request
            QCOMPARE(calls.load(), 1);
            QCOMPARE(manager.snapshot().state, State::Stopped);
        }
    }

    void stopFromInsideATunnelsStartIsSafe()
    {
        for (int i = 0; i < 200; ++i) {
            auto env = std::make_shared<Env>();
            env->devices = [] {
                return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
            };
            Manager *self = nullptr;
            std::atomic<int> calls{0};
            env->onStart = [&] {
                if (calls++ == 0) self->stop();
            };
            Manager manager(fakeSeams(env), fastOptions());
            self = &manager;
            QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
            QVERIFY2(waitUntil([&] { return manager.snapshot().state == State::Stopped; }), "never stopped");
            manager.stop();
            QCOMPARE(env->alive.load(), 0);
            QVERIFY(env->startedCount() <= 1);  // the second device is never started
            QCOMPARE(manager.snapshot().state, State::Stopped);
        }
    }

    void stopFromTheCapabilityRequestItselfIsSafe()
    {
        for (int i = 0; i < 200; ++i) {
            auto env = std::make_shared<Env>();
            Manager *self = nullptr;
            const auto ready = env->capability;
            std::atomic<bool> done{false};
            env->capability = [&, ready](int call) {
                self->stop();           // from the pool thread, before the answer is delivered
                done = true;
                return ready(call);
            };
            Manager manager(fakeSeams(env), fastOptions());
            self = &manager;
            QVERIFY(manager.start(config()));
            QVERIFY(waitUntil([&] { return done.load(); }));
            QTest::qWait(5);
            manager.stop();
            QCOMPARE(env->alive.load(), 0);
            QCOMPARE(env->startedCount(), 0);
            QCOMPARE(manager.snapshot().state, State::Stopped);
        }
    }

    void stopFromTheDeviceListerAfterTheAnswerIsSafe()
    {
        for (int i = 0; i < 200; ++i) {
            auto env = std::make_shared<Env>();
            env->devices = [] {
                return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
            };
            ManagerSeams seams = fakeSeams(env);
            Manager *self = nullptr;
            const auto list = seams.listDevices;
            seams.listDevices = [&, list]() -> QList<SharedDevice> {
                const auto devices = list();
                self->stop();           // the result path: right after the answer, on the worker
                return devices;         // the caller still gets a list to iterate
            };
            Manager manager(seams, fastOptions());
            self = &manager;
            QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
            QVERIFY2(waitUntil([&] { return manager.snapshot().state == State::Stopped; }), "never stopped");
            manager.stop();
            QCOMPARE(env->alive.load(), 0);
            QCOMPARE(env->startedCount(), 0);
        }
    }

    void stopFromASnapshotChangedHandlerIsSafe()
    {
        for (int i = 0; i < 200; ++i) {
            auto env = std::make_shared<Env>();
            Manager manager(fakeSeams(env), fastOptions());
            int seen = 0;
            connect(&manager, &Manager::snapshotChanged, this, [&] {
                // The first notification arrives inside start() itself; later ones queued.
                if (++seen == (i % 2 == 0 ? 1 : 3)) manager.stop();
            });
            QVERIFY(manager.start(config()));
            QVERIFY(waitUntil([&] { return manager.snapshot().state == State::Stopped; }));
            QTest::qWait(2);
            manager.stop();
            QCOMPARE(env->alive.load(), 0);
            QCOMPARE(manager.snapshot().state, State::Stopped);
        }
    }

    // ---- a device's own re-fetch -------------------------------------------

    void aDevicesFailedRefetchNeverDisturbsTheSessionOrStrandsTheDevice_data()
    {
        QTest::addColumn<bool>("throws");
        QTest::newRow("host unreachable") << true;
        QTest::newRow("host says unavailable") << false;
    }
    void aDevicesFailedRefetchNeverDisturbsTheSessionOrStrandsTheDevice()
    {
        QFETCH(bool, throws);
        auto env = std::make_shared<Env>();
        env->devices = [] {
            return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
        };
        env->act = [](int index) { return index == 0 ? Act::Forward : Act::Fail; };
        env->capability = [throws](int call) -> Capability {
            Capability c;
            if (call == 0) {
                c.available = true;
                c.reason = QStringLiteral("ready");
                c.port = kTunnelPort;
                c.token = kToken;
                return c;
            }
            if (throws) throw std::runtime_error("down");
            c.reason = QStringLiteral("unavailable");
            return c;
        };
        Manager manager(fakeSeams(env), fastOptions({20, 20}));
        QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
        bool sessionDisturbed = false;
        bool seenActive = false;
        bool firstDeviceDropped = false;
        const bool reached = waitUntil([&] {
            const Snapshot s = manager.snapshot();
            if (s.state == State::Active) seenActive = true;
            else if (seenActive) sessionDisturbed = true;
            if (s.state == State::Active && s.devices.size() == 2 && deviceState(s, "1-1") != DeviceState::Forwarding &&
                env->startedCount() >= 2) {
                firstDeviceDropped = true;
            }
            return deviceState(s, "1-2") == DeviceState::Failed;
        });
        QVERIFY2(reached, "the failing device was stranded");
        QVERIFY2(!sessionDisturbed, "a device's re-fetch changed the session state");
        QVERIFY(!firstDeviceDropped);
        QTest::qWait(300);
        const Snapshot s = manager.snapshot();
        QCOMPARE(s.state, State::Active);
        QCOMPARE(deviceState(s, "1-1"), DeviceState::Forwarding);
        QVERIFY2(s.status.contains(QStringLiteral("Forwarding")), qPrintable(s.status));
        for (const DeviceStatus &d : s.devices) {
            QVERIFY(d.state != DeviceState::Pending);       // nothing stuck waiting
        }
        QCOMPARE(env->fetches.load(), 3);                   // bounded: 1 + one per backoff entry
    }

    // ---- lifecycle --------------------------------------------------------

    void stopIsIdempotentTearsDownAndWorksFromAnyThread()
    {
        auto env = std::make_shared<Env>();
        env->devices = [] {
            return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
        };
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 2; }));
        QCOMPARE(env->alive.load(), 2);

        std::thread t1([&] { manager.stop(); });
        std::thread t2([&] { manager.stop(); });
        manager.stop();
        t1.join();
        t2.join();

        QCOMPARE(env->alive.load(), 0);        // every tunnel is gone
        QVERIFY(env->stops.load() >= 2);       // and was stopped, not just dropped
        const Snapshot after = manager.snapshot();
        QCOMPARE(after.state, State::Stopped);
        QVERIFY(after.devices.isEmpty());

        manager.stop();                        // again: nothing changes
        QCOMPARE(manager.snapshot().revision, after.revision);
        QVERIFY(!manager.toggle());            // a stopped session cannot be toggled
        const int started = env->startedCount();
        QTest::qWait(300);
        QCOMPARE(env->startedCount(), started); // nothing starts after stop
    }

    void stopDoesNotWaitForASlowCapabilityRequest()
    {
        auto env = std::make_shared<Env>();
        env->holdFetch = true;
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return env->fetches.load() == 1; }));
        QElapsedTimer timer;
        timer.start();
        manager.stop();
        QVERIFY2(timer.elapsed() < 1000, "stop() waited for the capability request");
        env->holdFetch = false; // the late answer arrives to a stopped session
        QTest::qWait(300);
        QCOMPARE(env->startedCount(), 0);
        QCOMPARE(manager.snapshot().state, State::Stopped);
    }

    void startReturnsWithoutWaitingForTheNetwork()
    {
        auto env = std::make_shared<Env>();
        env->holdFetch = true;
        Manager manager(fakeSeams(env), fastOptions());
        QElapsedTimer timer;
        timer.start();
        QVERIFY(manager.start(config()));
        QVERIFY2(timer.elapsed() < 500, "start() blocked");
        QCOMPARE(manager.snapshot().state, State::Checking);
        env->holdFetch = false;
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
    }

    void stopFromTheWorkerThreadDoesNotDeadlock()
    {
        auto env = std::make_shared<Env>();
        ManagerSeams seams = fakeSeams(env);
        std::atomic<bool> called{false};
        Manager *self = nullptr;
        seams.listDevices = [&, env]() -> QList<SharedDevice> {
            // Runs on the manager's worker thread.
            called = true;
            self->stop();
            return {};
        };
        Manager reentrant(seams, fastOptions());
        self = &reentrant;
        QVERIFY(reentrant.start(config()));
        QVERIFY(waitUntil([&] { return called.load() && reentrant.snapshot().state == State::Stopped; }));
        reentrant.stop(); // the thread is joined here
        QCOMPARE(reentrant.snapshot().state, State::Stopped);
    }

    void startingAgainReplacesTheRunningSession()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return env->startedCount() == 2 && manager.snapshot().forwardingCount() == 1; }));
        QCOMPARE(env->alive.load(), 1); // the first session's tunnel is gone
    }

    void snapshotChangedIsEmittedForEventLoopContexts()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        QSignalSpy spy(&manager, &Manager::snapshotChanged);
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
        QTRY_VERIFY(spy.count() >= 2);
    }

    void toggleFlipsForwardingForTheSession()
    {
        auto env = std::make_shared<Env>();
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));

        QVERIFY(manager.toggle()); // off
        QVERIFY(waitUntil([&] { return manager.snapshot().state == State::Paused; }));
        QCOMPARE(env->alive.load(), 0);
        QVERIFY(manager.snapshot().devices.isEmpty());
        QTest::qWait(200);
        QCOMPARE(env->startedCount(), 1); // paused means quiet

        QVERIFY(manager.toggle()); // on again: a fresh capability, a fresh tunnel
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
        QCOMPARE(env->fetches.load(), 2);
        QCOMPARE(env->startedCount(), 2);
        QCOMPARE(env->alive.load(), 1);
    }

    void pausingWhileTheCapabilityRequestIsInFlightDropsItsAnswer()
    {
        auto env = std::make_shared<Env>();
        env->holdFetch = true;
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return env->fetches.load() == 1; }));
        QVERIFY(manager.toggle());
        QVERIFY(waitUntil([&] { return manager.snapshot().state == State::Paused; }));
        env->holdFetch = false;
        QTest::qWait(300);
        QCOMPARE(env->startedCount(), 0); // the stale answer was ignored
        QCOMPARE(manager.snapshot().state, State::Paused);
    }

    // ---- failures ---------------------------------------------------------

    void failingTunnelBacksOffAndIsBounded()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int) { return Act::Fail; };
        const QList<int> backoff = {60, 120, 240};
        Manager manager(fakeSeams(env), fastOptions(backoff));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Failed; }));
        QTest::qWait(500);
        QCOMPARE(env->startedCount(), 4);  // the first try plus one per backoff entry, then stop
        const QList<qint64> times = env->startTimes();
        for (int i = 1; i < times.size(); ++i) {
            // Never earlier than the configured delay (timers may be a few ms early).
            QVERIFY2(times.at(i) - times.at(i - 1) >= backoff.at(i - 1) * 0.8,
                     qPrintable(QStringLiteral("retry %1 came after %2 ms").arg(i).arg(times.at(i) - times.at(i - 1))));
        }
        QCOMPARE(manager.snapshot().state, State::Active); // the session itself is fine
        QVERIFY(!manager.snapshot().status.isEmpty());
    }

    void retryFetchesAFreshCapabilityAndUsesItsToken()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int index) { return index == 0 ? Act::Fail : Act::Forward; };
        env->capability = [](int call) {
            Capability c;
            c.available = true;
            c.reason = QStringLiteral("ready");
            c.port = kTunnelPort;
            c.token = call == 0 ? kToken : kToken2;
            return c;
        };
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }));
        QCOMPARE(env->fetches.load(), 2);
        QCOMPARE(env->startedConfig(0).sessionToken, kToken);
        QCOMPARE(env->startedConfig(1).sessionToken, kToken2);
    }

    void aDeviceThatConnectsAndDropsAtOnceCannotRetryForever()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int) { return Act::ForwardThenFail; };
        env->afterMs = 5;
        Manager manager(fakeSeams(env), fastOptions({20, 20, 20})); // stableAfterMs is 10 s
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Failed; }));
        QTest::qWait(400);
        QCOMPARE(env->startedCount(), 4);
    }

    void aTunnelThatStayedUpEarnsAFreshRetryBudget()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int index) {
            if (index == 0) return Act::Fail;               // attempt 1 of the budget
            if (index == 1) return Act::ForwardThenFail;    // up for a while, then lost
            return Act::Fail;
        };
        env->afterMs = 80;
        ManagerOptions options = fastOptions({20, 20});
        options.stableAfterMs = 20;
        Manager manager(fakeSeams(env), options);
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Failed; }));
        QTest::qWait(300);
        QCOMPARE(env->startedCount(), 4); // 3 without the reset
    }

    void hostClosingTheTunnelCleanlyIsFinal()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int) { return Act::ForwardThenClose; };
        env->afterMs = 30;
        Manager manager(fakeSeams(env), fastOptions());
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Ended; }));
        QTest::qWait(400);
        QCOMPARE(env->startedCount(), 1); // revoked or finished by the host: not retried
        QCOMPARE(env->fetches.load(), 1);
    }

    // ---- snapshot ---------------------------------------------------------

    void snapshotCanBePolledFromAnotherThreadWhileStateChanges()
    {
        auto env = std::make_shared<Env>();
        env->act = [](int index) { return index % 3 == 0 ? Act::ForwardThenFail : Act::Forward; };
        env->afterMs = 10;
        env->devices = [] {
            return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}, {QStringLiteral("0781:5581"), "1-2"}};
        };
        Manager manager(fakeSeams(env), fastOptions({5, 5, 5, 5}));

        std::atomic<bool> stopPolling{false};
        std::atomic<long> polls{0};
        std::atomic<bool> inconsistent{false};
        std::thread poller([&] {
            quint64 lastRevision = 0;
            while (!stopPolling) {
                const Snapshot s = manager.snapshot();
                ++polls;
                if (s.revision < lastRevision) inconsistent = true;      // never goes backwards
                lastRevision = s.revision;
                if (s.status.isEmpty()) inconsistent = true;             // always readable text
                // Devices exist only while the host said yes; counts agree with states.
                if (s.state != State::Active && !s.devices.isEmpty()) inconsistent = true;
                int forwarding = 0;
                for (const DeviceStatus &d : s.devices) forwarding += d.state == DeviceState::Forwarding;
                if (forwarding != s.forwardingCount()) inconsistent = true;
            }
        });

        QVERIFY(manager.start(config({QStringLiteral("054c:0ce6"), QStringLiteral("0781:5581")})));
        for (int i = 0; i < 12; ++i) {
            QTest::qWait(15);
            manager.toggle(); // pause, resume, pause...
        }
        QTest::qWait(100);
        manager.stop();
        stopPolling = true;
        poller.join();
        QVERIFY(polls.load() > 100);
        QVERIFY2(!inconsistent.load(), "snapshot was inconsistent while polled concurrently");
        QCOMPARE(manager.snapshot().state, State::Stopped);
    }

    // ---- logging ----------------------------------------------------------

    void theTokenAndTheCapabilityAreNeverLogged()
    {
        auto env = std::make_shared<Env>();
        // The first request fails with the token in the exception text.
        env->capability = [](int call) -> Capability {
            if (call == 0) {
                throw std::runtime_error(std::string("failed with ") + kToken.constData());
            }
            Capability c;
            c.available = true;
            c.reason = QStringLiteral("ready");
            c.port = kTunnelPort;
            c.token = kToken;
            return c;
        };
        ManagerSeams seams = fakeSeams(env);
        // A tunnel that fails with a message that (wrongly) contains the token.
        class LeakyTunnel final : public TunnelHandle
        {
        public:
            bool start(QString *) override
            {
                QTimer::singleShot(0, this, [this] {
                    emit finished(QStringLiteral("rejected token ") + QString::fromLatin1(kToken));
                });
                return true;
            }
            void stop() noexcept override {}
        };
        seams.createTunnel = [](const TunnelConfig &) -> std::unique_ptr<TunnelHandle> {
            return std::make_unique<LeakyTunnel>();
        };
        Manager manager(seams, fastOptions({20, 20}));
        QVERIFY(manager.start(config()));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Failed; }));
        manager.stop();
        bool sawRedaction = false;
        for (const QString &line : logLines()) {
            QVERIFY2(!line.contains(QString::fromLatin1(kToken)), qPrintable(line));
            sawRedaction = sawRedaction || line.contains(QStringLiteral("[redacted]"));
        }
        QVERIFY(sawRedaction); // the failure was logged, with the secret removed
        // The user-visible status never carries free text either.
        QVERIFY(!manager.snapshot().status.contains(QString::fromLatin1(kToken)));
    }

    // ---- end to end with the real tunnel ---------------------------------

    void realTunnelThroughTheManagerReachesThePinnedHostOnly()
    {
        FakeTlsServer host(b); // the paired host presents certificate b
        QVERIFY(host.listen());
        QTcpServer local;     // stands in for usbipd-win
        QVERIFY(local.listen(QHostAddress::LocalHost, 0));
        bool replied = false;
        host.onData = [&](QSslSocket *socket, const QByteArray &all) {
            if (!replied && all.contains('\n')) {
                replied = true;
                socket->write("{\"op\":\"ready\"}\n");
            }
        };
        auto env = std::make_shared<Env>();
        env->capability = [&host](int) {
            Capability c;
            c.available = true;
            c.reason = QStringLiteral("ready");
            c.port = host.port();
            c.token = kToken;
            return c;
        };
        ManagerSeams seams = fakeSeams(env);
        seams.createTunnel = nullptr; // use the real tunnel
        ManagerOptions options = fastOptions();
        options.localUsbipPort = local.serverPort();
        Manager manager(seams, options);
        ManagerConfig c = config();
        c.streamAddress = c.pairedAddress = QStringLiteral("127.0.0.1");
        QVERIFY(manager.start(c));
        QVERIFY(waitUntil([&] { return manager.snapshot().forwardingCount() == 1; }, 10000));
        const QJsonObject hello = QJsonDocument::fromJson(host.received().left(host.received().indexOf('\n'))).object();
        QCOMPARE(hello.value(QStringLiteral("op")).toString(), QStringLiteral("forward"));
        QCOMPARE(hello.value(QStringLiteral("busid")).toString(), QStringLiteral("1-1"));
        QCOMPARE(hello.value(QStringLiteral("token")).toString(), QString::fromLatin1(kToken));
        manager.stop();
    }

    void realTunnelToAnImpostorHostNeverSendsTheToken()
    {
        FakeTlsServer impostor(a); // presents certificate a, but the pin is b
        QVERIFY(impostor.listen());
        QTcpServer local;
        QVERIFY(local.listen(QHostAddress::LocalHost, 0));
        auto env = std::make_shared<Env>();
        env->capability = [&impostor](int) {
            Capability c;
            c.available = true;
            c.reason = QStringLiteral("ready");
            c.port = impostor.port();
            c.token = kToken;
            return c;
        };
        ManagerSeams seams = fakeSeams(env);
        seams.createTunnel = nullptr;
        ManagerOptions options = fastOptions({20});
        options.localUsbipPort = local.serverPort();
        Manager manager(seams, options);
        ManagerConfig c = config();
        c.streamAddress = c.pairedAddress = QStringLiteral("127.0.0.1");
        QVERIFY(manager.start(c));
        QVERIFY(waitUntil([&] { return deviceState(manager.snapshot(), "1-1") == DeviceState::Failed; }, 15000));
        QVERIFY2(!impostor.received().contains(kToken), "token reached a host that is not the pinned one");
        QCOMPARE(manager.snapshot().forwardingCount(), 0);
        manager.stop();
    }

    // A device check that is stuck (usbipd hanging) must never hold up stop(),
    // and its answer, when it finally comes, must be dropped.
    void slowDeviceListNeverDelaysStopAndLateAnswerIsDropped()
    {
        for (int round = 0; round < 100; ++round) {
            auto env = std::make_shared<Env>();
            std::mutex gateMutex;
            std::condition_variable gate;
            bool release = false;
            std::atomic<bool> entered{false};
            std::atomic<bool> finishedLate{false};
            std::atomic<bool> abandoned{false};
            ManagerSeams seams = fakeSeams(env);
            seams.listDevices = [&] {
                entered = true;
                {
                    std::unique_lock<std::mutex> lock(gateMutex);
                    // "Blocks for several seconds" unless the test lets it go.
                    gate.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
                }
                finishedLate = true;
                return QList<SharedDevice>{{QStringLiteral("054c:0ce6"), "1-1"}};
            };
            seams.abandonListing = [&] { abandoned = true; };
            Manager manager(seams, fastOptions());
            QVERIFY(manager.start(config()));
            QVERIFY(waitUntil([&] { return entered.load(); }, 5000));

            QElapsedTimer stopTimer;
            stopTimer.start();
            manager.stop();
            const qint64 elapsed = stopTimer.elapsed();
            QVERIFY2(elapsed < 200, qPrintable(QStringLiteral("stop() took %1 ms (round %2)").arg(elapsed).arg(round)));
            QVERIFY(abandoned.load());
            QVERIFY(!finishedLate.load());   // the lister really was still stuck when stop() returned

            {
                std::lock_guard<std::mutex> lock(gateMutex);
                release = true;
            }
            gate.notify_all();
            QVERIFY(waitUntil([&] { return finishedLate.load(); }, 2000));
            QTest::qWait(10);                 // give a (wrongly) delivered answer time to arrive
            QCOMPARE(env->startedCount(), 0); // dropped: no tunnel was started for it
            QCOMPARE(manager.snapshot().state, State::Stopped);
        }
    }
};

QTEST_GUILESS_MAIN(ManagerTest)
#include "main.moc"
