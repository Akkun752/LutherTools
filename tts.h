#ifndef TTS_H
#define TTS_H

#include <QThread>
#include <QQueue>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <QLocale>
#include <QVoice>
#include <vector>

class QTextToSpeech;

// Une voix locale installée sur la machine, exposée par le backend Qt
// TextToSpeech (SAPI/OneCore sous Windows, speech-dispatcher/flite sous
// Linux, AVSpeechSynthesizer sous macOS...).
struct SapiVoice
{
    QVoice voice;  // objet QVoice complet, nécessaire pour re-sélectionner la voix
    QString name;  // nom lisible, ex: "Microsoft Hortense Desktop - French" ou "french-mbrola-1"
};

// Moteur TTS : empile les messages à lire et les synthétise un par un, dans un
// thread dédié, avec une voix choisie au hasard à chaque message.
//
// C'est l'équivalent du worker TTS de read.py (asyncio + queue.Queue +
// edge_tts.Communicate + sounddevice) :
//  - enqueue()      ~= tts_queue.put(message)
//  - run()          ~= _tts_worker() / start_tts_loop()
//  - speakOneEdge() ~= le bloc edge_tts.Communicate/tts.save()/sd.play()/sd.wait()
//
// La synthèse essaie d'abord le service cloud edge-tts (mêmes voix que
// read.py : Denise, Eloise, Henri...) et, si indisponible (pas de réseau,
// protocole cassé côté Microsoft, timeout...), bascule automatiquement sur
// une voix locale via QTextToSpeech (module Qt Speech, portable Windows/Linux/
// macOS) pour ne jamais rester muet.
//
// Deux filtres optionnels, réglables en direct :
//  - "lire 1 message sur N" : n'envoie qu'un message reçu sur N à la synthèse.
//  - "saturation aléatoire" : parmi les messages effectivement lus, chacun a
//    une chance (en %) d'être joué à un volume "saturé" = facteur × le
//    volume normal (ex: facteur 20 = 20 fois plus fort) au lieu du volume
//    normal.
class TTS : public QThread
{
    Q_OBJECT

public:
    explicit TTS(QObject *parent = nullptr);
    ~TTS() override;

    // Ajoute un message à la file d'attente (sous réserve du filtre "lire 1
    // message sur N") et démarre le thread si besoin.
    void enqueue(const QString &message);

    // Voix locales détectées au démarrage (utilisées en repli).
    const std::vector<SapiVoice> &availableVoices() const { return m_voices; }

    // Volume de base de la voix locale de repli, 0-100 (QTextToSpeech ne
    // permet pas de sur-amplifier au-delà de 100%, contrairement au flux
    // edge-tts).
    void setVolume(int percent);

    // Filtre "Lire 1 message sur N".
    void setReadEveryNEnabled(bool enabled);
    void setReadEveryN(int n);

    // Filtre "Saturation aléatoire" : chancePercent% des messages lus sont
    // joués à factor fois le volume de base (ex: 20 = 20x plus fort).
    void setSaturationEnabled(bool enabled);
    void setSaturationFactor(int factor);
    void setSaturationChancePercent(int chancePercent);

    // Demande l'arrêt du thread après le message en cours (à appeler avant la
    // destruction si on ne veut pas attendre la vidange complète de la file).
    void stop();

signals:
    void speechStarted(const QString &message, const QString &voiceName);
    void speechFinished(const QString &message);
    void errorOccurred(const QString &message);

protected:
    void run() override;

private:
    void loadVoices();
    float nextVolumeMultiplier(int &localVolumePercent);
    bool speakOne(const QString &message);
    bool speakOneEdge(const QString &message, float volumeMultiplier);
    bool speakOneLocal(const QString &message, int volumePercent);
    bool playMp3(const QByteArray &mp3Data, float volumeMultiplier);

    std::vector<SapiVoice> m_voices;

    QMutex m_mutex;
    QWaitCondition m_condition;
    QQueue<QString> m_queue;
    bool m_stopRequested = false;

    int m_volume = 100;

    bool m_readEveryNEnabled = false;
    int m_readEveryN = 1;
    qint64 m_receivedCount = 0;

    bool m_saturationEnabled = false;
    int m_saturationFactor = 20;
    int m_saturationChancePercent = 1;
};

#endif // TTS_H