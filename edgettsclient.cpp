#include "edgettsclient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace {

// edge-tts protocol constants (cf. rany2/edge-tts, constants.py).
// TRUSTED_CLIENT_TOKEN is a public token used by Edge itself for the
// "Read aloud" feature; the Chromium versions below only need to look
// plausible for the User-Agent / Sec-MS-GEC-Version, not stay exact forever.
constexpr auto kTrustedClientToken = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
constexpr qint64 kWinEpochSeconds = 11644473600LL; // seconds between 1601-01-01 and 1970-01-01
constexpr auto kChromiumMajorVersion = "143";
constexpr auto kChromiumFullVersion = "143.0.3650.75";

} // namespace

EdgeTtsClient::EdgeTtsClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_socket, &QWebSocket::connected, this, &EdgeTtsClient::onConnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &EdgeTtsClient::onTextMessageReceived);
    connect(&m_socket, &QWebSocket::binaryMessageReceived, this, &EdgeTtsClient::onBinaryMessageReceived);
    connect(&m_socket, &QWebSocket::errorOccurred, this, &EdgeTtsClient::onSocketError);

    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &EdgeTtsClient::onTimeout);
}

// Anti-bot token: SHA256(windows_ticks_rounded_5min + TRUSTED_CLIENT_TOKEN), uppercased.
QString EdgeTtsClient::generateSecMsGec()
{
    const qint64 unixSeconds = QDateTime::currentSecsSinceEpoch();
    qint64 ticks = unixSeconds + kWinEpochSeconds;
    ticks -= ticks % 300; // round down to the nearest 5-minute multiple
    const qint64 ticks100ns = ticks * 10'000'000LL; // seconds -> 100ns intervals

    const QString toHash = QString::number(ticks100ns) + QLatin1String(kTrustedClientToken);
    const QByteArray hash = QCryptographicHash::hash(toHash.toLatin1(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex().toUpper());
}

QString EdgeTtsClient::escapeSsml(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    escaped.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return escaped;
}

void EdgeTtsClient::synthesize(const QString &text, const QString &voice)
{
    m_text = text;
    m_voice = voice;
    m_audio.clear();
    m_done = false;

    const QString connectionId = QUuid::createUuid().toString(QUuid::Id128);

    QUrl url(QStringLiteral("wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("TrustedClientToken"), QLatin1String(kTrustedClientToken));
    query.addQueryItem(QStringLiteral("Sec-MS-GEC"), generateSecMsGec());
    query.addQueryItem(QStringLiteral("Sec-MS-GEC-Version"),
                        QStringLiteral("1-%1").arg(QLatin1String(kChromiumFullVersion)));
    query.addQueryItem(QStringLiteral("ConnectionId"), connectionId);
    url.setQuery(query);

    const QString userAgent =
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/%1.0.0.0 Safari/537.36 Edg/%1.0.0.0")
            .arg(QLatin1String(kChromiumMajorVersion));

    QNetworkRequest request(url);
    request.setRawHeader("Pragma", "no-cache");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    request.setRawHeader("Accept-Encoding", "gzip, deflate, br, zstd");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("User-Agent", userAgent.toUtf8());

    m_timeout.start(10000);
    m_socket.open(request);
}

void EdgeTtsClient::onConnected()
{
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("ddd MMM dd yyyy HH:mm:ss 'GMT+0000 (Coordinated Universal Time)'"));

    const QString configMessage =
        QStringLiteral("X-Timestamp:%1\r\n"
                       "Content-Type:application/json; charset=utf-8\r\n"
                       "Path:speech.config\r\n\r\n"
                       "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
                       "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
                       "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}")
            .arg(timestamp);
    m_socket.sendTextMessage(configMessage);

    // Note: QString::arg() does not treat "%%" as a literal "%" (unlike
    // printf), so the rate/volume percent signs are written plain, not doubled.
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128);
    const QString ssml =
        QStringLiteral("<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='fr-FR'>"
                       "<voice name='%1'><prosody pitch='+0Hz' rate='+0%' volume='+0%'>%2</prosody>"
                       "</voice></speak>")
            .arg(m_voice, escapeSsml(m_text));
    const QString ssmlMessage =
        QStringLiteral("X-RequestId:%1\r\nContent-Type:application/ssml+xml\r\nX-Timestamp:%2\r\n"
                       "Path:ssml\r\n\r\n%3")
            .arg(requestId, timestamp, ssml);
    m_socket.sendTextMessage(ssmlMessage);
}

void EdgeTtsClient::onTextMessageReceived(const QString &message)
{
    if (message.contains(QLatin1String("Path:turn.end")))
        finishWith(m_audio);
}

void EdgeTtsClient::onBinaryMessageReceived(const QByteArray &message)
{
    // Each binary message starts with a text header (2-byte big-endian
    // length, then the headers themselves) followed by the MP3 data.
    if (message.size() < 2)
        return;

    const int headerLength =
        (static_cast<unsigned char>(message.at(0)) << 8) | static_cast<unsigned char>(message.at(1));
    const int audioStart = 2 + headerLength;
    if (audioStart < message.size())
        m_audio.append(message.mid(audioStart));
}

void EdgeTtsClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    failWith(m_socket.errorString());
}

void EdgeTtsClient::onTimeout()
{
    failWith(QStringLiteral("Timed out waiting for edge-tts's response"));
}

void EdgeTtsClient::finishWith(const QByteArray &data)
{
    if (m_done)
        return;
    m_done = true;
    m_timeout.stop();
    m_socket.close();
    emit finished(data);
}

void EdgeTtsClient::failWith(const QString &reason)
{
    if (m_done)
        return;
    m_done = true;
    m_timeout.stop();
    m_socket.close();
    emit failed(reason);
}
