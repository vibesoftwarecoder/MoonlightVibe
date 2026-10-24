// How the client decides which usbipd.exe to trust (and run elevated).
// Windows only; the sharing stack itself is never started or touched.
// Derived from qiin2333/moonlight-qt (GPL-3.0).
#include "../../app/backend/usbforwardingenvironment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using Env = UsbForwardingEnvironment;

class EnvironmentTest : public QObject
{
    Q_OBJECT

    static bool writeFakeExe(const QString &path)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write("MZ not a real program");
        return true;
    }

private slots:
    void readinessErrors()
    {
        QVERIFY(Env::readinessError(Env::Ready).isEmpty());
        for (auto state : { Env::Checking, Env::NotInstalled, Env::ServiceStopped,
                            Env::DriverStopped, Env::CheckFailed, Env::Unsupported }) {
            QVERIFY2(!Env::readinessError(state).isEmpty(), "every non-ready state explains itself");
        }
        QVERIFY(Env::readinessError(Env::DriverStopped) != Env::readinessError(Env::ServiceStopped));
        QVERIFY(!Env::readinessError(Env::Unsupported).contains(QLatin1String("Sunshine")));
    }

    void serviceImagePathParsing_data()
    {
        QTest::addColumn<QString>("imagePath");
        QTest::addColumn<QString>("expected");
        const QString ok = QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
        QTest::newRow("quoted with args") << QStringLiteral("\"C:\\Program Files\\usbipd-win\\usbipd.exe\" server") << ok;
        QTest::newRow("quoted bare") << QStringLiteral("\"C:\\Program Files\\usbipd-win\\usbipd.exe\"") << ok;
        QTest::newRow("unquoted with spaces") << QStringLiteral("C:\\Program Files\\usbipd-win\\usbipd.exe server") << ok;
        QTest::newRow("unquoted bare") << QStringLiteral("C:\\Program Files\\usbipd-win\\usbipd.exe") << ok;
        QTest::newRow("forward slashes") << QStringLiteral("\"C:/Program Files/usbipd-win/usbipd.exe\"") << ok;
        QTest::newRow("padded") << QStringLiteral("  \"C:\\Program Files\\usbipd-win\\usbipd.exe\"  ") << ok;
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("bare name") << QStringLiteral("usbipd.exe server") << QString();
        QTest::newRow("relative") << QStringLiteral("\"tools\\usbipd.exe\"") << QString();
        QTest::newRow("drive relative") << QStringLiteral("\"C:usbipd.exe\"") << QString();
        QTest::newRow("unc") << QStringLiteral("\"\\\\server\\share\\usbipd.exe\"") << QString();
        QTest::newRow("device path") << QStringLiteral("\"\\\\?\\C:\\x\\usbipd.exe\"") << QString();
        QTest::newRow("env variable") << QStringLiteral("\"%ProgramFiles%\\usbipd-win\\usbipd.exe\"") << QString();
        QTest::newRow("unterminated quote") << QStringLiteral("\"C:\\Program Files\\usbipd.exe") << QString();
        QTest::newRow("no exe") << QStringLiteral("C:\\Windows\\system32\\svchost.dll -k x") << QString();
        QTest::newRow("exe not word end") << QStringLiteral("C:\\x\\usbipd.exetra.dll") << QString();
    }
    void serviceImagePathParsing()
    {
        QFETCH(QString, imagePath);
        QFETCH(QString, expected);
        QCOMPARE(Env::parseServiceBinaryPath(imagePath), expected);
    }

    void trustRejectsBadPaths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("reasonPart");
        QTest::newRow("empty") << QString() << QStringLiteral("empty");
        QTest::newRow("bare name") << QStringLiteral("usbipd.exe") << QStringLiteral("absolute");
        QTest::newRow("dot relative") << QStringLiteral("./usbipd.exe") << QStringLiteral("absolute");
        QTest::newRow("unc") << QStringLiteral("//server/share/usbipd.exe") << QStringLiteral("absolute");
        QTest::newRow("parent hop") << QStringLiteral("C:/Windows/System32/../System32/cmd.exe")
                                    << QStringLiteral("parent");
        QTest::newRow("wrong name") << QStringLiteral("C:/Windows/System32/cmd.exe")
                                    << QStringLiteral("expected one");
        QTest::newRow("env variable") << QStringLiteral("%ProgramFiles%/usbipd-win/usbipd.exe")
                                      << QStringLiteral("unsafe");
        QTest::newRow("missing file") << QStringLiteral("C:/Program Files/no-such-dir-usbfwd/usbipd.exe")
                                      << QStringLiteral("does not exist");
    }
    void trustRejectsBadPaths()
    {
        QFETCH(QString, path);
        QFETCH(QString, reasonPart);
        QString reason;
        QVERIFY2(!Env::isTrustedUsbipdPath(path, &reason), qPrintable(path));
        QVERIFY2(reason.contains(reasonPart), qPrintable(reason));
    }

    void trustRejectsUserWritableLocation()
    {
        // A perfectly named file in a directory the current user owns: this is
        // what a planted usbipd.exe looks like.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fake = dir.filePath(QStringLiteral("usbipd.exe"));
        QVERIFY(writeFakeExe(fake));
        QString reason;
        QVERIFY(!Env::isTrustedUsbipdPath(fake, &reason));
        QVERIFY2(reason.contains(QLatin1String("writable")), qPrintable(reason));
        QVERIFY(Env::isWritableByStandardUsers(fake));
    }

    void writabilityCheck()
    {
        QVERIFY(!Env::isWritableByStandardUsers(QStringLiteral("C:/Windows/System32/cmd.exe")));
        QVERIFY(!Env::isWritableByStandardUsers(QStringLiteral("C:/Program Files")));
        // Fails closed: a path we cannot inspect counts as writable.
        QVERIFY(Env::isWritableByStandardUsers(QStringLiteral("C:/does/not/exist/usbipd.exe")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(Env::isWritableByStandardUsers(dir.path()));
    }

    void installedUsbipdIsAcceptedWhenPresent()
    {
        const QString installed = QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
        if (!QFileInfo::exists(installed)) {
            QSKIP("usbipd-win is not installed here; the positive case cannot be exercised");
        }
        QString reason;
        QVERIFY2(Env::isTrustedUsbipdPath(installed, &reason), qPrintable(reason));
        QCOMPARE(Env::locateUsbipd().toLower(), installed.toLower());
    }

    void candidatesNeverComeFromPathCwdOrEnvironment()
    {
        // Plant files where they WOULD pass every check (the predicate below
        // trusts everything), first on PATH, as the cwd, and via the Program
        // Files variables. The lookup must not even list them.
        QTemporaryDir planted;
        QVERIFY(planted.isValid());
        QVERIFY(writeFakeExe(planted.filePath(QStringLiteral("usbipd.exe"))));
        QVERIFY(writeFakeExe(planted.filePath(QStringLiteral("usbipd-win/usbipd.exe"))));
        const QString plantedDir = QDir::fromNativeSeparators(planted.path());

        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldPf = qgetenv("ProgramFiles");
        const QByteArray oldPf6432 = qgetenv("ProgramW6432");
        const QByteArray oldPfx86 = qgetenv("ProgramFiles(x86)");
        const QString oldCwd = QDir::currentPath();
        const QByteArray native = QDir::toNativeSeparators(planted.path()).toLocal8Bit();
        qputenv("PATH", native + ";" + oldPath);
        qputenv("ProgramFiles", native);
        qputenv("ProgramW6432", native);
        qputenv("ProgramFiles(x86)", native);
        QVERIFY(QDir::setCurrent(planted.path()));

        const QStringList candidates = Env::candidateUsbipdPaths();
        const QString chosen = Env::firstAccepted(candidates, [](const QString &) { return true; });

        QDir::setCurrent(oldCwd);
        qputenv("PATH", oldPath);
        qputenv("ProgramFiles", oldPf);
        qputenv("ProgramW6432", oldPf6432);
        qputenv("ProgramFiles(x86)", oldPfx86);

        for (const QString &candidate : candidates) {
            QVERIFY2(!QDir::fromNativeSeparators(candidate).startsWith(plantedDir, Qt::CaseInsensitive),
                     qPrintable("planted path became a candidate: " + candidate));
            QVERIFY2(QFileInfo(candidate).fileName().compare(QStringLiteral("usbipd.exe"), Qt::CaseInsensitive) == 0,
                     qPrintable(candidate));
        }
        QVERIFY2(!QDir::fromNativeSeparators(chosen).startsWith(plantedDir, Qt::CaseInsensitive),
                 qPrintable("planted usbipd.exe was chosen: " + chosen));
        // The seam itself works: with a predicate that accepts, the first
        // candidate (if there is one) is returned.
        QCOMPARE(Env::firstAccepted({ QStringLiteral("C:/a/usbipd.exe"), QStringLiteral("C:/b/usbipd.exe") },
                                    [](const QString &) { return true; }),
                 QStringLiteral("C:/a/usbipd.exe"));
        QCOMPARE(Env::firstAccepted({ QStringLiteral("C:/a/usbipd.exe"), QStringLiteral("C:/b/usbipd.exe") },
                                    [](const QString &p) { return p.contains(QLatin1String("/b/")); }),
                 QStringLiteral("C:/b/usbipd.exe"));
    }

    void standardUserTokenJudgesRealSystemFiles()
    {
        // Owned and protected by the system: a normal user cannot change them.
        QVERIFY(!Env::isWritableByStandardUsers(QStringLiteral("C:/Windows/System32/notepad.exe")));
        QVERIFY(!Env::isWritableByStandardUsers(QStringLiteral("C:/Windows/System32")));
        // Writable through a group grant rather than ownership.
        QVERIFY(Env::isWritableByStandardUsers(QStringLiteral("C:/Windows/Temp")));
        // A file the current user just created.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mine = dir.filePath(QStringLiteral("mine.exe"));
        QVERIFY(writeFakeExe(mine));
        QVERIFY(Env::isWritableByStandardUsers(mine));
        QVERIFY(Env::isWritableByStandardUsers(dir.path()));
        // The drive root is judged too: adding folders there is normal and
        // must not condemn everything below it.
        QVERIFY(!Env::isWritableByStandardUsers(QStringLiteral("C:/Windows/System32/cmd.exe")));
    }

    void realSystemFilePassesEveryCheckExceptTheName()
    {
        // The positive case, run against a real protected file because
        // usbipd-win is not installed here.
        const QString notepad = QStringLiteral("C:/Windows/System32/notepad.exe");
        QString reason;
        QVERIFY2(Env::isTrustedExecutablePath(notepad, QStringLiteral("notepad.exe"), &reason), qPrintable(reason));
        QVERIFY(!Env::isTrustedExecutablePath(notepad, QStringLiteral("usbipd.exe"), &reason));
        // A file of the right name the current user can change is refused.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mine = dir.filePath(QStringLiteral("notepad.exe"));
        QVERIFY(writeFakeExe(mine));
        QVERIFY(!Env::isTrustedExecutablePath(mine, QStringLiteral("notepad.exe"), &reason));
        QVERIFY2(reason.contains(QLatin1String("writable")), qPrintable(reason));
    }

    void adminOnlyLocationIsTrustedUntilAGroupGetsWriteAccess()
    {
        // The real positive case: a usbipd.exe in a directory only SYSTEM and
        // Administrators can change. It lives directly under the drive root so
        // that no user-writable ancestor (like %TEMP%) hides the result. Needs
        // an elevated test run to create it there; otherwise it is skipped.
        const QString target = QStringLiteral("C:/usbfwd-acltest-") + QString::number(QCoreApplication::applicationPid());
        struct Cleanup {
            QString dir;
            ~Cleanup() { QDir(dir).removeRecursively(); }
        } cleanup { target };
        if (!QDir().mkpath(target)) {
            QSKIP("cannot create a directory under the drive root");
        }
        const QString file = target + QStringLiteral("/usbipd.exe");
        const QString native = QDir::toNativeSeparators(target);
        if (QProcess::execute(QStringLiteral("icacls.exe"),
                              { native, QStringLiteral("/inheritance:r"), QStringLiteral("/grant:r"),
                                QStringLiteral("*S-1-5-18:(OI)(CI)F"), QStringLiteral("*S-1-5-32-544:(OI)(CI)F"),
                                QStringLiteral("*S-1-5-32-545:(OI)(CI)RX"), QStringLiteral("*S-1-1-0:(OI)(CI)RX"), QStringLiteral("*S-1-5-11:(OI)(CI)RX"),
                                qEnvironmentVariable("USERDOMAIN") + QLatin1Char(0x5C) + qEnvironmentVariable("USERNAME") + QStringLiteral(":(OI)(CI)RX"),
                                QStringLiteral("/C"), QStringLiteral("/Q") }) != 0) {
            QSKIP("icacls could not restrict the test directory");
        }
        // Create the file only now, so it inherits the restricted ACL. Doing it
        // first and then running "icacls /inheritance:r /T" strips the file's
        // inherited entries and leaves it with an empty ACL nobody can open.
        QVERIFY(writeFakeExe(file));
        if (Env::isWritableByStandardUsers(file)) {
            // The current user owns the file (a non-elevated account), so ownership
            // alone grants control and the group case cannot be isolated here.
            QSKIP("the test file is owned by the current user; run this test elevated");
        }
        QString reason;
        QVERIFY2(Env::isTrustedUsbipdPath(file, &reason), qPrintable(reason));

        // "This Organization" (S-1-5-15) is in the token but in no fixed list.
        QVERIFY(QProcess::execute(QStringLiteral("icacls.exe"),
                                  { native, QStringLiteral("/grant"), QStringLiteral("*S-1-5-15:(OI)(CI)M"),
                                    QStringLiteral("/T"), QStringLiteral("/C"), QStringLiteral("/Q") }) == 0);
        QVERIFY2(Env::isWritableByStandardUsers(file), "group grant was not noticed");
        QVERIFY(!Env::isTrustedUsbipdPath(file, &reason));
        QVERIFY2(reason.contains(QLatin1String("writable")), qPrintable(reason));
    }

    void linkedPathIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = dir.filePath(QStringLiteral("real"));
        QVERIFY(writeFakeExe(real + QStringLiteral("/usbipd.exe")));
        const QString junction = dir.filePath(QStringLiteral("link"));
        if (QProcess::execute(QStringLiteral("cmd.exe"),
                              { QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                QDir::toNativeSeparators(junction), QDir::toNativeSeparators(real) }) != 0) {
            QSKIP("could not create a junction");
        }
        QString reason;
        const bool trusted = Env::isTrustedUsbipdPath(junction + QStringLiteral("/usbipd.exe"), &reason);
        // Remove the junction itself (not its target) before the directory is deleted.
        QProcess::execute(QStringLiteral("cmd.exe"),
                          { QStringLiteral("/c"), QStringLiteral("rmdir"), QDir::toNativeSeparators(junction) });
        QVERIFY(!trusted);
        QVERIFY2(reason.contains(QLatin1String("link")), qPrintable(reason));
    }

    void hungVersionProbeIsACheckFailureNotAStoppedService()
    {
        Env *env = Env::get();
        env->probeWith(QStringLiteral("C:/Windows/System32/PING.EXE"),
                       { QStringLiteral("-n"), QStringLiteral("30"), QStringLiteral("127.0.0.1") }, 500);
        QVERIFY(env->checking());
        QTRY_VERIFY_WITH_TIMEOUT(!env->checking(), 10000);
        QCOMPARE(env->state(), Env::CheckFailed);
    }

    void applicationQuitStopsARunningProbe()
    {
        Env *env = Env::get();
        env->probeWith(QStringLiteral("C:/Windows/System32/PING.EXE"),
                       { QStringLiteral("-n"), QStringLiteral("60"), QStringLiteral("127.0.0.1") }, 60000);
        QVERIFY(env->checking());
        QCOMPARE(env->findChildren<QProcess *>().size(), 1);
        QElapsedTimer timer;
        timer.start();
        QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");   // a private signal: call it by name
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!env->checking());
        QCOMPARE(env->findChildren<QProcess *>().size(), 0);
        QVERIFY(env->state() != Env::Checking);
    }

    void failingVersionProbeMeansStoppedService()
    {
        Env *env = Env::get();
        env->probeWith(QStringLiteral("C:/Windows/System32/cmd.exe"),
                       { QStringLiteral("/c"), QStringLiteral("exit"), QStringLiteral("3") }, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!env->checking(), 10000);
        QCOMPARE(env->state(), Env::ServiceStopped);
        env->probeWith(QStringLiteral("C:/no/such/dir/usbipd.exe"), {}, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!env->checking(), 10000);
        QCOMPARE(env->state(), Env::ServiceStopped);
    }

    void locateNeverConsultsPathCwdOrEnvironment()
    {
        QTemporaryDir planted;
        QVERIFY(planted.isValid());
        const QString fake = planted.filePath(QStringLiteral("usbipd.exe"));
        const QString fakeInProgramFiles = planted.filePath(QStringLiteral("usbipd-win/usbipd.exe"));
        QVERIFY(writeFakeExe(fake));
        QVERIFY(writeFakeExe(fakeInProgramFiles));

        // Plant it first on PATH, make it the working directory, and point the
        // Program Files variables at the same directory.
        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldPf = qgetenv("ProgramFiles");
        const QByteArray oldPf6432 = qgetenv("ProgramW6432");
        const QString oldCwd = QDir::currentPath();
        qputenv("PATH", QDir::toNativeSeparators(planted.path()).toLocal8Bit() + ";" + oldPath);
        qputenv("ProgramFiles", QDir::toNativeSeparators(planted.path()).toLocal8Bit());
        qputenv("ProgramW6432", QDir::toNativeSeparators(planted.path()).toLocal8Bit());
        QVERIFY(QDir::setCurrent(planted.path()));

        const QString located = Env::locateUsbipd();

        QDir::setCurrent(oldCwd);
        qputenv("PATH", oldPath);
        qputenv("ProgramFiles", oldPf);
        qputenv("ProgramW6432", oldPf6432);

        QVERIFY2(!located.startsWith(QDir::fromNativeSeparators(planted.path()), Qt::CaseInsensitive),
                 qPrintable("planted usbipd.exe was chosen: " + located));
        if (!located.isEmpty()) {
            QString reason;
            QVERIFY2(Env::isTrustedUsbipdPath(located, &reason), qPrintable(reason));
        }
    }
};

QTEST_MAIN(EnvironmentTest)
#include "main.moc"
