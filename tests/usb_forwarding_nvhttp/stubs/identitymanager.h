#pragma once
// Test stub: a client identity with no certificate. The fake hosts do not
// require one.
//
// The certificate "b" from USBFWD_TEST_CERTS (an impostor host) is installed
// as a trusted CA, so a host presenting it passes normal TLS verification
// (its certificate carries a 127.0.0.1 subjectAltName). Only the paired-
// certificate pin can refuse it, which is what the tests need to prove.
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>

class IdentityManager
{
public:
    QString getUniqueId() { return QStringLiteral("0123456789ABCDEF"); }
    QSslConfiguration getSslConfig()
    {
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        QFile file(qEnvironmentVariable("USBFWD_TEST_CERTS") + QStringLiteral("/cert-b.pem"));
        if (file.open(QIODevice::ReadOnly)) {
            QList<QSslCertificate> authorities = config.caCertificates();
            authorities.append(QSslCertificate(file.readAll(), QSsl::Pem));
            config.setCaCertificates(authorities);
        }
        return config;
    }
    static IdentityManager* get() { static IdentityManager manager; return &manager; }
};
