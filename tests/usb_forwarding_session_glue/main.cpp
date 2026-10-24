// What a streaming session does with the USB forwarding preferences: nothing
// when the feature is off or unsupported, a refusal when the inputs are not
// safe, and one canonical address for both sides when they are.
#include "../../app/backend/usbforwardingsessionglue.h"
#include "../usb_forwarding_common/faketlsserver.h"

#include <QtTest>

using namespace UsbForwarding;

namespace {

QStringList g_Messages;

void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_Messages.append(message);
}

}

class SessionGlueTest : public QObject
{
    Q_OBJECT

    TestIdentity m_Server;
    TestIdentity m_Client;

    SessionInputs valid() const
    {
        SessionInputs in;
        in.platformSupported = true;
        in.enabled = true;
        in.autoDevices = { QStringLiteral("054c:0ce6"), QStringLiteral("1234:5678#abc") };
        in.streamAddress = QStringLiteral("10.0.0.5");
        in.pairedAddress = QStringLiteral("10.0.0.5");
        in.pinnedServerCertificate = m_Server.certificate;
        in.clientSsl = QSslConfiguration::defaultConfiguration();
        in.clientSsl.setLocalCertificate(m_Client.certificate);
        in.clientSsl.setPrivateKey(m_Client.key);
        return in;
    }

    static void verifyUntouched(const SessionPlan &plan)
    {
        QVERIFY(plan.reason.isEmpty());
        QVERIFY(!plan.config.enabled);
        QVERIFY(plan.config.streamAddress.isEmpty());
        QVERIFY(plan.config.pairedAddress.isEmpty());
        QVERIFY(plan.config.autoDevices.isEmpty());
        QVERIFY(plan.config.pinnedServerCertificate.isNull());
    }

private slots:
    void initTestCase()
    {
        m_Server = loadTestIdentity(QStringLiteral("a"));
        m_Client = loadTestIdentity(QStringLiteral("b"));
        if (!m_Server.valid() || !m_Client.valid()) {
            QSKIP("test certificates are not available (USBFWD_TEST_CERTS)");
        }
    }

    void validInputsStart()
    {
        const SessionPlan plan = planSession(valid());
        QCOMPARE(plan.kind, PlanKind::Start);
        QVERIFY(plan.reason.isEmpty());
    }

