#pragma once

#include <QObject>
#include <atomic>
#include <array>
#include <string>
#include <vector>

#include <SDL.h>
#include <opus.h>

class MicrophoneCapture : public QObject
{
    Q_OBJECT

public:
    explicit MicrophoneCapture(QObject* parent = nullptr);
    ~MicrophoneCapture() override;

    bool initialize(const std::string& deviceName = {});
    bool start();
    void stop();

    void setEnabled(bool enabled);
    bool isEnabled() const;
    bool isStreaming() const;

private:
    static void audioCallback(void* userdata, Uint8* stream, int len);
    void handleAudioData(const Uint8* stream, int len);
    void clearBufferedSamples();

    SDL_AudioDeviceID m_DeviceId;
    SDL_AudioSpec m_ObtainedSpec;
    SDL_mutex* m_Lock;
    OpusEncoder* m_Encoder;
    std::vector<opus_int16> m_SampleBuffer;
    std::array<unsigned char, 1400> m_EncodedPacket;
    std::atomic_bool m_Streaming;
    bool m_Initialized;
    bool m_Enabled;

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 1;
    static constexpr int kFrameSize = 960;
    static constexpr int kBitrate = 64000;
};
