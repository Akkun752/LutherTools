#ifndef CHAT_H
#define CHAT_H

#include <QObject>
#include <QTcpSocket>
#include <QString>

// Connexion au chat Twitch en IRC et extraction des messages envoyés.
//
// Équivalent de connect_to_twitch() / read_chat() / parse_message() dans
// read.py, mais basé sur QTcpSocket : la lecture est pilotée par les signaux
// Qt (readyRead), pas par une boucle bloquante dans un thread séparé.
//
// Si un token OAuth (avec les scopes chat:read/chat:edit) et un login sont
// fournis, la connexion s'authentifie sous cette identité au lieu d'être
// anonyme - ce qui permet aussi d'envoyer des messages via sendMessage().
class Chat : public QObject
{
    Q_OBJECT

public:
    explicit Chat(QString channel, QString login = QString(), QString oauthToken = QString(),
                  QObject *parent = nullptr);

    void connectToChat();
    void disconnectFromChat();
    bool isConnected() const;
    void sendMessage(const QString &message);

signals:
    // Émis pour chaque message de chat reçu (équivalent du print + tts_queue.put de read.py).
    void messageReceived(const QString &username, const QString &message);

    // Messages d'état (connexion, déconnexion, erreurs) - équivalent des print(...) de read.py.
    void statusChanged(const QString &status);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);

private:
    void sendLine(const QString &line);
    void processLine(const QString &line);

    static const QString TwitchServer;
    static const quint16 TwitchPort;
    static const QString Nickname;

    QString m_channel;
    QString m_login;
    QString m_oauthToken;
    QTcpSocket m_socket;
    QString m_buffer;
};

#endif // CHAT_H
