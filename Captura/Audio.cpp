// Defines the property keys used below (must come before the headers that declare them).
#include <initguid.h>

#include "Audio.h"

#include <audioclient.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr REFERENCE_TIME kCaptureBuffer = 2'000'000; // 200 ms, in 100 ns units
constexpr REFERENCE_TIME kRenderBuffer = 4'000'000;  // 400 ms

std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// The container ID is shared by every interface of one physical USB device, so
// it ties a capture card's audio to its video.
bool VideoContainerId(const std::wstring& symbolicLink, GUID& container)
{
    WCHAR instance[MAX_DEVICE_ID_LEN] = {};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(instance);
    if (CM_Get_Device_Interface_PropertyW(symbolicLink.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
        return false;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;
    size = sizeof(GUID);
    return CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(&container),
                                    &size, 0) == CR_SUCCESS;
}

bool IsNull(const GUID& g)
{
    return g == GUID{};
}

// Picks the capture endpoint belonging to the video device, or nullptr.
const AudioDevice* MatchInput(const std::vector<AudioDevice>& inputs, const std::wstring& videoLink)
{
    GUID container{};
    if (!videoLink.empty() && VideoContainerId(videoLink, container) && !IsNull(container))
        for (const AudioDevice& d : inputs)
            if (d.container == container)
                return &d;

    // Fall back to how capture chips name their audio interface.
    for (const AudioDevice& d : inputs)
    {
        const std::wstring n = ToLower(d.name);
        for (const wchar_t* k : {L"usb digital audio", L"digital audio interface", L"hdmi", L"capture", L"cam link",
                                 L"elgato", L"avermedia", L"magewell", L"usb3.0 video"})
            if (n.find(k) != std::wstring::npos)
                return &d;
    }
    return nullptr;
}

std::wstring DeviceName(IMMDevice* device)
{
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            name = value.pwszVal;
        PropVariantClear(&value);
    }
    return name;
}

WAVEFORMATEX StreamFormat()
{
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = kChannels;
    f.nSamplesPerSec = kSampleRate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = static_cast<WORD>(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

// Opens a shared-mode client that Windows converts to our fixed format, so the
// device's own mix format doesn't matter.
HRESULT OpenClient(IMMDevice* device, REFERENCE_TIME buffer, ComPtr<IAudioClient>& client)
{
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr))
        return hr;
    const WAVEFORMATEX format = StreamFormat();
    return client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, buffer, 0,
                              &format, nullptr);
}
} // namespace

AudioEngine::~AudioEngine()
{
    Stop();
    if (wakeEvent_)
        CloseHandle(wakeEvent_);
}

void AudioEngine::Start()
{
    if (worker_.joinable())
        return;
    if (!wakeEvent_)
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = false;
    worker_ = std::thread(&AudioEngine::Run, this);
}

void AudioEngine::Stop()
{
    if (!worker_.joinable())
        return;
    stop_ = true;
    SetEvent(wakeEvent_);
    worker_.join();
}

void AudioEngine::Configure(const AudioConfig& config)
{
    volume_ = std::clamp(config.volume, 0, 200);
    muted_ = config.muted;

    std::lock_guard lock(mutex_);
    const bool changed = !configured_ || config.enabled != config_.enabled || config.input != config_.input ||
                         config.output != config_.output || config.latencyMs != config_.latencyMs ||
                         config.videoLink != config_.videoLink || config.allowAuto != config_.allowAuto;
    config_ = config;
    configured_ = true;
    if (changed)
    {
        restart_ = true;
        if (wakeEvent_)
            SetEvent(wakeEvent_);
    }
}

void AudioEngine::Rescan()
{
    if (wakeEvent_)
        SetEvent(wakeEvent_);
}

std::wstring AudioEngine::Status()
{
    std::lock_guard lock(mutex_);
    return status_;
}

std::wstring AudioEngine::ActiveInputName()
{
    std::lock_guard lock(mutex_);
    return activeInput_;
}

void AudioEngine::SetStatus(std::wstring text)
{
    std::lock_guard lock(mutex_);
    status_ = std::move(text);
}

std::vector<AudioDevice> AudioEngine::EnumerateDevices(bool capture)
{
    std::vector<AudioDevice> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)))
        return result;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)))
            continue;
        AudioDevice info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id)
        {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.name = DeviceName(device.Get());

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &value)) && value.vt == VT_CLSID && value.puuid)
                info.container = *value.puuid;
            PropVariantClear(&value);
        }
        result.push_back(std::move(info));
    }
    return result;
}

