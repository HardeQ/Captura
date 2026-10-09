#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioDevice
{
    std::wstring id;
    std::wstring name;
    GUID container{}; // physical device the endpoint belongs to
};

struct AudioConfig
{
    bool enabled = true;
    int volume = 100;      // 0..200 %
    bool muted = false;
    std::wstring input;    // "auto" or an endpoint id
    std::wstring output;   // empty = system default
    int latencyMs = 80;
    std::wstring videoLink; // active video device, used to find its audio in "auto" mode
    bool allowAuto = true;  // false for webcams: their microphone would cause feedback
};

inline constexpr wchar_t kAutoAudioInput[] = L"auto";

// Captures from a USB audio endpoint (capture cards expose one next to their
// video interface) and plays it on an output device with WASAPI shared mode.
// Reconnects by itself when devices come and go.
class AudioEngine
{
public:
    AudioEngine() = default;
    ~AudioEngine();

    void Start();
    void Stop();

    // Cheap to call every frame. Volume and mute apply immediately; anything
    // else restarts the stream.
    void Configure(const AudioConfig& config);
    void Rescan();

    std::wstring Status();
    std::wstring ActiveInputName();
    float Level() const { return level_; } // 0..1 peak of the incoming signal

    static std::vector<AudioDevice> EnumerateDevices(bool capture);

private:
    void Run();
    bool Stream(const AudioConfig& config, const AudioDevice& input);
    void SetStatus(std::wstring text);

    std::thread worker_;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> restart_{false};
    std::atomic<float> level_{0.0f};
    std::atomic<int> volume_{100};
    std::atomic<bool> muted_{false};

    std::mutex mutex_;
    AudioConfig config_;
    bool configured_ = false;
    std::wstring status_ = L"Audio starting";
    std::wstring activeInput_;
};
