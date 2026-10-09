#include "Capture.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <tuple>

using Microsoft::WRL::ComPtr;

namespace
{
std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle)
{
    return haystack.find(needle) != std::wstring::npos;
}

// Scores and labels a device. Higher score = more likely a capture card.
void ClassifyDevice(DeviceInfo& device)
{
    const std::wstring n = ToLower(device.name);
    const std::wstring l = ToLower(device.link);
    int score = 0;
    bool captureName = false, captureVendor = false, webcam = false, virtualCam = false;

    // Generic names used by UVC capture cards (MS2109/MS2130 dongles report "USB Video").
    // "USB Video Device" is Windows' stock name for any UVC webcam, so it doesn't count.
    const bool genericUvc = Contains(n, L"usb video device");
    for (const wchar_t* k : {L"usb video", L"capture", L"hdmi", L"cam link", L"elgato", L"avermedia",
                             L"live gamer", L"magewell", L"usb3.0 video", L"ezcap", L"game capture",
                             L"video grabber", L"uvc"})
        if (Contains(n, k) && !(genericUvc && std::wcscmp(k, L"usb video") == 0))
        {
            score += 50;
            captureName = true;
        }

    // USB vendor IDs of common capture chips/brands: MacroSilicon, Elgato, AVerMedia, Magewell.
    for (const wchar_t* k : {L"vid_534d", L"vid_345f", L"vid_0fd9", L"vid_07ca", L"vid_2935"})
        if (Contains(l, k))
        {
            score += 100;
            captureVendor = true;
        }

    // Vendors that make laptop/desk webcams: Chicony, Realtek, Azurewave, Quanta, Bison,
    // Sonix, Logitech, Microsoft, Sunplus, Syntek, Foxlink, Luxvisions.
    for (const wchar_t* k : {L"vid_04f2", L"vid_0bda", L"vid_13d3", L"vid_0408", L"vid_5986", L"vid_0c45",
                             L"vid_046d", L"vid_045e", L"vid_1bcf", L"vid_174f", L"vid_05c8", L"vid_30c9"})
        if (Contains(l, k))
        {
            score -= 60;
            webcam = true;
        }

    for (const wchar_t* k : {L"webcam", L"integrated", L"facetime", L"ir camera", L"front", L"rear"})
        if (Contains(n, k))
        {
            score -= 80;
            webcam = true;
        }
    if (Contains(n, L"camera"))
    {
        score -= 20;
        webcam = true;
    }

    for (const wchar_t* k : {L"virtual", L"obs", L"snap camera", L"droidcam", L"iriun"})
        if (Contains(n, k))
        {
            score -= 80;
            virtualCam = true;
        }

    const bool usb = Contains(l, L"\\\\?\\usb#");
    score += usb ? 10 : -50; // non-USB links are software devices; rank below real cameras

    device.score = score;
    if (captureName || captureVendor)
        device.kind = L"Capture card";
    else if (virtualCam || !usb)
        device.kind = L"Virtual camera";
    else if (webcam)
        device.kind = L"Webcam";
    else
        device.kind = L"USB camera";
}

std::wstring GetString(IMFActivate* activate, REFGUID key)
{
    WCHAR* value = nullptr;
    UINT32 length = 0;
    std::wstring result;
    if (SUCCEEDED(activate->GetAllocatedString(key, &value, &length)))
    {
        result.assign(value, length);
        CoTaskMemFree(value);
    }
    return result;
}

int SubtypePreference(const GUID& subtype)
{
    if (subtype == MFVideoFormat_NV12) return 3;
    if (subtype == MFVideoFormat_YUY2) return 2;
    if (subtype == MFVideoFormat_MJPG) return 1;
    return 0;
}

struct NativeType
{
    FormatInfo info;
    ComPtr<IMFMediaType> type;
};

HRESULT GetTypeHandler(IMFMediaSource* source, IMFMediaTypeHandler** handler)
{
    ComPtr<IMFPresentationDescriptor> pd;
    HRESULT hr = source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) return hr;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    return sd->GetMediaTypeHandler(handler);
}

