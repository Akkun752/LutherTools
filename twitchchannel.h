#ifndef TWITCHCHANNEL_H
#define TWITCHCHANNEL_H

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class TwitchAuth;

// Reads/writes the stream's title, category and tags via the Helix API
// (GET/PATCH https://api.twitch.tv/helix/channels), using the token
// provided by TwitchAuth.
//
// The customizable "go-live notification message" in the Twitch dashboard
// is deliberately not handled here: Twitch exposes no API to read or write
// it (confirmed by a Twitch moderator on the developer forum, see
// discuss.dev.twitch.com/t/set-the-notification-message-like-title-status/32343).
class TwitchChannel : public QObject
{
    Q_OBJECT

public:
    explicit TwitchChannel(TwitchAuth *auth, QObject *parent = nullptr);
    ~TwitchChannel() override;

    // Fetches the current title/category/tags; emits infoReceived() or infoFailed().
    void fetchInfo();

    // Updates the title/category/tags. Empty gameId = category left
    // unchanged (Twitch ignores the game_id field when it's omitted).
    void updateInfo(const QString &title, const QString &gameId, const QStringList &tags);

    // Searches categories by name (for autocompletion); emits categoriesFound().
    void searchCategories(const QString &query);

    // Deletes a single chat message (requires the moderator:manage:chat_messages
    // scope and moderator rights on the channel - the connected account
    // always has both on its own channel). Emits messageDeleted() or
    // messageDeleteFailed().
    void deleteMessage(const QString &messageId);

signals:
    void infoReceived(const QString &title, const QString &gameName, const QString &gameId,
                       const QStringList &tags);
    void infoFailed(const QString &reason);
    void updateSucceeded();
    void updateFailed(const QString &reason);
    // (name, id) pairs, in the order returned by Twitch (decreasing relevance).
    void categoriesFound(const QVector<QPair<QString, QString>> &categories);
    void messageDeleted(const QString &messageId);
    void messageDeleteFailed(const QString &reason);

private:
    TwitchAuth *m_auth;
    QNetworkAccessManager *m_network;
};

#endif // TWITCHCHANNEL_H
