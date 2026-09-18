#include "twitchauth.h"
#include "appsettings.h"
#include "third_party/logger.h"
#include "twitchconstants.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

// channel:manage:broadcast: reading/editing the stream's title, category
// and tags (Helix GET/PATCH /channels API).
// chat:read / chat:edit: authenticated IRC connection (instead of
// anonymous), needed to be able to send messages in chat as the connected
// channel.
// moderator:manage:chat_messages: deleting a chat message via the Helix API
// (used by the banned-words auto-deletion feature).
constexpr auto kScopes = "channel:manage:broadcast chat:read chat:edit moderator:manage:chat_messages";

} // namespace

TwitchAuth::TwitchAuth(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_clientId(QLatin1String(kTwitchClientId))
{
}

TwitchAuth::~TwitchAuth()
{
    // A request may be in flight (poll, validate, fetchProfile...) at
    // destruction time. QObject's implicit cleanup would only delete
    // m_network after our own members (m_login, m_accessToken...) have
    // already been destroyed by the implicit destructor; if that deletion
    // synchronously triggers the in-flight reply's finished() (direct
    // connection, same thread), the connected lambda would touch already-
    // destroyed members -> "destructor may have already run". So it's
    // deleted first, while the rest of the object is still valid.
    delete m_network;
    m_network = nullptr;
}

void TwitchAuth::restoreSession()
{
    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("twitch"));
    m_accessToken = settings.value(QStringLiteral("accessToken")).toString();
    m_refreshToken = settings.value(QStringLiteral("refreshToken")).toString();
    settings.endGroup();

    if (m_accessToken.isEmpty())
        return;

    validateToken();
}

void TwitchAuth::startLogin()
{
    requestDeviceCode();
}

void TwitchAuth::logout()
{
    logger.info("Twitch OAuth logged out: login=" + m_login.toStdString());

    clearTokens();
    m_login.clear();
    m_userId.clear();
    m_scopes.clear();
    emit loggedOut();
}

void TwitchAuth::requestDeviceCode()
{
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), m_clientId);
    body.addQueryItem(QStringLiteral("scopes"), QLatin1String(kScopes));

    QNetworkRequest request((QUrl(QStringLiteral("https://id.twitch.tv/oauth2/device"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply *reply = m_network->post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit authFailed(
                QStringLiteral("Could not start the Twitch login: %1").arg(reply->errorString()));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_deviceCode = obj.value(QStringLiteral("device_code")).toString();
        m_pollIntervalSec = qMax(1, obj.value(QStringLiteral("interval")).toInt(5));
        const QString verificationUri = obj.value(QStringLiteral("verification_uri")).toString();
        const QString userCode = obj.value(QStringLiteral("user_code")).toString();

        if (m_deviceCode.isEmpty() || verificationUri.isEmpty()) {
            emit authFailed(QStringLiteral("Unexpected response from Twitch while requesting the code"));
            return;
        }

        QDesktopServices::openUrl(QUrl(verificationUri));
        emit awaitingAuthorization(verificationUri, userCode);

        if (!m_pollTimer) {
            m_pollTimer = new QTimer(this);
            connect(m_pollTimer, &QTimer::timeout, this, &TwitchAuth::pollToken);
        }
        m_pollTimer->start(m_pollIntervalSec * 1000);
    });
}

void TwitchAuth::pollToken()
{
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), m_clientId);
    body.addQueryItem(QStringLiteral("scopes"), QLatin1String(kScopes));
    body.addQueryItem(QStringLiteral("device_code"), m_deviceCode);
    body.addQueryItem(QStringLiteral("grant_type"),
                       QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));

    QNetworkRequest request((QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply *reply = m_network->post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleTokenResponse(reply, false); });
}

