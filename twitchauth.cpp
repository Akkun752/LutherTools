#include "twitchauth.h"
#include "appsettings.h"
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

// channel:manage:broadcast : lecture/modification du titre, de la catégorie
// et des tags du stream (API Helix GET/PATCH /channels).
// chat:read / chat:edit : connexion IRC authentifiée (au lieu d'anonyme),
// nécessaire pour pouvoir envoyer des messages dans le chat en tant que la
// chaîne connectée.
constexpr auto kScopes = "channel:manage:broadcast chat:read chat:edit";

} // namespace

TwitchAuth::TwitchAuth(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_clientId(QLatin1String(kTwitchClientId))
{
}

TwitchAuth::~TwitchAuth()
{
    // Une requête peut être en cours (poll, validate, fetchProfile...) à la
    // destruction. Le nettoyage implicite d'QObject ne supprimerait m_network
    // qu'après que nos propres membres (m_login, m_accessToken...) aient déjà
    // été détruits par le destructeur implicite ; si cette suppression
    // déclenche le finished() du reply en cours de façon synchrone (connexion
    // directe, même thread), le lambda connecté toucherait des membres déjà
    // détruits -> "destructor may have already run". On le supprime donc en
    // premier, tant que le reste de l'objet est encore valide.
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
                QStringLiteral("Impossible de démarrer la connexion Twitch : %1").arg(reply->errorString()));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_deviceCode = obj.value(QStringLiteral("device_code")).toString();
        m_pollIntervalSec = qMax(1, obj.value(QStringLiteral("interval")).toInt(5));
        const QString verificationUri = obj.value(QStringLiteral("verification_uri")).toString();
        const QString userCode = obj.value(QStringLiteral("user_code")).toString();

        if (m_deviceCode.isEmpty() || verificationUri.isEmpty()) {
            emit authFailed(QStringLiteral("Réponse inattendue de Twitch lors de la demande de code"));
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
        // Le refresh a échoué : la session est irrécupérable, il faudra se reconnecter.
        clearTokens();
        emit authFailed(QStringLiteral("Session Twitch expirée, reconnexion nécessaire"));
        return;
    }

    // Flux "device code" : tant que ce n'est pas encore autorisé côté navigateur,
    // Twitch répond authorization_pending et il faut continuer de poller.
    const QString message = obj.value(QStringLiteral("message")).toString();
    if (message == QLatin1String("authorization_pending"))
        return; // le timer relancera un poll au prochain tick

    if (m_pollTimer)
        m_pollTimer->stop();

    const QString reason = message.isEmpty() ? reply->errorString() : message;
    emit authFailed(QStringLiteral("Connexion Twitch refusée ou expirée : %1").arg(reason));
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
            if (!m_login.isEmpty()) {
                fetchProfile();
                return;
            }
        }

        // Token invalide ou expiré : on tente un rafraîchissement avant d'abandonner.
        if (m_refreshToken.isEmpty()) {
            clearTokens();
            emit authFailed(QStringLiteral("Session Twitch invalide"));
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

// Récupère le nom d'affichage et l'avatar via l'API Helix (GET /users, sans
// paramètre id/login -> renvoie l'utilisateur associé au token lui-même).
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

        // Le profil (avatar/nom d'affichage) est un bonus cosmétique : même
        // en cas d'échec de cet appel, l'authentification elle-même est
        // acquise (login valide) et ne doit pas être bloquée pour autant.
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