std::vector<NativeType> ListNativeTypes(IMFMediaTypeHandler* handler)
{
    std::vector<NativeType> types;
    DWORD count = 0;
    if (FAILED(handler->GetMediaTypeCount(&count)))
        return types;
    for (DWORD i = 0; i < count; ++i)
    {
        NativeType t;
        if (FAILED(handler->GetMediaTypeByIndex(i, &t.type)))
            continue;
        UINT32 num = 0, den = 0;
        t.type->GetGUID(MF_MT_SUBTYPE, &t.info.subtype);
        MFGetAttributeSize(t.type.Get(), MF_MT_FRAME_SIZE, &t.info.width, &t.info.height);
        MFGetAttributeRatio(t.type.Get(), MF_MT_FRAME_RATE, &num, &den);
        t.info.fpsX100 = den ? static_cast<UINT32>((static_cast<UINT64>(num) * 100 + den / 2) / den) : 0;
        if (t.info.width && t.info.height)
            types.push_back(std::move(t));
    }
    return types;
}

// Honors the request where possible, relaxing format, then frame rate, then
// resolution if the device can't match. Among candidates prefers up to 1080p,
// then highest frame rate, then uncompressed over MJPG.
size_t ChooseFormat(const std::vector<NativeType>& types, const FormatRequest& request)
{
    auto key = [](const FormatInfo& f) {
        return std::make_tuple(std::min<UINT32>(f.height, 1080), f.fpsX100, SubtypePreference(f.subtype),
                               static_cast<UINT64>(f.width) * f.height);
    };
    for (int relax = 0; relax < 4; ++relax)
    {
        const bool matchSize = relax < 3 && request.width;
        const bool matchFps = relax < 2 && request.fpsX100;
        const bool matchSubtype = relax < 1 && request.subtype != GUID_NULL;
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < types.size(); ++i)
        {
            const FormatInfo& f = types[i].info;
            if (matchSize && (f.width != request.width || f.height != request.height)) continue;
            if (matchFps && f.fpsX100 != request.fpsX100) continue;
            if (matchSubtype && f.subtype != request.subtype) continue;
            if (best == SIZE_MAX || key(f) > key(types[best].info))
                best = i;
        }
        if (best != SIZE_MAX)
            return best;
    }
    return SIZE_MAX;
}

HRESULT CreateSourceFromLink(const std::wstring& link, IMFMediaSource** source)
{
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) return hr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    return MFCreateDeviceSource(attrs.Get(), source);
}
} // namespace

std::wstring SubtypeName(const GUID& subtype)
{
    if (subtype == MFVideoFormat_MJPG) return L"MJPG";
    if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    if (subtype == MFVideoFormat_H264) return L"H264";
    if (subtype == MFVideoFormat_RGB24) return L"RGB24";
    if (subtype == MFVideoFormat_RGB32) return L"RGB32";
    // FourCC-based subtypes keep the code in Data1.
    char fourcc[5] = {};
    std::memcpy(fourcc, &subtype.Data1, 4);
    return std::wstring(fourcc, fourcc + 4);
}

std::wstring FpsText(UINT32 fpsX100)
{
    if (fpsX100 % 100 == 0)
        return std::to_wstring(fpsX100 / 100);
    return std::format(L"{:.2f}", fpsX100 / 100.0);
}

CaptureEngine::CaptureEngine(HWND notifyWindow)
    : notifyWindow_(notifyWindow), wakeEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
}

CaptureEngine::~CaptureEngine()
{
    Stop();
    if (wakeEvent_)
        CloseHandle(wakeEvent_);
}

void CaptureEngine::Start()
{
    stop_ = false;
    worker_ = std::thread(&CaptureEngine::Run, this);
}

void CaptureEngine::Stop()
{
    if (!worker_.joinable())
        return;
    stop_ = true;
    SetEvent(wakeEvent_);
    ShutdownActiveSource(); // unblocks a pending ReadSample
    worker_.join();
}

void CaptureEngine::Select(const std::wstring& link, const std::wstring& name, const FormatRequest& format)
{
    {
        std::lock_guard lock(requestMutex_);
        request_ = {true, link, name, format};
        restart_ = true;
    }
    ShutdownActiveSource();
    SetEvent(wakeEvent_);
}

void CaptureEngine::Rescan()
{
    const std::wstring link = ActiveLink();
    if (!link.empty())
    {
        const auto devices = EnumerateDevices();
        const bool stillPresent = std::any_of(devices.begin(), devices.end(),
                                              [&](const DeviceInfo& d) { return d.link == link; });
        if (!stillPresent)
            ShutdownActiveSource();
    }
    SetEvent(wakeEvent_);
}

