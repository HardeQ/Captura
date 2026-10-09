#pragma once

#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Settings.h"

// Posted to the notify window whenever the status text changes.
constexpr UINT WM_APP_STATUS = WM_APP + 1;

struct VideoFrame
{
    std::vector<uint32_t> pixels; // top-down BGRX, width * height
    UINT32 width = 0;
    UINT32 height = 0;
    uint64_t sequence = 0; // bumped on every change
};

struct DeviceInfo
{
    std::wstring name;
    std::wstring link; // symbolic link, unique per device
    std::wstring kind; // "Capture card", "Webcam", ...
    int score = 0;     // higher = more likely a capture card
};

struct FormatInfo
{
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsX100 = 0;
    GUID subtype{};
};

std::wstring SubtypeName(const GUID& subtype);
std::wstring FpsText(UINT32 fpsX100);

// Streams a capture device on a worker thread and converts every frame to
// RGB32. Reconnects automatically when the selected device comes back.
class CaptureEngine
{
public:
    explicit CaptureEngine(HWND notifyWindow);
    ~CaptureEngine();

    void Start();
    void Stop();

    // Starts streaming `link` (empty = best available device) in `format`.
    void Select(const std::wstring& link, const std::wstring& name, const FormatRequest& format);

    // Call after a device arrival/removal.
    void Rescan();

    bool HasRequest();
    std::wstring Status();
    std::wstring ActiveName();
    std::wstring ActiveLink();
    std::vector<FormatInfo> Formats(); // native modes of the streaming device
    bool CurrentFormat(FormatInfo& format);
    double MeasuredFps() const { return measuredFps_; }

    // Runs f(const VideoFrame&) with the latest frame locked.
    template <class F>
    void WithFrame(F&& f)
    {
        std::lock_guard lock(frameMutex_);
        f(frame_);
    }

    // All video capture devices, most likely capture card first.
    static std::vector<DeviceInfo> EnumerateDevices();

private:
    struct Request
    {
        bool active = false;
        std::wstring link;
        std::wstring name;
        FormatRequest format;
    };

    void Run();
    bool StreamDevice(const DeviceInfo& device, const FormatRequest& format);
    void SetStatus(std::wstring text);
    void ClearFrame();
    void ShutdownActiveSource();

    HWND notifyWindow_;
    std::thread worker_;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> restart_{false};
    std::atomic<double> measuredFps_{0.0};

    std::mutex requestMutex_;
    Request request_;

    std::mutex sourceMutex_;
    Microsoft::WRL::ComPtr<IMFMediaSource> activeSource_;
    std::wstring activeLink_;
    std::wstring activeName_;
    std::vector<FormatInfo> formats_;
    FormatInfo current_;
    bool hasCurrent_ = false;

    std::mutex statusMutex_;
    std::wstring status_;

    std::mutex frameMutex_;
    VideoFrame frame_;
    VideoFrame backFrame_; // filled by the worker, swapped into frame_
};
