#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

// MoonlightVibe checks its OWN releases. Upstream Moonlight's manifest
// (moonlight-stream.org/updates/qt.json) describes upstream's versions, so reading it would tell
// MoonlightVibe users to "update" to upstream Moonlight whenever upstream's version number is
// higher than ours. /releases/latest never returns drafts or pre-releases.
static const char* const k_LatestReleaseUrl =
    "https://api.github.com/repos/vibesoftwarecoder/MoonlightVibe/releases/latest";

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    QString currentVersion(VERSION_STR);
    qDebug() << "Current MoonlightVibe version:" << currentVersion;
    parseStringToVersionQuad(currentVersion, m_CurrentVersionQuad);

    // Should at least have a 1.0-style version number
    Q_ASSERT(m_CurrentVersionQuad.count() > 1);
}

void AutoUpdateChecker::start()
{
    if (!m_Nam) {
        Q_ASSERT(m_Nam);
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
    if (releaseAssetPrefix().isEmpty()) {
        // MoonlightVibe publishes no build for this platform, so there is nothing to offer.
        qDebug() << "No MoonlightVibe release build for this platform; skipping the update check";
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // We'll get a callback when this is finished
    QUrl url(k_LatestReleaseUrl);
    QNetworkRequest request(url);
    // GitHub's API refuses requests without a User-Agent (HTTP 403).
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("MoonlightVibe/%1").arg(QString(VERSION_STR)));
    request.setRawHeader("Accept", "application/vnd.github+json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

void AutoUpdateChecker::parseStringToVersionQuad(QString& string, QVector<int>& version)
{
    QStringList list = string.split('.');
    for (const QString& component : std::as_const(list)) {
        version.append(component.toInt());
    }
}

QString AutoUpdateChecker::releaseAssetPrefix()
{
    // Must match the asset names the release workflows attach:
    //   build-moonlightvibe-windows.yml -> MoonlightVibe-windows-x64-<version>.zip
    //   build-moonlightvibe-mac.yml     -> MoonlightVibe-mac-<version>.dmg
    // There are no Steam Link, AppImage or Windows ARM64 release builds.
#if defined(STEAM_LINK) || defined(APP_IMAGE)
    return QString();
#elif defined(Q_OS_DARWIN)
    return QStringLiteral("MoonlightVibe-mac-");
#elif defined(Q_OS_WIN32)
    if (QSysInfo::buildCpuArchitecture() == QStringLiteral("x86_64")) {
        return QStringLiteral("MoonlightVibe-windows-x64-");
    }
    return QString();
#else
    return QString();
#endif
}

int AutoUpdateChecker::compareVersion(QVector<int>& version1, QVector<int>& version2) {
    for (int i = 0;; i++) {
        int v1Val = 0;
        int v2Val = 0;

        // Treat missing decimal places as 0
        if (i < version1.count()) {
            v1Val = version1[i];
        }
        if (i < version2.count()) {
            v2Val = version2[i];
        }
        if (i >= version1.count() && i >= version2.count()) {
            // Equal versions
            return 0;
        }

        if (v1Val < v2Val) {
            return -1;
        }
        else if (v1Val > v2Val) {
            return 1;
        }
    }
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Update checking failed with error:" << reply->error();
        reply->deleteLater();
        return;
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonParseError error;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(body, &error);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        qWarning() << "Latest release response malformed:" << error.errorString();
        return;
    }

    // const: QJsonObject's non-const operator[] inserts missing keys.
    const QJsonObject release = jsonDoc.object();
    if (!release["tag_name"].isString() || !release["html_url"].isString() ||
            !release["assets"].isArray()) {
        qWarning() << "Latest release response missing tag_name, html_url or assets";
        return;
    }

    // /releases/latest excludes these already; checked anyway so a future API change cannot turn a
    // test build into an update prompt.
    if (release["draft"].toBool() || release["prerelease"].toBool()) {
        qDebug() << "Latest release is a draft or pre-release; not offering it";
        return;
    }

    // Offer only a release that actually carries this platform's build. The Windows and macOS
    // workflows attach their assets separately, so one may briefly exist without the other.
    const QString prefix = releaseAssetPrefix();
    bool hasAsset = false;
    const QJsonArray assets = release["assets"].toArray();
    for (const auto& asset : assets) {
        const QJsonObject assetObj = asset.toObject();
        if (assetObj["name"].toString().startsWith(prefix) &&
                assetObj["state"].toString() == QStringLiteral("uploaded")) {
            hasAsset = true;
            break;
        }
    }
    if (!hasAsset) {
        qDebug() << "Latest release has no" << prefix << "asset yet; not offering it";
        return;
    }

    // Tags are v<version>, e.g. v6.3.4; the release workflows refuse a tag that does not match
    // app/version.txt.
    QString latestVersion = release["tag_name"].toString();
    if (latestVersion.startsWith('v') || latestVersion.startsWith('V')) {
        latestVersion.remove(0, 1);
    }
    qDebug() << "Latest MoonlightVibe release is:" << latestVersion;

    QVector<int> latestVersionQuad;
    parseStringToVersionQuad(latestVersion, latestVersionQuad);
    if (latestVersionQuad.count() < 2) {
        qWarning() << "Latest release tag is not a version number:" << release["tag_name"].toString();
        return;
    }

    int res = compareVersion(m_CurrentVersionQuad, latestVersionQuad);
    if (res < 0) {
        qDebug() << "Update available";
        emit onUpdateAvailable(latestVersion, release["html_url"].toString());
    }
    else if (res > 0) {
        qDebug() << "Latest release is older than this build";
    }
    else {
        qDebug() << "This build is the latest release";
    }
}