bool CaptureEngine::HasRequest()
{
    std::lock_guard lock(requestMutex_);
    return request_.active;
}

std::wstring CaptureEngine::Status()
{
    std::lock_guard lock(statusMutex_);
    return status_;
}

std::wstring CaptureEngine::ActiveName()
{
    std::lock_guard lock(sourceMutex_);
    return activeName_;
}

std::wstring CaptureEngine::ActiveLink()
{
    std::lock_guard lock(sourceMutex_);
    return activeLink_;
}

std::vector<FormatInfo> CaptureEngine::Formats()
{
    std::lock_guard lock(sourceMutex_);
    return formats_;
}

bool CaptureEngine::CurrentFormat(FormatInfo& format)
{
    std::lock_guard lock(sourceMutex_);
    format = current_;
    return hasCurrent_;
}

std::vector<DeviceInfo> CaptureEngine::EnumerateDevices()
{
    std::vector<DeviceInfo> result;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return result;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)))
        return result;

    for (UINT32 i = 0; i < count; ++i)
    {
        DeviceInfo info;
        info.name = GetString(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        info.link = GetString(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        ClassifyDevice(info);
        result.push_back(std::move(info));
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    std::stable_sort(result.begin(), result.end(),
                     [](const DeviceInfo& a, const DeviceInfo& b) { return a.score > b.score; });
    return result;
}

void CaptureEngine::SetStatus(std::wstring text)
{
    {
        std::lock_guard lock(statusMutex_);
        status_ = std::move(text);
    }
    PostMessageW(notifyWindow_, WM_APP_STATUS, 0, 0);
}

void CaptureEngine::ClearFrame()
{
    std::lock_guard lock(frameMutex_);
    if (frame_.width)
    {
        frame_.width = frame_.height = 0;
        ++frame_.sequence;
    }
}

void CaptureEngine::ShutdownActiveSource()
{
    std::lock_guard lock(sourceMutex_);
    if (activeSource_)
        activeSource_->Shutdown();
}

void CaptureEngine::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!stop_)
    {
        Request request;
        {
            std::lock_guard lock(requestMutex_);
            request = request_;
            restart_ = false;
        }
        if (!request.active)
        {
            ClearFrame();
            SetStatus(L"No source selected");
            WaitForSingleObject(wakeEvent_, INFINITE);
            continue;
        }

        const auto devices = EnumerateDevices();
        std::vector<DeviceInfo> candidates;
        for (const DeviceInfo& d : devices)
            if (request.link.empty() || d.link == request.link)
                candidates.push_back(d);

        if (candidates.empty())
        {
            ClearFrame();
            SetStatus(request.link.empty() ? std::wstring(L"Waiting for a USB capture device...")
                                           : std::format(L"Waiting for {} to reconnect...", request.name));
            WaitForSingleObject(wakeEvent_, 2000);
            continue;
        }

        bool streamed = false;
        for (const DeviceInfo& d : candidates)
        {
            if (stop_ || restart_)
                break;
            if (StreamDevice(d, request.format))
            {
                streamed = true;
                break;
            }
        }

        ClearFrame();
        if (stop_ || restart_)
            continue;
        if (!streamed)
        {
            SetStatus(L"Could not open the device (in use by another app?). Retrying...");
            WaitForSingleObject(wakeEvent_, 2000);
        }
        else
        {
            SetStatus(L"Signal lost. Reconnecting...");
            WaitForSingleObject(wakeEvent_, 500); // let the device list settle
        }
    }

    CoUninitialize();
}

