#ifndef EDGETTSCLIENT_H
#define EDGETTSCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QWebSocket>
#include <QTimer>

// Client "one-shot" pour le service cloud edge-tts de Microsoft — le même
// service que celui utilisé par read.py via la bibliothèque Python edge_tts.
// Ouvre une connexion WebSocket, envoie la configuration puis le SSML,
// récupère l'audio MP3 renvoyé par morceaux binaires, puis se ferme.
//
// Protocole non documenté par Microsoft, sujet à changer sans préavis (le
// jeton anti-bot Sec-MS-GEC est recalculé à chaque connexion, voir
// generateSecMsGec()). Un objet est à usage unique : appeler synthesize()
// une seule fois, il émet finished() ou failed() une seule fois.
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
