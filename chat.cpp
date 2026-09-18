#include "chat.h"
#include "third_party/logger.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

namespace {

struct MessageTags
{
    bool isModerator = false;
    QString id;
};

// IRCv3 tags look like "badge-info=;badges=moderator/1,subscriber/12;...;id=<uuid>;mod=1;...".
// The broadcaster's own messages carry "broadcaster/1" in badges instead of
// a separate flag, so both are checked.
MessageTags parseTags(const QString &tags)
{
    MessageTags result;
    for (const QString &pair : tags.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = pair.left(eq);
        const QString value = pair.mid(eq + 1);

        if (key == QLatin1String("id"))
            result.id = value;
        else if (key == QLatin1String("mod") && value == QLatin1String("1"))
            result.isModerator = true;
        else if (key == QLatin1String("badges")
                 && (value.contains(QLatin1String("moderator/")) || value.contains(QLatin1String("broadcaster/"))))
            result.isModerator = true;
    }
    return result;
}

} // namespace

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
    // Requests IRCv3 tags: without this, PRIVMSG lines carry no badge or
    // message-id information at all, needed respectively for moderator
    // detection and for the banned-words auto-deletion feature (Twitch's
    // delete-message API takes the message's id).
    sendLine(QStringLiteral("CAP REQ :twitch.tv/tags"));

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

    logger.info("Twitch IRC connected: channel=" + m_channel.toStdString()
                + (m_login.isEmpty() ? " (anonymous)" : " (authenticated as " + m_login.toStdString() + ")"));
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

    // Equivalent of parse_message(): ":(\w+)!.*PRIVMSG #\w+ :(.+)", with an
    // optional leading "@tag1=val1;tag2=val2 " IRCv3 tags block (present
    // once the twitch.tv/tags capability has been acknowledged).
    static const QRegularExpression pattern(QStringLiteral(R"(^(?:@(\S+) )?:(\w+)!.*PRIVMSG #\w+ :(.+)$)"));
    const QRegularExpressionMatch match = pattern.match(line);
    if (match.hasMatch()) {
        const MessageTags tags = parseTags(match.captured(1));
        emit messageReceived(match.captured(2), match.captured(3).trimmed(), tags.isModerator, tags.id);
    }
}

void Chat::onDisconnected()
{
    logger.info("Twitch IRC disconnected: channel=" + m_channel.toStdString());
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
