#ifndef EDGETTSCLIENT_H
#define EDGETTSCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QWebSocket>
#include <QTimer>

// "One-shot" client for Microsoft's edge-tts cloud service — the same
// service used by read.py via the Python edge_tts library.
// Opens a WebSocket connection, sends the config then the SSML, collects the
// MP3 audio returned as binary chunks, then closes.
//
// Protocol undocumented by Microsoft, subject to change without notice (the
// Sec-MS-GEC anti-bot token is recomputed on every connection, see
// generateSecMsGec()). An object is single-use: call synthesize() only
// once, it emits finished() or failed() exactly once.
class EdgeTtsClient : public QObject
{
    Q_OBJECT

public:
    explicit EdgeTtsClient(QObject *parent = nullptr);

    void synthesize(const QString &text, const QString &voice);

signals:
    void finished(const QByteArray &mp3Data);
    void failed(const QString &reason);

private slots:
    void onConnected();
    void onTextMessageReceived(const QString &message);
    void onBinaryMessageReceived(const QByteArray &message);
    void onSocketError(QAbstractSocket::SocketError error);
    void onTimeout();

private:
    static QString generateSecMsGec();
    static QString escapeSsml(const QString &text);
    void finishWith(const QByteArray &data);
    void failWith(const QString &reason);

    QWebSocket m_socket;
    QTimer m_timeout;
    QByteArray m_audio;
    QString m_text;
    QString m_voice;
    bool m_done = false;
};

#endif // EDGETTSCLIENT_H
