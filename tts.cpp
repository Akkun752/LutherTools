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

// Voix cloud edge-tts (mêmes que VOIX_DISPONIBLES dans read.py) : c'est le
// service que Microsoft Edge utilise pour "Lire à voix haute", pas les voix
// Windows locales. Essayé en priorité, avec repli automatique sur les voix
// Windows si le service est inaccessible.
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

// Initialise COM sur le thread courant. hr reçoit le résultat brut ; la
// fonction renvoie true si l'appartement COM est utilisable (initialisé ici,
// ou déjà initialisé auparavant sur ce thread avec un autre modèle -
// RPC_E_CHANGED_MODE -, par exemple par Qt).
bool comInit(HRESULT &hr)
{
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

// Indique si l'appel à comInit() correspondant doit être équilibré par un
// CoUninitialize() (uniquement s'il a réellement (re)compté une initialisation).
bool comNeedsUninit(HRESULT hr)
{
    return hr == S_OK || hr == S_FALSE;
}

} // namespace

TTS::TTS(QObject *parent)
    : QThread(parent)
{
    // L'énumération des voix a lieu tout de suite pour pouvoir les exposer
    // via availableVoices() avant même le premier enqueue()/start().
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

    // Les voix "OneCore" (Windows 10/11, catégorie Speech_OneCore) sont
    // celles qu'on télécharge depuis Paramètres > Heure et langue > Voix :
    // bien plus nombreuses et de meilleure qualité que les 2 voix SAPI
    // "Desktop" historiques (Zira/Hortense) enregistrées sous la catégorie
    // SAPI classique SPCAT_VOICES. On les utilise donc en priorité, avec un
    // repli sur SPCAT_VOICES si jamais aucune voix OneCore n'est installée.
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
        // Filtre "Lire 1 message sur N" : seul le N-ième message reçu passe.
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
    // Chaque thread qui appelle SAPI doit initialiser COM lui-même.
    HRESULT hr;
    if (!comInit(hr)) {
        emit errorOccurred(QStringLiteral("Impossible d'initialiser COM pour le TTS"));
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

// Détermine le volume de ce message : normalement le volume de base (x1.0
// côté edge-tts, m_volume% côté SAPI), sauf si la "saturation aléatoire" est
// active et que le tirage aléatoire tombe dans m_saturationChancePercent%,
// auquel cas il est joué à m_saturationFactor fois le volume de base (ex:
// 20 = vingt fois plus fort). sapiVolumePercent reçoit l'équivalent pour le
// repli SAPI : celui-ci ne pouvant jamais dépasser 100%, tout facteur ≥ 1
// y revient simplement au volume maximum.
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

    // edge-tts (cloud, mêmes voix que read.py) en priorité ; repli sur les
    // voix Windows locales si le service est inaccessible pour une raison
    // quelconque (pas de réseau, protocole cassé côté Microsoft, timeout...).
    if (speakOneEdge(message, volumeMultiplier))
        return true;

    emit errorOccurred(QStringLiteral("edge-tts indisponible : bascule sur une voix Windows locale"));
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

// Décode le MP3 renvoyé par edge-tts (dr_mp3), applique le multiplicateur de
// volume sur le PCM brut avec écrêtage à ±1.0 (équivalent du
// np.clip(audio_data * VOLUME_MULTIPLIER, -1.0, 1.0) de read.py), puis joue
// le résultat sur la sortie audio par défaut.
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

bool TTS::speakOneLocal(const QString &message, int volumePercent)
{
    if (m_voices.empty()) {
        emit errorOccurred(QStringLiteral("Aucune voix Windows installée trouvée"));
        return false;
    }

    // Voix aléatoire parmi toutes les voix Windows installées, comme
    // random.choice(VOIX_DISPONIBLES) dans read.py.
    const int index = int(QRandomGenerator::global()->bounded(int(m_voices.size())));
    const SapiVoice &voice = m_voices[index];

    // Le token énuméré dans le constructeur ne peut pas être réutilisé ici
    // (autre thread COM) : on le recrée à partir de son identifiant.
    ComPtr<ISpObjectToken> token;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token));
    if (SUCCEEDED(hr))
        hr = token->SetId(nullptr, reinterpret_cast<LPCWSTR>(voice.id.utf16()), FALSE);
    if (FAILED(hr)) {
        emit errorOccurred(QStringLiteral("Voix introuvable : %1").arg(voice.name));
        return false;
    }

    ComPtr<ISpVoice> spVoice;
    hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&spVoice));
    if (FAILED(hr)) {
        emit errorOccurred(QStringLiteral("Impossible de créer le moteur SAPI"));
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
