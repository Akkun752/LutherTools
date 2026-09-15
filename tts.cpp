#include "tts.h"
#include "edgettsclient.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QEventLoop>
#include <QMediaDevices>
#include <QMutexLocker>
#include <QRandomGenerator>

#include <windows.h>
#include <sapi.h>
#include <wrl/client.h>

#include "third_party/dr_mp3.h"

using Microsoft::WRL::ComPtr;

namespace {

// edge-tts cloud voices:
// this is the service Microsoft Edge uses for "Read aloud", not the local Windows
// voices. Tried first, with automatic fallback to Windows voices if the
// service is unreachable.
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

namespace {

// Initializes COM on the current thread. hr receives the raw result; the
// function returns true if the COM apartment is usable (initialized here,
// or already initialized earlier on this thread with a different model -
// RPC_E_CHANGED_MODE -, e.g. by Qt).
bool comInit(HRESULT &hr)
{
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

// Whether the matching comInit() call needs to be balanced by a
// CoUninitialize() (only if it actually (re)counted an initialization).
bool comNeedsUninit(HRESULT hr)
{
    return hr == S_OK || hr == S_FALSE;
}

} // namespace

TTS::TTS(QObject *parent)
    : QThread(parent)
{
    // Voice enumeration happens right away so they can be exposed via
    // availableVoices() before the very first enqueue()/start().
    HRESULT hr;
    const bool comOk = comInit(hr);
    if (comOk)
        loadVoices();
    if (comNeedsUninit(hr))
        CoUninitialize();
}

TTS::~TTS()
{
    stop();
    wait();
}

void TTS::loadVoices()
{
    m_voices.clear();

    // "OneCore" voices (Windows 10/11, Speech_OneCore category) are the ones
    // downloaded from Settings > Time & Language > Speech: far more numerous
    // and better quality than the 2 legacy "Desktop" SAPI voices
    // (Zira/Hortense) registered under the classic SAPI category
    // SPCAT_VOICES. So they're used first, falling back to SPCAT_VOICES if
    // no OneCore voice is installed at all.
    static const wchar_t *kOneCoreVoicesCategory =
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";

    if (!loadVoicesFromCategory(kOneCoreVoicesCategory))
        loadVoicesFromCategory(SPCAT_VOICES);
}

bool TTS::loadVoicesFromCategory(const wchar_t *categoryId)
{
    ComPtr<ISpObjectTokenCategory> category;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&category));
    if (FAILED(hr))
        return false;

    if (FAILED(category->SetId(categoryId, FALSE)))
        return false;

    ComPtr<IEnumSpObjectTokens> enumTokens;
    if (FAILED(category->EnumTokens(nullptr, nullptr, &enumTokens)))
        return false;

    ComPtr<ISpObjectToken> token;
    while (enumTokens->Next(1, &token, nullptr) == S_OK) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(token->GetId(&id))) {
            SapiVoice voice;
            voice.id = QString::fromWCharArray(id);
            CoTaskMemFree(id);

            LPWSTR description = nullptr;
            if (SUCCEEDED(token->GetStringValue(nullptr, &description))) {
                voice.name = QString::fromWCharArray(description);
                CoTaskMemFree(description);
            } else {
                voice.name = voice.id;
            }

            m_voices.push_back(voice);
        }
    }

    return !m_voices.empty();
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
    // Every thread that calls SAPI must initialize COM itself.
    HRESULT hr;
    if (!comInit(hr)) {
        emit errorOccurred(QStringLiteral("Could not initialize COM for TTS"));
        return;
    }
    const bool needsUninit = comNeedsUninit(hr);

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

    if (needsUninit)
        CoUninitialize();
}

