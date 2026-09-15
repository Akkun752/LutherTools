#include "chat.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

const QString Chat::TwitchServer = QStringLiteral("irc.chat.twitch.tv");
const quint16 Chat::TwitchPort = 6667;
const QString Chat::Nickname = QStringLiteral("justinfan12345");

Chat::Chat(QString channel, QString login, QString oauthToken, QObject *parent)
    : QObject(parent)
    , m_channel(std::move(channel))
    , m_login(std::move(login))
    , m_oauthToken(std::move(oauthToken))
{
    connect(&m_socket, &QTcpSocket::connected, this, &Chat::onConnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &Chat::onReadyRead);
    connect(&m_socket, &QTcpSocket::disconnected, this, &Chat::onDisconnected);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &Chat::onSocketError);
}

void Chat::connectToChat()
{
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        return;

    emit statusChanged(QStringLiteral("Connecting to %1:%2...").arg(TwitchServer).arg(TwitchPort));
    m_buffer.clear();
    m_socket.connectToHost(TwitchServer, TwitchPort);
}

void Chat::disconnectFromChat()
{
    m_socket.disconnectFromHost();
}

bool Chat::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void Chat::reconnect()
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        connectToChat();
        return;
    }

    // Ignore extra requests while one is already pending (e.g. a button
    // mashed repeatedly): otherwise each call would queue its own one-shot
    // connection below, all firing off connectToChat() together once the
    // socket finally closes.
    if (m_reconnectPending)
        return;
    m_reconnectPending = true;

    // disconnectFromHost() is asynchronous (it doesn't cut the connection
    // right away, especially if data is still pending): connectToChat() is
    // only called once the socket actually confirms the closure, otherwise
    // its own "already connected/connecting" guard would just no-op it.
    connect(&m_socket, &QTcpSocket::disconnected, this, [this]() {
        m_reconnectPending = false;
        connectToChat();
    }, Qt::SingleShotConnection);
    m_socket.disconnectFromHost();
}

void Chat::onConnected()
{
    if (m_login.isEmpty() || m_oauthToken.isEmpty()) {
        // No credentials: anonymous PASS/NICK, like read.py's
        // connect_to_twitch(). Read-only (Twitch refuses to let an
        // anonymous connection send messages).
        sendLine(QStringLiteral("PASS SCHMOOPIIE"));
        sendLine(QStringLiteral("NICK %1").arg(Nickname));
    } else {
        // Authenticated connection (token with the chat:read/chat:edit
        // scopes): also allows sending messages via sendMessage().
        sendLine(QStringLiteral("PASS oauth:%1").arg(m_oauthToken));
        sendLine(QStringLiteral("NICK %1").arg(m_login));
    }
    sendLine(QStringLiteral("JOIN #%1").arg(m_channel));

    emit statusChanged(QStringLiteral("Connected to %1's chat").arg(m_channel));
}

void Chat::sendMessage(const QString &message)
{
    if (!isConnected())
        return;

    // A \r or \n in the message would break the IRC frame format (one line
    // = one command): stripped out as a precaution.
    QString sanitized = message;
    sanitized.remove(QLatin1Char('\r'));
    sanitized.remove(QLatin1Char('\n'));
    if (sanitized.trimmed().isEmpty())
        return;

    sendLine(QStringLiteral("PRIVMSG #%1 :%2").arg(m_channel, sanitized));
}

void Chat::onReadyRead()
{
    m_buffer += QString::fromUtf8(m_socket.readAll());

    int idx;
    while ((idx = m_buffer.indexOf(QLatin1String("\r\n"))) != -1) {
        const QString line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 2);
        processLine(line);
    }
}

void Chat::processLine(const QString &line)
{
    if (line.startsWith(QLatin1String("PING"))) {
        QString pong = line;
        pong.replace(QStringLiteral("PING"), QStringLiteral("PONG"));
        sendLine(pong);
        return;
    }

    // Equivalent of parse_message(): ":(\w+)!.*PRIVMSG #\w+ :(.+)"
    static const QRegularExpression pattern(QStringLiteral(R"(^:(\w+)!.*PRIVMSG #\w+ :(.+)$)"));
    const QRegularExpressionMatch match = pattern.match(line);
    if (match.hasMatch())
        emit messageReceived(match.captured(1), match.captured(2).trimmed());
}

void Chat::onDisconnected()
{
    emit statusChanged(QStringLiteral("Connection closed"));
}

void Chat::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    emit statusChanged(QStringLiteral("Connection error: %1").arg(m_socket.errorString()));
}

void Chat::sendLine(const QString &line)
{
    m_socket.write((line + QStringLiteral("\r\n")).toUtf8());
}
