#include "tts.h"
#include "edgettsclient.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QEventLoop>
#include <QMediaDevices>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QTextToSpeech>

#include "third_party/dr_mp3.h"

namespace {

// edge-tts cloud voices: this is the service Microsoft Edge uses for "Read
// aloud", not local voices. Tried first, with automatic fallback to
// QTextToSpeech if the service is unreachable.
const QVector<QString> &edgeVoices()
{
    static const QVector<QString> voices = {
        QStringLiteral("fr-FR-DeniseNeural"),
        QStringLiteral("fr-FR-EloiseNeural"),
        QStringLiteral("fr-FR-HenriNeural"),
        QStringLiteral("fr-BE-CharlineNeural"),
        QStringLiteral("fr-BE-GerardNeural"),
        QStringLiteral("fr-CA-AntoineNeural"),
        QStringLiteral("fr-CA-JeanNeural"),
        QStringLiteral("fr-CA-SylvieNeural"),
        QStringLiteral("fr-CA-ThierryNeural"),
        QStringLiteral("fr-CH-ArianeNeural"),
        QStringLiteral("fr-CH-FabriceNeural"),
    };
    return voices;
}

} // namespace

TTS::TTS(QObject *parent)
    : QThread(parent)
{
    // Voice enumeration happens right away so they can be exposed via
    // availableVoices() before the very first enqueue()/start().
    loadVoices();
}

TTS::~TTS()
{
    stop();
    wait();
}

void TTS::loadVoices()
{
    m_voices.clear();

    // QTextToSpeech uses the platform's default engine: SAPI/OneCore on
    // Windows, speech-dispatcher (or flite/espeak depending on the install)
    // on Linux, AVSpeechSynthesizer on macOS. One codebase, portable, no
    // hand-written system dependency.
    QTextToSpeech engine;
    engine.setLocale(QLocale(QLocale::French));

    const QVector<QVoice> voices = engine.availableVoices();
    m_voices.reserve(voices.size());
    for (const QVoice &voice : voices) {
        SapiVoice entry;
        entry.voice = voice;
        entry.name = voice.name();
        m_voices.push_back(entry);
    }

    // If no French voice is available (system engine without French data
    // installed), retry without a language filter rather than being left
    // with no local fallback at all.
    if (m_voices.empty()) {
        engine.setLocale(QLocale());
        const QVector<QVoice> fallbackVoices = engine.availableVoices();
        m_voices.reserve(fallbackVoices.size());
        for (const QVoice &voice : fallbackVoices) {
            SapiVoice entry;
            entry.voice = voice;
            entry.name = voice.name();
            m_voices.push_back(entry);
        }
    }
}

void TTS::enqueue(const QString &message)
{
    {
        QMutexLocker locker(&m_mutex);

        ++m_receivedCount;
        // "Read 1 message out of N" filter: only the Nth received message goes through.
        if (m_readEveryNEnabled && (m_receivedCount % qMax(1, m_readEveryN)) != 0)
            return;

        m_queue.enqueue(message);
    }
    m_condition.wakeOne();

    if (!isRunning())
        start();
}

void TTS::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
}

void TTS::setReadEveryNEnabled(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_readEveryNEnabled = enabled;
}

void TTS::setReadEveryN(int n)
{
    QMutexLocker locker(&m_mutex);
    m_readEveryN = qMax(1, n);
}

void TTS::setSaturationEnabled(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_saturationEnabled = enabled;
}

void TTS::setSaturationFactor(int factor)
{
    QMutexLocker locker(&m_mutex);
    m_saturationFactor = qBound(1, factor, 9999);
}

void TTS::setSaturationChancePercent(int chancePercent)
{
    QMutexLocker locker(&m_mutex);
    m_saturationChancePercent = qBound(0, chancePercent, 100);
}

void TTS::stop()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stopRequested = true;
    }
    m_condition.wakeAll();
}

void TTS::run()
{
    while (true) {
        QString message;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() && !m_stopRequested)
                m_condition.wait(&m_mutex);

            if (m_stopRequested && m_queue.isEmpty())
                break;

            message = m_queue.dequeue();
        }

        speakOne(message);
    }
}

// Determines this message's volume: normally the base volume (x1.0 on the
// edge-tts side, m_volume% on the local-fallback side), unless "random
// saturation" is enabled and the random roll falls within
// m_saturationChancePercent%, in which case it's played at m_saturationFactor
// times the base volume (e.g. 20 = twenty times louder). localVolumePercent
// receives the equivalent for the local fallback: since that can never
// exceed 100%, any factor ≥ 1 simply comes back as the maximum volume there.
float TTS::nextVolumeMultiplier(int &localVolumePercent)
{
    QMutexLocker locker(&m_mutex);

    const bool saturate =
        m_saturationEnabled && QRandomGenerator::global()->bounded(100) < m_saturationChancePercent;

    if (saturate) {
        localVolumePercent = 100;
        return static_cast<float>(m_saturationFactor);
    }

    localVolumePercent = m_volume;
    return 1.0f;
}

