#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

signals:
    void onUpdateAvailable(QString newVersion, QString url);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    void parseStringToVersionQuad(QString& string, QVector<int>& version);

    int compareVersion(QVector<int>& version1, QVector<int>& version2);

    // The start of this platform's asset name on a MoonlightVibe release, or an empty string when
    // MoonlightVibe publishes no build for this platform.
    QString releaseAssetPrefix();

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
};
