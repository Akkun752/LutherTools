#ifndef TWITCHCHANNEL_H
#define TWITCHCHANNEL_H

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class TwitchAuth;

// Lecture/écriture du titre, de la catégorie et des tags du stream, via
// l'API Helix (GET/PATCH https://api.twitch.tv/helix/channels), en
// s'appuyant sur le token fourni par TwitchAuth.
//
// Le "message de notification de live" personnalisable dans le dashboard
// Twitch n'est volontairement pas géré ici : Twitch n'expose aucune API pour
// le lire ni l'écrire (confirmé par un modérateur Twitch sur le forum
// développeur, cf. discuss.dev.twitch.com/t/set-the-notification-message-like-title-status/32343).
class TwitchChannel : public QObject
{
    Q_OBJECT

public:
    explicit TwitchChannel(TwitchAuth *auth, QObject *parent = nullptr);
    ~TwitchChannel() override;

    // Récupère titre/catégorie/tags actuels ; émet infoReceived() ou infoFailed().
    void fetchInfo();

    // Met à jour le titre/la catégorie/les tags. gameId vide = catégorie
    // inchangée (Twitch ignore le champ game_id s'il est omis).
    void updateInfo(const QString &title, const QString &gameId, const QStringList &tags);

    // Recherche de catégories par nom (pour l'auto-complétion) ; émet categoriesFound().
    void searchCategories(const QString &query);

signals:
    void infoReceived(const QString &title, const QString &gameName, const QString &gameId,
                       const QStringList &tags);
    void infoFailed(const QString &reason);
    void updateSucceeded();
    void updateFailed(const QString &reason);
    // Paires (nom, id), dans l'ordre renvoyé par Twitch (pertinence décroissante).
    void categoriesFound(const QVector<QPair<QString, QString>> &categories);

private:
    TwitchAuth *m_auth;
    QNetworkAccessManager *m_network;
};

#endif // TWITCHCHANNEL_H