// Returns true if the device was opened and streamed (until it stopped), false
// if it could not be opened at all.
bool CaptureEngine::StreamDevice(const DeviceInfo& device, const FormatRequest& request)
{
    SetStatus(std::format(L"Connecting to {}...", device.name));

    ComPtr<IMFMediaSource> source;
    if (FAILED(CreateSourceFromLink(device.link, &source)))
        return false;

    {
        std::lock_guard lock(sourceMutex_);
        activeSource_ = source;
        activeLink_ = device.link;
    }
    auto releaseSource = [&] {
        std::lock_guard lock(sourceMutex_);
        activeSource_->Shutdown();
        activeSource_.Reset();
        activeLink_.clear();
        activeName_.clear();
        formats_.clear();
        hasCurrent_ = false;
        measuredFps_ = 0.0;
    };

    // Stop()/Select() may have run before activeSource_ was published.
    if (stop_ || restart_)
    {
        releaseSource();
        return true;
    }

    ComPtr<IMFMediaTypeHandler> handler;
    std::vector<NativeType> types;
    size_t chosen = SIZE_MAX;
    if (SUCCEEDED(GetTypeHandler(source.Get(), &handler)))
    {
        types = ListNativeTypes(handler.Get());
        chosen = ChooseFormat(types, request);
    }

    ComPtr<IMFAttributes> readerAttrs;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> outputType;
    bool ok = chosen != SIZE_MAX && SUCCEEDED(handler->SetCurrentMediaType(types[chosen].type.Get())) &&
              SUCCEEDED(MFCreateAttributes(&readerAttrs, 2));
    if (ok)
    {
        // Lets the reader insert the MJPEG/H.264 decoder and YUV->RGB32 converter.
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        ok = SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader)) &&
             SUCCEEDED(MFCreateMediaType(&outputType));
    }
    if (ok)
    {
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        ok = SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                    nullptr, outputType.Get()));
    }
    if (!ok)
    {
        reader.Reset();
        releaseSource();
        return false;
    }

    const FormatInfo format = types[chosen].info;
    {
        std::lock_guard lock(sourceMutex_);
        activeName_ = device.name;
        formats_.clear();
        for (const NativeType& t : types)
        {
            const bool duplicate = std::any_of(formats_.begin(), formats_.end(), [&](const FormatInfo& f) {
                return f.width == t.info.width && f.height == t.info.height && f.fpsX100 == t.info.fpsX100 &&
                       f.subtype == t.info.subtype;
            });
            if (!duplicate)
                formats_.push_back(t.info);
        }
        current_ = format;
        hasCurrent_ = true;
    }

    UINT32 width = 0, height = 0;
    LONG defaultStride = 0;
    auto readOutputType = [&] {
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current)))
        {
            MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
            defaultStride = static_cast<LONG>(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
        }
    };
    readOutputType();

    const std::wstring baseStatus = std::format(L"{} — {}×{} {} @ {} fps", device.name, format.width,
                                                format.height, SubtypeName(format.subtype), FpsText(format.fpsX100));
    SetStatus(baseStatus);

    auto windowStart = std::chrono::steady_clock::now();
    int framesInWindow = 0;

    while (!stop_ && !restart_)
    {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex,
                                        &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
            break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            readOutputType();
        if (!sample || !width || !height)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer)))
            continue;

        backFrame_.width = width;
        backFrame_.height = height;
        backFrame_.pixels.resize(static_cast<size_t>(width) * height);
        const size_t rowBytes = static_cast<size_t>(width) * 4;

        auto copyRows = [&](const BYTE* scan0, LONG pitch) {
            for (UINT32 y = 0; y < height; ++y)
                std::memcpy(&backFrame_.pixels[static_cast<size_t>(y) * width], scan0 + static_cast<ptrdiff_t>(y) * pitch,
                            rowBytes);
        };

        bool copied = false;
        ComPtr<IMF2DBuffer> buffer2d;
        if (SUCCEEDED(buffer.As(&buffer2d)))
        {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
            {
                copyRows(scan0, pitch);
                buffer2d->Unlock2D();
                copied = true;
            }
        }
        else
        {
            BYTE* data = nullptr;
            DWORD length = 0;
            const size_t absStride = static_cast<size_t>(std::abs(defaultStride));
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
            {
                if (length >= absStride * height && absStride >= rowBytes)
                {
                    // Negative stride means bottom-up rows.
                    const BYTE* scan0 = defaultStride < 0 ? data + absStride * (height - 1) : data;
                    copyRows(scan0, defaultStride);
                    copied = true;
                }
                buffer->Unlock();
            }
        }
        if (!copied)
            continue;

        {
            std::lock_guard lock(frameMutex_);
            backFrame_.sequence = frame_.sequence + 1;
            std::swap(frame_, backFrame_);
        }

        ++framesInWindow;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - windowStart).count();
        if (elapsed >= 1.0)
        {
            measuredFps_ = framesInWindow / elapsed;
            SetStatus(std::format(L"{} (actual {:.1f} fps)", baseStatus, measuredFps_.load()));
            windowStart = now;
            framesInWindow = 0;
        }
    }

    reader.Reset();
    releaseSource();
    return true;
}