void TwitchAuth::handleTokenResponse(QNetworkReply *reply, bool isRefresh)
{
    reply->deleteLater();
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

    const QString newAccessToken = obj.value(QStringLiteral("access_token")).toString();
    if (!newAccessToken.isEmpty()) {
        m_accessToken = newAccessToken;
        const QString newRefreshToken = obj.value(QStringLiteral("refresh_token")).toString();
        if (!newRefreshToken.isEmpty())
            m_refreshToken = newRefreshToken;
        saveTokens();

        if (m_pollTimer)
            m_pollTimer->stop();

        validateToken();
        return;
    }

    if (isRefresh) {
        // The refresh failed: the session is unrecoverable, a fresh login is needed.
        clearTokens();
        emit authFailed(QStringLiteral("Twitch session expired, please log in again"));
        return;
    }

    // "Device code" flow: as long as it hasn't been authorized in the
    // browser yet, Twitch replies authorization_pending and polling must
    // continue.
    const QString message = obj.value(QStringLiteral("message")).toString();
    if (message == QLatin1String("authorization_pending"))
        return; // the timer will trigger another poll on the next tick

    if (m_pollTimer)
        m_pollTimer->stop();

    const QString reason = message.isEmpty() ? reply->errorString() : message;
    emit authFailed(QStringLiteral("Twitch login denied or expired: %1").arg(reason));
}

void TwitchAuth::validateToken()
{
    QNetworkRequest request((QUrl(QStringLiteral("https://id.twitch.tv/oauth2/validate"))));
    request.setRawHeader("Authorization", "OAuth " + m_accessToken.toUtf8());

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            m_login = obj.value(QStringLiteral("login")).toString();
            m_userId = obj.value(QStringLiteral("user_id")).toString();
            m_scopes.clear();
            for (const QJsonValue &scope : obj.value(QStringLiteral("scopes")).toArray())
                m_scopes << scope.toString();
            logger.info("Twitch OAuth scopes granted: " + m_scopes.join(QStringLiteral(", ")).toStdString());
            if (!m_login.isEmpty()) {
                fetchProfile();
                return;
            }
        }

        // Invalid or expired token: try a refresh before giving up.
        if (m_refreshToken.isEmpty()) {
            clearTokens();
            emit authFailed(QStringLiteral("Invalid Twitch session"));
            return;
        }

        QUrlQuery body;
        body.addQueryItem(QStringLiteral("client_id"), m_clientId);
        body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        body.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);

        QNetworkRequest refreshRequest((QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"))));
        refreshRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                  QStringLiteral("application/x-www-form-urlencoded"));

        QNetworkReply *refreshReply =
            m_network->post(refreshRequest, body.toString(QUrl::FullyEncoded).toUtf8());
        connect(refreshReply, &QNetworkReply::finished, this,
                [this, refreshReply]() { handleTokenResponse(refreshReply, true); });
    });
}

// Fetches the display name and avatar via the Helix API (GET /users, with
// no id/login parameter -> returns the user associated with the token itself).
void TwitchAuth::fetchProfile()
{
    QNetworkRequest request((QUrl(QStringLiteral("https://api.twitch.tv/helix/users"))));
    request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    request.setRawHeader("Client-Id", m_clientId.toUtf8());

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        QString displayName = m_login;
        QString avatarUrl;

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
            if (!data.isEmpty()) {
                const QJsonObject user = data.first().toObject();
                displayName = user.value(QStringLiteral("display_name")).toString(m_login);
                avatarUrl = user.value(QStringLiteral("profile_image_url")).toString();
            }
        }

        // The profile (avatar/display name) is a cosmetic bonus: even if
        // this call fails, authentication itself already succeeded (valid
        // login) and shouldn't be blocked on it.
        logger.info("Twitch OAuth authenticated: login=" + m_login.toStdString());
        emit authenticated(m_login, displayName, avatarUrl);
    });
}

void TwitchAuth::saveTokens()
{
    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("twitch"));
    settings.setValue(QStringLiteral("accessToken"), m_accessToken);
    settings.setValue(QStringLiteral("refreshToken"), m_refreshToken);
    settings.endGroup();
}

void TwitchAuth::clearTokens()
{
    m_accessToken.clear();
    m_refreshToken.clear();

    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("twitch"));
    settings.remove(QString());
    settings.endGroup();
}