void AudioEngine::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!stop_)
    {
        AudioConfig config;
        {
            std::lock_guard lock(mutex_);
            config = config_;
            restart_ = false;
            activeInput_.clear();
        }
        level_ = 0.0f;

        if (!config.enabled)
        {
            SetStatus(L"Audio is off");
            WaitForSingleObject(wakeEvent_, INFINITE);
            continue;
        }

        const auto inputs = EnumerateDevices(true);
        const AudioDevice* input = nullptr;
        if (config.input == kAutoAudioInput)
        {
            if (!config.allowAuto)
            {
                SetStatus(L"Audio: this source has no capture-card audio. Choose an input in the Audio menu.");
                WaitForSingleObject(wakeEvent_, INFINITE);
                continue;
            }
            input = MatchInput(inputs, config.videoLink);
            if (!input)
            {
                SetStatus(config.videoLink.empty() ? L"Audio: waiting for a video source"
                                                   : L"Audio: no audio device found for this source");
                WaitForSingleObject(wakeEvent_, 2000);
                continue;
            }
        }
        else
        {
            for (const AudioDevice& d : inputs)
                if (d.id == config.input)
                    input = &d;
            if (!input)
            {
                SetStatus(L"Audio: the chosen input device is not connected");
                WaitForSingleObject(wakeEvent_, 2000);
                continue;
            }
        }

        const bool streamed = Stream(config, *input);
        if (stop_ || restart_)
            continue;
        SetStatus(streamed ? L"Audio: device lost. Reconnecting..." : L"Audio: could not open the audio devices");
        level_ = 0.0f;
        WaitForSingleObject(wakeEvent_, 1500);
    }

    CoUninitialize();
}

// Returns true if streaming started (and later ended), false if the devices
// could not be opened.
bool AudioEngine::Stream(const AudioConfig& config, const AudioDevice& input)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return false;

    ComPtr<IMMDevice> captureDevice, renderDevice;
    if (FAILED(enumerator->GetDevice(input.id.c_str(), &captureDevice)))
        return false;
    HRESULT hr = config.output.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice)
                                       : enumerator->GetDevice(config.output.c_str(), &renderDevice);
    if (FAILED(hr))
        return false;
    const std::wstring outputName = DeviceName(renderDevice.Get());

    ComPtr<IAudioClient> captureClient, renderClient;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    if (FAILED(OpenClient(captureDevice.Get(), kCaptureBuffer, captureClient)) ||
        FAILED(OpenClient(renderDevice.Get(), kRenderBuffer, renderClient)) ||
        FAILED(captureClient->GetService(IID_PPV_ARGS(&capture))) ||
        FAILED(renderClient->GetService(IID_PPV_ARGS(&render))))
        return false;

    UINT32 renderFrames = 0;
    if (FAILED(renderClient->GetBufferSize(&renderFrames)))
        return false;

    // Keep about `latencyMs` of audio queued: enough to ride out scheduling
    // jitter, small enough to stay in sync with the picture.
    const UINT32 target = std::min<UINT32>(kSampleRate / 1000 * std::clamp(config.latencyMs, 10, 300), renderFrames / 2);
    const UINT32 maxFill = std::min<UINT32>(target * 2, renderFrames);

    // Start with silence so the first real samples have a cushion.
    BYTE* data = nullptr;
    if (FAILED(render->GetBuffer(target, &data)) || FAILED(render->ReleaseBuffer(target, AUDCLNT_BUFFERFLAGS_SILENT)))
        return false;
    if (FAILED(captureClient->Start()) || FAILED(renderClient->Start()))
        return false;

    {
        std::lock_guard lock(mutex_);
        activeInput_ = input.name;
    }
    SetStatus(std::format(L"Audio: {} → {}", input.name, outputName));

    float gain = muted_ ? 0.0f : volume_ / 100.0f;
    while (!stop_ && !restart_)
    {
        WaitForSingleObject(wakeEvent_, 5);
        if (stop_ || restart_)
            break;

        UINT32 packet = 0;
        hr = capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0)
        {
            BYTE* in = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&in, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const float* src = reinterpret_cast<const float*>(in);
            const size_t samples = static_cast<size_t>(frames) * kChannels;

            float peak = 0.0f;
            if (!silent)
                for (size_t i = 0; i < samples; ++i)
                    peak = std::max(peak, std::fabs(src[i]));
            level_ = std::max(peak, level_.load() * 0.85f);

            UINT32 padding = 0;
            if (SUCCEEDED(renderClient->GetCurrentPadding(&padding)))
            {
                // Underrun: rebuild the cushion. Overrun (clocks drifted): drop this packet.
                if (padding == 0 && SUCCEEDED(render->GetBuffer(target / 2, &data)))
                    render->ReleaseBuffer(target / 2, AUDCLNT_BUFFERFLAGS_SILENT);
                else if (padding + frames <= maxFill && SUCCEEDED(render->GetBuffer(frames, &data)))
                {
                    if (silent)
                    {
                        render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                    else
                    {
                        // Ramp the gain across the packet so volume changes don't click.
                        const float next = muted_ ? 0.0f : volume_ / 100.0f;
                        float* out = reinterpret_cast<float*>(data);
                        for (UINT32 f = 0; f < frames; ++f)
                        {
                            const float g = gain + (next - gain) * (static_cast<float>(f) / static_cast<float>(frames));
                            for (int c = 0; c < kChannels; ++c)
                                out[f * kChannels + c] = std::clamp(src[f * kChannels + c] * g, -1.0f, 1.0f);
                        }
                        gain = next;
                        render->ReleaseBuffer(frames, 0);
                    }
                }
            }

            capture->ReleaseBuffer(frames);
            hr = capture->GetNextPacketSize(&packet);
        }
        if (FAILED(hr))
            break;
    }

    captureClient->Stop();
    renderClient->Stop();
    level_ = 0.0f;
    return true;
}