bool TTS::speakOne(const QString &message)
{
    int localVolumePercent = m_volume;
    const float volumeMultiplier = nextVolumeMultiplier(localVolumePercent);

    // edge-tts (cloud, same voices) as priority; fallback to a
    // local voice (QTextToSpeech) if the service is unreachable for any
    // reason (no network, broken protocol on Microsoft side,
    // timeout...).
    if (speakOneEdge(message, volumeMultiplier))
        return true;

    emit errorOccurred(QStringLiteral("edge-tts indisponible : bascule sur une voix locale"));
    return speakOneLocal(message, localVolumePercent);
}

bool TTS::speakOneEdge(const QString &message, float volumeMultiplier)
{
    const QVector<QString> &voices = edgeVoices();
    const QString voice = voices[QRandomGenerator::global()->bounded(voices.size())];

    EdgeTtsClient client;
    QByteArray mp3Data;
    bool success = false;

    QEventLoop loop;
    connect(&client, &EdgeTtsClient::finished, &loop, [&](const QByteArray &data) {
        mp3Data = data;
        success = true;
        loop.quit();
    });
    connect(&client, &EdgeTtsClient::failed, &loop, [&](const QString &reason) {
        emit errorOccurred(QStringLiteral("edge-tts : %1").arg(reason));
        loop.quit();
    });

    emit speechStarted(message, voice);
    client.synthesize(message, voice);
    loop.exec();

    if (!success || mp3Data.isEmpty())
        return false;

    const bool played = playMp3(mp3Data, volumeMultiplier);
    emit speechFinished(message);
    return played;
}

// Decodes the MP3 returned by edge-tts (dr_mp3), applies the volume
// multiplier on raw PCM with clipping to ±1.0 (equivalent to
// np.clip(audio_data * VOLUME_MULTIPLIER, -1.0, 1.0)), then plays
// the result on the default audio output.
bool TTS::playMp3(const QByteArray &mp3Data, float volumeMultiplier)
{
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, mp3Data.constData(), static_cast<size_t>(mp3Data.size()), nullptr)) {
        emit errorOccurred(QStringLiteral("Impossible de décoder l'audio edge-tts"));
        return false;
    }

    const drmp3_uint64 frameCount = drmp3_get_pcm_frame_count(&mp3);
    const int channels = static_cast<int>(mp3.channels);
    const int sampleRate = static_cast<int>(mp3.sampleRate);

    std::vector<float> samples(static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
    const drmp3_uint64 framesRead = drmp3_read_pcm_frames_f32(&mp3, frameCount, samples.data());
    drmp3_uninit(&mp3);

    if (framesRead == 0) {
        emit errorOccurred(QStringLiteral("Audio edge-tts vide"));
        return false;
    }
    samples.resize(static_cast<size_t>(framesRead) * static_cast<size_t>(channels));

    for (float &sample : samples)
        sample = qBound(-1.0f, sample * volumeMultiplier, 1.0f);

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Float);

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(format)) {
        emit errorOccurred(QStringLiteral("Format audio edge-tts non supporté par la sortie par défaut"));
        return false;
    }

    QByteArray pcmBytes(reinterpret_cast<const char *>(samples.data()),
                        static_cast<int>(samples.size() * sizeof(float)));
    QBuffer buffer(&pcmBytes);
    buffer.open(QIODevice::ReadOnly);

    QAudioSink sink(device, format);

    QEventLoop loop;
    connect(&sink, &QAudioSink::stateChanged, &loop, [&](QAudio::State state) {
        if (state == QAudio::IdleState || state == QAudio::StoppedState)
            loop.quit();
    });

    sink.start(&buffer);
    loop.exec();

    return true;
}

// Local, portable fallback via QTextToSpeech (Qt Speech module). An engine is
// recreated here (rather than reused from loadVoices()) because it must live
// on the thread using it; this is the TTS thread (QThread::run()),
// different from the thread that constructed TTS.
bool TTS::speakOneLocal(const QString &message, int volumePercent)
{
    if (m_voices.empty()) {
        emit errorOccurred(QStringLiteral("Aucune voix locale installée trouvée"));
        return false;
    }

    // Random voice selected from all available local voices, like
    // selecting a random choice from available voices.
    const int index = int(QRandomGenerator::global()->bounded(int(m_voices.size())));
    const SapiVoice &voice = m_voices[index];

    QTextToSpeech engine;
    engine.setVoice(voice.voice);
    // QTextToSpeech expects volume in [-1.0, 1.0] (0.0 = "normal" system volume),
    // not as an absolute percentage like SAPI; we therefore map it
    // to [-1.0, 0.0] to remain a simple attenuator, never an
    // amplifier (like the former SetVolume(0-100)).
    engine.setVolume((qBound(0, volumePercent, 100) - 100) / 100.0);

    // say() is asynchronous: we do not wait for a direct return, but for the
    // next state change indicating completion (Ready = finished
    // normally, Error = failure). stateChanged() only triggers on a
    // real change, never for the initial state: no risk of exiting
    // the loop before say() has even started speaking.
    bool ok = true;
    QEventLoop loop;
    connect(&engine, &QTextToSpeech::stateChanged, &loop, [&](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Error)
            ok = false;
        if (state == QTextToSpeech::Ready || state == QTextToSpeech::Error)
            loop.quit();
    });
    emit speechStarted(message, voice.name);
    engine.say(message);
    loop.exec();

    emit speechFinished(message);
    return ok;
}