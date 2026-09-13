#include "twitchchannel.h"
#include "twitchauth.h"
#include "twitchconstants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

TwitchChannel::TwitchChannel(TwitchAuth *auth, QObject *parent)
    : QObject(parent)
    , m_auth(auth)
    , m_network(new QNetworkAccessManager(this))
{
}

TwitchChannel::~TwitchChannel()
{
    // Même précaution que dans TwitchAuth (cf. son destructeur) : une requête
    // en cours pourrait sinon déclencher un signal après que nos membres
    // aient déjà été détruits par le nettoyage implicite d'QObject.
    delete m_network;
    m_network = nullptr;
}

void TwitchChannel::fetchInfo()
{
    if (m_auth->accessToken().isEmpty() || m_auth->userId().isEmpty()) {
        emit infoFailed(QStringLiteral("Non connecté à Twitch"));
        return;
    }

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("broadcaster_id"), m_auth->userId());
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_auth->accessToken().toUtf8());
    request.setRawHeader("Client-Id", kTwitchClientId);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit infoFailed(reply->errorString());
            return;
        }

        const QJsonArray data =
            QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) {
            emit infoFailed(QStringLiteral("Aucune information de chaîne reçue"));
            return;
        }

        const QJsonObject obj = data.first().toObject();
        const QString title = obj.value(QStringLiteral("title")).toString();
        const QString gameName = obj.value(QStringLiteral("game_name")).toString();
        const QString gameId = obj.value(QStringLiteral("game_id")).toString();

        QStringList tags;
        for (const QJsonValue &v : obj.value(QStringLiteral("tags")).toArray())
            tags << v.toString();

        emit infoReceived(title, gameName, gameId, tags);
    });
}

void TwitchChannel::updateInfo(const QString &title, const QString &gameId, const QStringList &tags)
{
    if (m_auth->accessToken().isEmpty() || m_auth->userId().isEmpty()) {
        emit updateFailed(QStringLiteral("Non connecté à Twitch"));
        return;
    }

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("broadcaster_id"), m_auth->userId());
    url.setQuery(query);

    QJsonObject body;
    body.insert(QStringLiteral("title"), title);
    if (!gameId.isEmpty())
        body.insert(QStringLiteral("game_id"), gameId);

    QJsonArray tagsArray;
    for (const QString &tag : tags)
        tagsArray.append(tag);
    body.insert(QStringLiteral("tags"), tagsArray);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_auth->accessToken().toUtf8());
    request.setRawHeader("Client-Id", kTwitchClientId);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    // Twitch attend un PATCH ; QNetworkAccessManager n'a pas de méthode patch()
    // dédiée, il faut passer par sendCustomRequest().
    QNetworkReply *reply =
        m_network->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const QJsonObject errorObj = QJsonDocument::fromJson(reply->readAll()).object();
            const QString message = errorObj.value(QStringLiteral("message")).toString();
            emit updateFailed(message.isEmpty() ? reply->errorString() : message);
            return;
        }

        emit updateSucceeded();
    });
}

void TwitchChannel::searchCategories(const QString &query)
{
    if (query.trimmed().isEmpty())
        return;

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/search/categories"));
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("query"), query);
    urlQuery.addQueryItem(QStringLiteral("first"), QStringLiteral("10"));
    url.setQuery(urlQuery);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_auth->accessToken().toUtf8());
    request.setRawHeader("Client-Id", kTwitchClientId);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        // Recherche "best effort" pour l'auto-complétion : une erreur ici ne
        // doit pas interrompre la saisie, on n'émet simplement rien.
        if (reply->error() != QNetworkReply::NoError)
            return;

        const QJsonArray data =
            QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();

        QVector<QPair<QString, QString>> results;
        results.reserve(data.size());
        for (const QJsonValue &v : data) {
            const QJsonObject obj = v.toObject();
            results.append({obj.value(QStringLiteral("name")).toString(),
                             obj.value(QStringLiteral("id")).toString()});
        }

        emit categoriesFound(results);
    });
}
