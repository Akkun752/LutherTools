#ifndef CHAT_H
#define CHAT_H

#include <QObject>
#include <QTcpSocket>
#include <QString>

// Connection to Twitch chat over IRC and extraction of sent messages.
//
// Equivalent of read.py's connect_to_twitch() / read_chat() /
// parse_message(), but based on QTcpSocket: reading is driven by Qt signals
// (readyRead), not by a blocking loop in a separate thread.
//
// If an OAuth token (with the chat:read/chat:edit scopes) and a login are
// provided, the connection authenticates as that identity instead of being
// anonymous - which also allows sending messages via sendMessage().
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

    // Closes the connection (if any) and reopens it. Safe to call whether
    // currently connected, connecting, or already disconnected.
    void reconnect();

signals:
    // Emitted for every chat message received (equivalent of read.py's print + tts_queue.put).
    void messageReceived(const QString &username, const QString &message);

    // Status messages (connecting, disconnecting, errors) - equivalent of read.py's print(...).
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
    bool m_reconnectPending = false;
};

#endif // CHAT_H
