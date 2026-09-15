#ifndef TTS_H
#define TTS_H

#include <QThread>
#include <QQueue>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <vector>

// A Windows (SAPI) voice installed on the machine.
struct SapiVoice
{
    QString id;   // COM token identifier (registry path), used to re-select the voice
    QString name; // readable name, e.g. "Microsoft Hortense Desktop - French"
};

// TTS engine: queues up messages to read and synthesizes them one by one, in
// a dedicated thread, with a voice picked at random for each message.
//
// This is the equivalent of read.py's TTS worker (asyncio + queue.Queue +
// edge_tts.Communicate + sounddevice):
//  - enqueue()      ~= tts_queue.put(message)
//  - run()          ~= _tts_worker() / start_tts_loop()
//  - speakOneEdge() ~= the edge_tts.Communicate/tts.save()/sd.play()/sd.wait() block
//
// Synthesis first tries the edge-tts cloud service (same voices as read.py:
// Denise, Eloise, Henri...) and, if unavailable (no network, protocol broken
// on Microsoft's end, timeout...), automatically falls back to a local
// Windows voice (SAPI/OneCore) so it's never silent.
//
// Two optional filters, adjustable live:
//  - "read 1 message out of N": only forwards one message out of every N
//    received to synthesis.
//  - "random saturation": among the messages actually read, each one has a
//    chance (in %) of being played at a "saturated" volume = factor × the
//    normal volume (e.g. factor 20 = 20 times louder) instead of the normal
//    volume.
class TTS : public QThread
{
    Q_OBJECT

public:
    explicit TTS(QObject *parent = nullptr);
    ~TTS() override;

    // Adds a message to the queue (subject to the "read 1 message out of N"
    // filter) and starts the thread if needed.
    void enqueue(const QString &message);

    // Local Windows voices detected at startup (used as a fallback).
    const std::vector<SapiVoice> &availableVoices() const { return m_voices; }

    // Base volume of the fallback Windows voice, 0-100 (SAPI can't
    // over-amplify beyond 100%, unlike the edge-tts stream).
    void setVolume(int percent);

    // "Read 1 message out of N" filter.
    void setReadEveryNEnabled(bool enabled);
    void setReadEveryN(int n);

    // "Random saturation" filter: chancePercent% of the messages read are
    // played at factor times the base volume (e.g. 20 = 20x louder).
    void setSaturationEnabled(bool enabled);
    void setSaturationFactor(int factor);
    void setSaturationChancePercent(int chancePercent);

    // Requests the thread to stop after the current message (call before
    // destruction if you don't want to wait for the queue to fully drain).
    void stop();

signals:
    void speechStarted(const QString &message, const QString &voiceName);
    void speechFinished(const QString &message);
    void errorOccurred(const QString &message);

protected:
    void run() override;

private:
    void loadVoices();
    bool loadVoicesFromCategory(const wchar_t *categoryId);
    float nextVolumeMultiplier(int &sapiVolumePercent);
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
