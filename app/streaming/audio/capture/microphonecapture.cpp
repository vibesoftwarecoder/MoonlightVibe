#include "microphonecapture.h"

#include <Limelight.h>

MicrophoneCapture::MicrophoneCapture(QObject* parent)
    : QObject(parent)
    , m_DeviceId(0)
    , m_ObtainedSpec({})
    , m_Lock(nullptr)
    , m_Encoder(nullptr)
    , m_Streaming(false)
    , m_Initialized(false)
    , m_Enabled(false)
    , m_FirstPacketLogged(false)
{
}

MicrophoneCapture::~MicrophoneCapture()
{
    stop();

    if (m_DeviceId != 0) {
        SDL_CloseAudioDevice(m_DeviceId);
        m_DeviceId = 0;
    }

    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }

    if (m_Lock != nullptr) {
        SDL_DestroyMutex(m_Lock);
        m_Lock = nullptr;
    }
}

bool MicrophoneCapture::initialize(const std::string& deviceName)
{
    if (m_Initialized) {
        return true;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_InitSubSystem(SDL_INIT_AUDIO) failed for microphone capture: %s",
                    SDL_GetError());
        return false;
    }

    m_Lock = SDL_CreateMutex();
    if (m_Lock == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateMutex() failed for microphone capture: %s",
                    SDL_GetError());
        return false;
    }

    int opusError = OPUS_OK;
    m_Encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &opusError);
    if (m_Encoder == nullptr || opusError != OPUS_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "opus_encoder_create() failed for microphone capture: %s",
                    opus_strerror(opusError));
        return false;
    }

    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_COMPLEXITY(8));

    SDL_AudioSpec desired = {};
    desired.freq = kSampleRate;
    desired.format = AUDIO_S16SYS;
    desired.channels = kChannels;
    desired.samples = kFrameSize;
    desired.callback = &MicrophoneCapture::audioCallback;
    desired.userdata = this;

    const char* selectedDevice = deviceName.empty() ? nullptr : deviceName.c_str();
    m_DeviceId = SDL_OpenAudioDevice(selectedDevice, 1, &desired, &m_ObtainedSpec, 0);
    if (m_DeviceId == 0 && selectedDevice != nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_OpenAudioDevice() failed for requested microphone '%s': %s. Falling back to default input device.",
                    selectedDevice,
                    SDL_GetError());
        m_DeviceId = SDL_OpenAudioDevice(nullptr, 1, &desired, &m_ObtainedSpec, 0);
    }

    if (m_DeviceId == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_OpenAudioDevice() failed for microphone capture: %s",
                    SDL_GetError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone capture device: %s",
                selectedDevice != nullptr ? selectedDevice : "<default>");

    if (m_ObtainedSpec.freq != kSampleRate ||
            m_ObtainedSpec.channels != kChannels ||
            m_ObtainedSpec.format != AUDIO_S16SYS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone capture format mismatch: got %d Hz, %d channels, format 0x%x",
                    m_ObtainedSpec.freq,
                    m_ObtainedSpec.channels,
                    m_ObtainedSpec.format);
        return false;
    }

    SDL_PauseAudioDevice(m_DeviceId, 1);
    m_SampleBuffer.reserve(kFrameSize * 4);
    m_Initialized = true;
    return true;
}

bool MicrophoneCapture::start()
{
    if (!m_Enabled || !m_Initialized || m_DeviceId == 0 || !LiIsMicrophoneStreamActive()) {
        return false;
    }

    clearBufferedSamples();
    m_FirstPacketLogged = false;
    m_Streaming.store(true, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone capture streaming started; negotiated mic stream is active");
    SDL_PauseAudioDevice(m_DeviceId, 0);
    return true;
}

void MicrophoneCapture::stop()
{
    if (m_DeviceId != 0) {
        SDL_PauseAudioDevice(m_DeviceId, 1);
    }

    m_Streaming.store(false, std::memory_order_release);
    clearBufferedSamples();
}

void MicrophoneCapture::setEnabled(bool enabled)
{
    m_Enabled = enabled;
    if (!m_Enabled) {
        stop();
    }
}

bool MicrophoneCapture::isEnabled() const
{
    return m_Enabled;
}

bool MicrophoneCapture::isStreaming() const
{
    return m_Streaming.load(std::memory_order_acquire);
}

void MicrophoneCapture::audioCallback(void* userdata, Uint8* stream, int len)
{
    auto* capture = static_cast<MicrophoneCapture*>(userdata);
    if (capture != nullptr) {
        capture->handleAudioData(stream, len);
    }
}

void MicrophoneCapture::handleAudioData(const Uint8* stream, int len)
{
    if (!m_Streaming.load(std::memory_order_acquire) || stream == nullptr || len <= 0) {
        return;
    }

    const auto* inputSamples = reinterpret_cast<const opus_int16*>(stream);
    const int sampleCount = len / (int)sizeof(opus_int16);

    SDL_LockMutex(m_Lock);
    m_SampleBuffer.insert(m_SampleBuffer.end(), inputSamples, inputSamples + sampleCount);

    while ((int)m_SampleBuffer.size() >= kFrameSize) {
        int encodedBytes = opus_encode(m_Encoder,
                                       m_SampleBuffer.data(),
                                       kFrameSize,
                                       m_EncodedPacket.data(),
                                       (opus_int32)m_EncodedPacket.size());
        if (encodedBytes > 0) {
            int sendResult = LiSendMicrophoneOpusData(m_EncodedPacket.data(), encodedBytes);
            if (sendResult >= 0 && !m_FirstPacketLogged) {
                m_FirstPacketLogged = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Sent first client microphone packet (%d bytes Opus)",
                            encodedBytes);
            }
            else if (sendResult < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "LiSendMicrophoneOpusData() failed for microphone capture");
            }
        }

        m_SampleBuffer.erase(m_SampleBuffer.begin(), m_SampleBuffer.begin() + kFrameSize);
    }
    SDL_UnlockMutex(m_Lock);
}

void MicrophoneCapture::clearBufferedSamples()
{
    if (m_Lock != nullptr) {
        SDL_LockMutex(m_Lock);
        m_SampleBuffer.clear();
        SDL_UnlockMutex(m_Lock);
    }
}