    void otherPlatformsDoNothing()
    {
        SessionInputs in = valid();
        in.platformSupported = false;
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::NotApplicable);
        verifyUntouched(plan);
    }

    void disabledDoesNothing()
    {
        SessionInputs in = valid();
        in.enabled = false;
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Disabled);
        verifyUntouched(plan);
    }

    void disabledStaysQuietEvenWithBrokenInputs()
    {
        // The default (off) must never complain about a host it will not use.
        SessionInputs in;
        in.platformSupported = true;
        in.enabled = false;
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Disabled);
        verifyUntouched(plan);
    }

    void missingPinnedCertificateIsRefused()
    {
        SessionInputs in = valid();
        in.pinnedServerCertificate = QSslCertificate();
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Refused);
        QVERIFY(!plan.reason.isEmpty());
        QVERIFY(!plan.config.enabled);
        QVERIFY(plan.config.pinnedServerCertificate.isNull());
    }

    void missingClientIdentityIsRefused_data()
    {
        QTest::addColumn<bool>("dropCertificate");
        QTest::addColumn<bool>("dropKey");
        QTest::newRow("no certificate") << true << false;
        QTest::newRow("no key") << false << true;
        QTest::newRow("neither") << true << true;
    }
    void missingClientIdentityIsRefused()
    {
        QFETCH(bool, dropCertificate);
        QFETCH(bool, dropKey);
        SessionInputs in = valid();
        if (dropCertificate) in.clientSsl.setLocalCertificate(QSslCertificate());
        if (dropKey) in.clientSsl.setPrivateKey(QSslKey());
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Refused);
        QVERIFY(!plan.reason.isEmpty());
    }

    void missingOrWrongAddressIsRefused_data()
    {
        QTest::addColumn<QString>("stream");
        QTest::addColumn<QString>("paired");
        QTest::newRow("no stream address") << "" << "10.0.0.5";
        QTest::newRow("no paired address") << "10.0.0.5" << "";
        QTest::newRow("neither") << "" << "";
        QTest::newRow("stream has a port") << "10.0.0.5:47989" << "10.0.0.5";
        QTest::newRow("stream has a scheme") << "https://10.0.0.5" << "10.0.0.5";
        QTest::newRow("stream has spaces") << "10.0.0.5 " << "10.0.0.5";
        QTest::newRow("different hosts") << "10.0.0.5" << "10.0.0.6";
        QTest::newRow("name vs its ip") << "pc.local" << "10.0.0.5";
        QTest::newRow("other IPv6 scope") << "fe80::1%12" << "fe80::1%13";
        QTest::newRow("scoped vs unscoped") << "fe80::1%12" << "fe80::1";
    }
    void missingOrWrongAddressIsRefused()
    {
        QFETCH(QString, stream);
        QFETCH(QString, paired);
        SessionInputs in = valid();
        in.streamAddress = stream;
        in.pairedAddress = paired;
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Refused);
        QVERIFY(!plan.reason.isEmpty());
        QVERIFY(!plan.config.enabled);
        QVERIFY(plan.config.streamAddress.isEmpty());
    }

    void equivalentSpellingsGiveOneCanonicalAddress_data()
    {
        QTest::addColumn<QString>("stream");
        QTest::addColumn<QString>("paired");
        QTest::addColumn<QString>("expected");
        QTest::newRow("same ipv4") << "10.0.0.5" << "10.0.0.5" << "10.0.0.5";
        QTest::newRow("bracketed ipv6") << "[2001:db8::1]" << "2001:db8::1" << "2001:db8::1";
        QTest::newRow("ipv6 case and zeros") << "2001:DB8:0:0:0:0:0:1" << "[2001:db8::1]" << "2001:db8::1";
        QTest::newRow("trailing dot") << "pc.local." << "pc.local" << "pc.local";
        QTest::newRow("case") << "PC.Local" << "pc.LOCAL" << "pc.local";
        QTest::newRow("dot and case") << "PC.LOCAL." << "pc.local" << "pc.local";
        QTest::newRow("scoped ipv6 kept") << "[fe80::1%12]" << "fe80::1%12" << "fe80::1%12";
    }
    void equivalentSpellingsGiveOneCanonicalAddress()
    {
        QFETCH(QString, stream);
        QFETCH(QString, paired);
        QFETCH(QString, expected);
        SessionInputs in = valid();
        in.streamAddress = stream;
        in.pairedAddress = paired;
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Start);
        QCOMPARE(plan.config.streamAddress, expected);
        QCOMPARE(plan.config.pairedAddress, expected);
        // The manager's own validation must accept exactly what the glue built.
        QString why;
        QVERIFY2(plan.config.validate(&why), qPrintable(why));
    }

    void configCarriesPinnedCertificateAndClientIdentity()
    {
        const SessionInputs in = valid();
        const SessionPlan plan = planSession(in);
        QCOMPARE(plan.kind, PlanKind::Start);
        QVERIFY(plan.config.enabled);
        QCOMPARE(plan.config.autoDevices, in.autoDevices);
        QCOMPARE(plan.config.pinnedServerCertificate.toDer(), m_Server.certificate.toDer());
        QCOMPARE(plan.config.clientSsl.localCertificate().toDer(), m_Client.certificate.toDer());
        QCOMPARE(plan.config.clientSsl.privateKey().toDer(), m_Client.key.toDer());
        // The pinned certificate is the server's, never the client's.
        QVERIFY(plan.config.pinnedServerCertificate.toDer() != m_Client.certificate.toDer());
    }

    void noSecretReachesTheLogOrTheReason()
    {
        g_Messages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(captureMessages);

        QStringList reasons;
        {
            SessionInputs a = valid(); a.pinnedServerCertificate = QSslCertificate();
            SessionInputs b = valid(); b.clientSsl.setPrivateKey(QSslKey());
            SessionInputs c = valid(); c.streamAddress = QStringLiteral("10.9.9.9");
            SessionInputs d = valid(); d.streamAddress.clear();
            for (const SessionInputs &in : { a, b, c, d }) {
                reasons.append(planSession(in).reason);
            }
            planSession(valid());
        }
        qInstallMessageHandler(previous);

        QVERIFY(g_Messages.isEmpty());   // the glue itself writes no log at all
        const QByteArray keyBody = m_Client.key.toPem().split('\n').value(1).trimmed();
        const QByteArray certBody = m_Server.certificate.toPem().split('\n').value(1).trimmed();
        QVERIFY(keyBody.size() > 20 && certBody.size() > 20);   // the probe really has something to find
        for (const QString &reason : reasons) {
            QVERIFY(!reason.isEmpty());
            QVERIFY(!reason.contains(QStringLiteral("BEGIN")));
            QVERIFY(!reason.contains(QString::fromLatin1(keyBody)));
            QVERIFY(!reason.contains(QString::fromLatin1(certBody)));
            QVERIFY(!reason.contains(QStringLiteral("usbfwd-test")));
            QVERIFY(!reason.contains(QStringLiteral("10.")));
        }
    }
};

QTEST_GUILESS_MAIN(SessionGlueTest)
#include "main.moc"