// Determines this message's volume: normally the base volume (x1.0 on the
// edge-tts side, m_volume% on the SAPI side), unless "random saturation" is
// enabled and the random roll falls within m_saturationChancePercent%, in
// which case it's played at m_saturationFactor times the base volume (e.g.
// 20 = twenty times louder). sapiVolumePercent receives the equivalent for
// the SAPI fallback: since that can never exceed 100%, any factor ≥ 1
// simply comes back as the maximum volume there.
float TTS::nextVolumeMultiplier(int &sapiVolumePercent)
{
    QMutexLocker locker(&m_mutex);

    const bool saturate =
        m_saturationEnabled && QRandomGenerator::global()->bounded(100) < m_saturationChancePercent;

    if (saturate) {
        sapiVolumePercent = 100;
        return static_cast<float>(m_saturationFactor);
    }

    sapiVolumePercent = m_volume;
    return 1.0f;
}

bool TTS::speakOne(const QString &message)
{
    int sapiVolumePercent = m_volume;
    const float volumeMultiplier = nextVolumeMultiplier(sapiVolumePercent);

    // edge-tts (cloud, same voices as read.py) first; falls back to local
    // Windows voices if the service is unreachable for any reason (no
    // network, protocol broken on Microsoft's end, timeout...).
    if (speakOneEdge(message, volumeMultiplier))
        return true;

    emit errorOccurred(QStringLiteral("edge-tts unavailable: falling back to a local Windows voice"));
    return speakOneLocal(message, sapiVolumePercent);
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
        emit errorOccurred(QStringLiteral("edge-tts: %1").arg(reason));
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
// multiplier on the raw PCM with clipping at ±1.0 (equivalent of read.py's
// np.clip(audio_data * VOLUME_MULTIPLIER, -1.0, 1.0)), then plays the
// result on the default audio output.
bool TTS::playMp3(const QByteArray &mp3Data, float volumeMultiplier)
{
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, mp3Data.constData(), static_cast<size_t>(mp3Data.size()), nullptr)) {
        emit errorOccurred(QStringLiteral("Could not decode edge-tts audio"));
        return false;
    }

    const drmp3_uint64 frameCount = drmp3_get_pcm_frame_count(&mp3);
    const int channels = static_cast<int>(mp3.channels);
    const int sampleRate = static_cast<int>(mp3.sampleRate);

    std::vector<float> samples(static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
    const drmp3_uint64 framesRead = drmp3_read_pcm_frames_f32(&mp3, frameCount, samples.data());
    drmp3_uninit(&mp3);

    if (framesRead == 0) {
        emit errorOccurred(QStringLiteral("Empty edge-tts audio"));
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
        emit errorOccurred(QStringLiteral("edge-tts audio format not supported by the default output"));
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

bool TTS::speakOneLocal(const QString &message, int volumePercent)
{
    if (m_voices.empty()) {
        emit errorOccurred(QStringLiteral("No installed Windows voice found"));
        return false;
    }

    // Random voice among all installed Windows voices, like
    // random.choice(VOIX_DISPONIBLES) in read.py.
    const int index = int(QRandomGenerator::global()->bounded(int(m_voices.size())));
    const SapiVoice &voice = m_voices[index];

    // The token enumerated in the constructor can't be reused here (a
    // different COM thread): it's recreated from its identifier instead.
    ComPtr<ISpObjectToken> token;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token));
    if (SUCCEEDED(hr))
        hr = token->SetId(nullptr, reinterpret_cast<LPCWSTR>(voice.id.utf16()), FALSE);
    if (FAILED(hr)) {
        emit errorOccurred(QStringLiteral("Voice not found: %1").arg(voice.name));
        return false;
    }

    ComPtr<ISpVoice> spVoice;
    hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&spVoice));
    if (FAILED(hr)) {
        emit errorOccurred(QStringLiteral("Could not create the SAPI engine"));
        return false;
    }

    spVoice->SetVoice(token.Get());
    spVoice->SetVolume(static_cast<USHORT>(qBound(0, volumePercent, 100)));

    emit speechStarted(message, voice.name);

    // Appel bloquant (pas de SPF_ASYNC) : équivalent du sd.wait() de read.py,
    // le message suivant de la file n'est traité qu'une fois celui-ci terminé.
    hr = spVoice->Speak(reinterpret_cast<LPCWSTR>(message.utf16()), SPF_DEFAULT, nullptr);

    emit speechFinished(message);

    return SUCCEEDED(hr);
}
