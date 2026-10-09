#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

namespace
{
constexpr float kVirtualW = 1280.0f;
constexpr float kVirtualH = 720.0f;
constexpr float kListX = 110.0f;
constexpr float kListRight = 1170.0f;
constexpr float kListTop = 168.0f;
constexpr float kRowH = 58.0f;
constexpr int kVisibleRows = 7;
constexpr float kValueX = 720.0f;
constexpr double kSplashLength = 4.4;
constexpr double kCountdownLength = 5.0;
constexpr double kExitLength = 0.5;
constexpr double kOsdLength = 4.0;
constexpr wchar_t kAutoDevice[] = L"auto";

constexpr D2D1_COLOR_F Color(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }

float Smooth(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return static_cast<float>(t * t * (3 - 2 * t));
}

float Pulse(double now, double speed, double phase = 0.0)
{
    return 0.5f + 0.5f * static_cast<float>(std::sin(now * speed + phase));
}

bool Inside(const D2D1_RECT_F& r, D2D1_POINT_2F p)
{
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// "VID 534D · PID 2109" from a USB symbolic link.
std::wstring ShortId(const std::wstring& link)
{
    std::wstring lower = link;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const size_t vid = lower.find(L"vid_");
    const size_t pid = lower.find(L"pid_");
    if (vid == std::wstring::npos || pid == std::wstring::npos || vid + 8 > lower.size() || pid + 8 > lower.size())
        return L"Software device";
    std::wstring v = link.substr(vid + 4, 4), p = link.substr(pid + 4, 4);
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    std::transform(p.begin(), p.end(), p.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return std::format(L"VID {} · PID {}", v, p);
}

const wchar_t* PageTitle(int page)
{
    static const wchar_t* titles[] = {L"Select Source", L"Captura", L"Video Quality", L"Picture", L"Audio", L"Display"};
    return titles[page];
}
} // namespace

Ui::Ui(HWND hwnd, CaptureEngine& engine, AudioEngine& audio, Renderer& renderer, Settings& settings)
    : hwnd_(hwnd), engine_(engine), audio_(audio), renderer_(renderer), settings_(settings)
{
    IDWriteFactory* dw = renderer_.DWrite();
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                             L"en-us", &out);
    };
    make(40, DWRITE_FONT_WEIGHT_LIGHT, fTitle_);
    make(24, DWRITE_FONT_WEIGHT_SEMI_LIGHT, fItem_);
    make(15, DWRITE_FONT_WEIGHT_NORMAL, fDetail_);
    make(17, DWRITE_FONT_WEIGHT_NORMAL, fHint_);
    make(104, DWRITE_FONT_WEIGHT_LIGHT, fLogo_);
    make(19, DWRITE_FONT_WEIGHT_NORMAL, fSubtitle_);
    make(30, DWRITE_FONT_WEIGHT_LIGHT, fClock_);
    make(28, DWRITE_FONT_WEIGHT_SEMI_LIGHT, fOsd_);
}

// ---- State -----------------------------------------------------------------

void Ui::Start(double now)
{
    now_ = now;
    splashStart_ = now;
    splash_ = true;
}

void Ui::Update(double now)
{
    now_ = now;
    UpdateAudio();
    if (splash_ && now - splashStart_ >= kSplashLength)
        EndSplash();

    if (countdownEnd_ > 0 && now >= countdownEnd_)
    {
        countdownEnd_ = 0;
        if (launching_ && stack_.size() == 1 && stack_.back() == Page::Sources)
        {
            auto items = BuildItems(Page::Sources);
            ClampSelection(items);
            if (items[Selected()].activate)
                items[Selected()].activate();
        }
    }

    if (exiting_ && !closePosted_ && now - exitStart_ >= kExitLength)
    {
        closePosted_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    const std::wstring name = engine_.ActiveName();
    if (name != lastActiveName_)
    {
        lastActiveName_ = name;
        if (!name.empty())
            osdUntil_ = now + kOsdLength;
    }
}

RenderParams Ui::Params(double now) const
{
    RenderParams p;
    p.time = static_cast<float>(std::fmod(now, 1000.0));
    p.settings = &settings_;
    const bool video = renderer_.HasVideo();
    if (splash_)
    {
        const double t = now - splashStart_;
        p.backgroundAlpha = Smooth(0.5, 1.6, t);
        p.crt = settings_.crtInMenus;
        p.power = settings_.crtInMenus ? static_cast<float>(std::clamp(t / 0.9, 0.0, 1.0)) : 1.0f;
    }
    else if (!stack_.empty())
    {
        p.drawVideo = video;
        p.backgroundAlpha = video ? 0.8f : 1.0f;
        p.crt = settings_.crtInMenus;
    }
    else
    {
        p.drawVideo = video;
        p.backgroundAlpha = video ? 0.0f : 1.0f;
        p.crt = video ? settings_.crtOnVideo : settings_.crtInMenus;
    }
    if (exiting_)
    {
        p.crt = true;
        p.power = 1.0f - static_cast<float>(std::clamp((now - exitStart_) / (kExitLength * 0.9), 0.0, 1.0));
    }
    return p;
}

int& Ui::Selected()
{
    return selected_[static_cast<int>(stack_.empty() ? Page::Main : stack_.back())];
}

void Ui::ClampSelection(const std::vector<Item>& items)
{
    int& sel = Selected();
    const int n = static_cast<int>(items.size());
    sel = std::clamp(sel, 0, std::max(0, n - 1));
    if (n && items[sel].kind == Kind::Info)
    {
        for (int i = sel; i < n; ++i)
            if (items[i].kind != Kind::Info)
                return void(sel = i);
        for (int i = sel; i >= 0; --i)
            if (items[i].kind != Kind::Info)
                return void(sel = i);
    }
}

void Ui::Move(const std::vector<Item>& items, int direction)
{
    int& sel = Selected();
    for (int i = sel + direction; i >= 0 && i < static_cast<int>(items.size()); i += direction)
        if (items[i].kind != Kind::Info)
        {
            sel = i;
            return;
        }
}

void Ui::Push(Page page)
{
    stack_.push_back(page);
    selected_[static_cast<int>(page)] = 0;
    scroll_[static_cast<int>(page)] = 0;
    fadeStart_ = now_;
    hits_.clear();
    if (page == Page::Sources)
    {
        RefreshDevices();
        // Highlight the current source.
        const std::wstring live = engine_.ActiveLink();
        for (size_t i = 0; i < devices_.size(); ++i)
            if (devices_[i].link == live)
                selected_[static_cast<int>(page)] = static_cast<int>(i);
    }
    if (page == Page::Quality)
        pending_ = settings_.format;
    if (page == Page::Audio)
        RefreshAudioDevices();
}

void Ui::Back()
{
    if (stack_.size() > 1)
    {
        stack_.pop_back();
        fadeStart_ = now_;
        hits_.clear();
    }
    else if (!launching_)
    {
        CloseMenu();
    }
}

void Ui::OpenMenu()
{
    stack_.clear();
    Push(Page::Main);
}

void Ui::CloseMenu()
{
    stack_.clear();
    hits_.clear();
    dragging_ = false;
}

void Ui::EndSplash()
{
    splash_ = false;
    launching_ = true;
    stack_.clear();
    Push(Page::Sources);

    // Preselect the last used source (or the best one) and auto-start it.
    int sel = 0;
    if (settings_.lastDevice == kAutoDevice)
        sel = devices_.empty() ? 1 : static_cast<int>(devices_.size());
    else
        for (size_t i = 0; i < devices_.size(); ++i)
            if (devices_[i].link == settings_.lastDevice)
                sel = static_cast<int>(i);
    selected_[static_cast<int>(Page::Sources)] = sel;
    if (!devices_.empty())
        countdownEnd_ = now_ + kCountdownLength;
}

void Ui::BeginExit()
{
    exiting_ = true;
    exitStart_ = now_;
}

void Ui::StartDevice(const std::wstring& link, const std::wstring& name)
{
    if (link != settings_.lastDevice)
        settings_.format = {};
    settings_.lastDevice = link;
    engine_.Select(link == kAutoDevice ? std::wstring() : link, name, settings_.format);
    settings_.Save();
    launching_ = false;
    countdownEnd_ = 0;
    lastActiveName_.clear(); // show the OSD again even for the same device
    CloseMenu();
}

void Ui::RefreshDevices()
{
    int& sel = selected_[static_cast<int>(Page::Sources)];
    std::wstring selectedLink;
    if (sel >= 0 && sel < static_cast<int>(devices_.size()))
        selectedLink = devices_[sel].link;
    devices_ = CaptureEngine::EnumerateDevices();
    for (size_t i = 0; i < devices_.size(); ++i)
        if (!selectedLink.empty() && devices_[i].link == selectedLink)
            sel = static_cast<int>(i);
}

void Ui::OnDevicesChanged()
{
    RefreshDevices();
    RefreshAudioDevices();
    engine_.Rescan();
    audio_.Rescan();
}

// ---- Menu contents -----------------------------------------------------------

Ui::Item Ui::MakeInfo(std::wstring label, std::wstring detail)
{
    Item it;
    it.kind = Kind::Info;
    it.label = std::move(label);
    it.detail = std::move(detail);
    return it;
}

Ui::Item Ui::MakeSlider(std::wstring label, std::wstring detail, int& value, int lo, int hi, int step,
                        const std::function<std::wstring(int)>& format)
{
    Item it;
    it.kind = Kind::Slider;
    it.label = std::move(label);
    it.detail = std::move(detail);
    it.value = format(value);
    it.fraction = static_cast<float>(value - lo) / static_cast<float>(hi - lo);
    int* v = &value;
    it.adjust = [v, lo, hi, step](int d) { *v = std::clamp(*v + d * step, lo, hi); };
    it.setFraction = [v, lo, hi](float f) { *v = std::clamp(lo + static_cast<int>(std::lround(f * (hi - lo))), lo, hi); };
    return it;
}

Ui::Item Ui::MakeToggle(std::wstring label, std::wstring detail, bool value, std::function<void()> flip)
{
    Item it;
    it.kind = Kind::Choice;
    it.label = std::move(label);
    it.detail = std::move(detail);
    it.value = value ? L"On" : L"Off";
    it.adjust = [flip, value](int d) {
        if ((d > 0) != value)
            flip();
    };
    it.activate = std::move(flip);
    return it;
}

std::vector<Ui::Item> Ui::BuildItems(Page page)
{
    std::vector<Item> items;
    switch (page)
    {
    case Page::Sources: BuildSources(items); break;
    case Page::Main: BuildMain(items); break;
    case Page::Quality: BuildQuality(items); break;
    case Page::Picture: BuildPicture(items); break;
    case Page::Audio: BuildAudio(items); break;
    case Page::Display: BuildDisplay(items); break;
    default: break;
    }
    return items;
}

void Ui::BuildSources(std::vector<Item>& items)
{
    const std::wstring live = engine_.ActiveLink();
    for (size_t i = 0; i < devices_.size(); ++i)
    {
        const DeviceInfo& d = devices_[i];
        const bool best = i == 0 && d.score > 0;
        Item it;
        it.label = d.name.empty() ? L"Unnamed device" : d.name;
        it.detail = d.kind + (best ? L"  ·  Best match" : L"") + L"  ·  " + ShortId(d.link);
        if (d.link == live)
        {
            it.value = L"● LIVE";
            it.good = true;
        }
        else if (best)
        {
            it.value = L"RECOMMENDED";
        }
        it.activate = [this, d] { StartDevice(d.link, d.name); };
        items.push_back(std::move(it));
    }
    if (devices_.empty())
        items.push_back(MakeInfo(L"No video devices found",
                                 L"Connect a USB capture card — it will appear here automatically"));

    Item autoItem;
    autoItem.label = L"Auto-detect";
    autoItem.detail = devices_.empty() ? L"Use the best device as soon as one is connected"
                                       : L"Always use the best available device (now: " + devices_[0].name + L")";
    if (settings_.lastDevice == kAutoDevice && engine_.HasRequest())
        autoItem.value = L"ACTIVE";
    autoItem.activate = [this] { StartDevice(kAutoDevice, L"Auto-detect"); };
    items.push_back(std::move(autoItem));

    Item rescan;
    rescan.label = L"Rescan devices";
    rescan.detail = L"Search for newly connected sources";
    rescan.activate = [this] { OnDevicesChanged(); };
    items.push_back(std::move(rescan));

    if (launching_)
    {
        Item settingsItem;
        settingsItem.label = L"Settings";
        settingsItem.detail = L"Picture and display options";
        settingsItem.activate = [this] { Push(Page::Main); };
        items.push_back(std::move(settingsItem));

        Item exit;
        exit.label = L"Exit Captura";
        exit.activate = [this] { BeginExit(); };
        items.push_back(std::move(exit));
    }
}

void Ui::BuildMain(std::vector<Item>& items)
{
    if (engine_.HasRequest() && !launching_)
    {
        Item resume;
        resume.label = L"Resume";
        resume.detail = L"Back to the picture";
        resume.activate = [this] { CloseMenu(); };
        items.push_back(std::move(resume));
    }

    const std::wstring live = engine_.ActiveName();
    Item source;
    source.label = L"Source";
    source.detail = live.empty() ? L"Choose a capture device" : L"Now: " + live;
    source.activate = [this] { Push(Page::Sources); };
    items.push_back(std::move(source));

    Item quality;
    quality.label = L"Video Quality";
    quality.detail = L"Resolution, frame rate and format";
    quality.activate = [this] { Push(Page::Quality); };
    items.push_back(std::move(quality));

    Item picture;
    picture.label = L"Picture";
    picture.detail = L"Brightness, contrast, color and sharpness";
    picture.activate = [this] { Push(Page::Picture); };
    items.push_back(std::move(picture));

    Item audio;
    audio.label = L"Audio";
    audio.detail = L"Volume, input and output devices, latency";
    audio.activate = [this] { Push(Page::Audio); };
    items.push_back(std::move(audio));

    Item display;
    display.label = L"Display";
    display.detail = L"Color palette, CRT filter, scanlines and fullscreen";
    display.activate = [this] { Push(Page::Display); };
    items.push_back(std::move(display));

    Item exit;
    exit.label = L"Exit Captura";
    exit.activate = [this] { BeginExit(); };
    items.push_back(std::move(exit));
}

void Ui::ValidatePendingFormat()
{
    const auto formats = engine_.Formats();
    auto anyMatch = [&](auto pred) { return std::any_of(formats.begin(), formats.end(), pred); };
    auto sizeOk = [&](const FormatInfo& f) {
        return !pending_.width || (f.width == pending_.width && f.height == pending_.height);
    };
    auto fpsOk = [&](const FormatInfo& f) { return !pending_.fpsX100 || f.fpsX100 == pending_.fpsX100; };
    if (pending_.width && !anyMatch(sizeOk))
        pending_.width = pending_.height = 0;
    if (pending_.fpsX100 && !anyMatch([&](const FormatInfo& f) { return sizeOk(f) && fpsOk(f); }))
        pending_.fpsX100 = 0;
    if (pending_.subtype != GUID_NULL &&
        !anyMatch([&](const FormatInfo& f) { return sizeOk(f) && fpsOk(f) && f.subtype == pending_.subtype; }))
        pending_.subtype = GUID_NULL;
}

void Ui::BuildQuality(std::vector<Item>& items)
{
    const auto formats = engine_.Formats();
    FormatInfo cur{};
    if (formats.empty() || !engine_.CurrentFormat(cur))
    {
        items.push_back(MakeInfo(L"No active source", L"Start a source to choose its resolution and frame rate"));
        return;
    }

    auto sizeOk = [&](const FormatInfo& f) {
        return !pending_.width || (f.width == pending_.width && f.height == pending_.height);
    };
    auto fpsOk = [&](const FormatInfo& f) { return !pending_.fpsX100 || f.fpsX100 == pending_.fpsX100; };

    std::vector<std::pair<UINT32, UINT32>> sizes;
    std::vector<UINT32> rates;
    std::vector<GUID> subtypes;
    for (const FormatInfo& f : formats)
    {
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(f.width, f.height)) == sizes.end())
            sizes.emplace_back(f.width, f.height);
        if (sizeOk(f) && std::find(rates.begin(), rates.end(), f.fpsX100) == rates.end())
            rates.push_back(f.fpsX100);
        if (sizeOk(f) && fpsOk(f) && std::find(subtypes.begin(), subtypes.end(), f.subtype) == subtypes.end())
            subtypes.push_back(f.subtype);
    }
    std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) {
        return static_cast<UINT64>(a.first) * a.second > static_cast<UINT64>(b.first) * b.second;
    });
    std::sort(rates.begin(), rates.end(), std::greater<>());

    int sizeIndex = 0, rateIndex = 0, subtypeIndex = 0;
    for (size_t i = 0; i < sizes.size(); ++i)
        if (pending_.width == sizes[i].first && pending_.height == sizes[i].second)
            sizeIndex = static_cast<int>(i) + 1;
    for (size_t i = 0; i < rates.size(); ++i)
        if (pending_.fpsX100 == rates[i])
            rateIndex = static_cast<int>(i) + 1;
    for (size_t i = 0; i < subtypes.size(); ++i)
        if (pending_.subtype == subtypes[i])
            subtypeIndex = static_cast<int>(i) + 1;

    Item size;
    size.kind = Kind::Choice;
    size.label = L"Resolution";
    size.detail = L"Higher resolutions need more USB bandwidth";
    size.value = sizeIndex ? std::format(L"{}×{}", sizes[sizeIndex - 1].first, sizes[sizeIndex - 1].second)
                           : std::format(L"Auto ({}×{})", cur.width, cur.height);
    size.adjust = [this, sizes, sizeIndex](int d) {
        const int n = std::clamp(sizeIndex + d, 0, static_cast<int>(sizes.size()));
        pending_.width = n ? sizes[n - 1].first : 0;
        pending_.height = n ? sizes[n - 1].second : 0;
        ValidatePendingFormat();
    };
    items.push_back(std::move(size));

    Item rate;
    rate.kind = Kind::Choice;
    rate.label = L"Frame rate";
    rate.detail = L"Frames per second delivered by the device";
    rate.value = rateIndex ? FpsText(rates[rateIndex - 1]) + L" fps" : L"Auto (" + FpsText(cur.fpsX100) + L" fps)";
    rate.adjust = [this, rates, rateIndex](int d) {
        const int n = std::clamp(rateIndex + d, 0, static_cast<int>(rates.size()));
        pending_.fpsX100 = n ? rates[n - 1] : 0;
        ValidatePendingFormat();
    };
    items.push_back(std::move(rate));

    Item format;
    format.kind = Kind::Choice;
    format.label = L"Format";
    format.detail = L"MJPG is compressed; YUY2 / NV12 are uncompressed but need more bandwidth";
    format.value = subtypeIndex ? SubtypeName(subtypes[subtypeIndex - 1]) : L"Auto (" + SubtypeName(cur.subtype) + L")";
    format.adjust = [this, subtypes, subtypeIndex](int d) {
        const int n = std::clamp(subtypeIndex + d, 0, static_cast<int>(subtypes.size()));
        pending_.subtype = n ? subtypes[n - 1] : GUID_NULL;
    };
    items.push_back(std::move(format));

    const bool changed = !(pending_ == settings_.format);
    Item apply;
    apply.label = L"Apply";
    apply.detail = changed ? L"Restart the stream with the new mode" : L"These settings are active";
    if (now_ < appliedUntil_)
    {
        apply.value = L"✓ APPLIED";
        apply.good = true;
    }
    else if (changed)
    {
        apply.value = L"PENDING";
    }
    apply.activate = [this] {
        settings_.format = pending_;
        const std::wstring link = settings_.lastDevice == kAutoDevice ? std::wstring() : settings_.lastDevice;
        engine_.Select(link, engine_.ActiveName(), pending_);
        settings_.Save();
        appliedUntil_ = now_ + 2.0;
    };
    items.push_back(std::move(apply));

    items.push_back(MakeInfo(L"Now playing",
                             std::format(L"{}×{}  {}  @ {} fps  ·  measured {:.1f} fps", cur.width, cur.height,
                                         SubtypeName(cur.subtype), FpsText(cur.fpsX100), engine_.MeasuredFps())));
}

void Ui::BuildPicture(std::vector<Item>& items)
{
    PictureSettings& p = settings_.picture;
    auto signedValue = [](int v) { return std::format(L"{:+}", v); };
    auto percent = [](int v) { return std::format(L"{}%", v); };
    items.push_back(MakeSlider(L"Brightness", L"Lift or lower the whole picture", p.brightness, -100, 100, 5, signedValue));
    items.push_back(MakeSlider(L"Contrast", L"Distance between darks and lights", p.contrast, 0, 200, 5, percent));
    items.push_back(MakeSlider(L"Saturation", L"Color intensity", p.saturation, 0, 200, 5, percent));
    items.push_back(MakeSlider(L"Hue", L"Rotate all colors", p.hue, -180, 180, 5,
                               [](int v) { return std::format(L"{:+}°", v); }));
    items.push_back(MakeSlider(L"Gamma", L"Brighten or darken mid-tones", p.gamma, 50, 250, 5,
                               [](int v) { return std::format(L"{:.2f}", v / 100.0); }));
    items.push_back(MakeSlider(L"Sharpness", L"Crisper edges", p.sharpness, 0, 100, 5, percent));

    Item reset;
    reset.label = L"Reset picture";
    reset.detail = L"Restore default color settings";
    reset.activate = [this] { settings_.picture = {}; };
    items.push_back(std::move(reset));
}

void Ui::RefreshAudioDevices()
{
    audioIn_ = AudioEngine::EnumerateDevices(true);
    audioOut_ = AudioEngine::EnumerateDevices(false);
}

void Ui::UpdateAudio()
{
    // Remember the last real video device: it is briefly empty while the stream restarts.
    const std::wstring link = engine_.ActiveLink();
    if (!link.empty())
        audioVideoLink_ = link;

    AudioConfig config;
    config.enabled = settings_.audioEnabled;
    config.volume = settings_.audioVolume;
    config.muted = settings_.audioMuted;
    config.input = settings_.audioInput.empty() ? std::wstring(kAutoAudioInput) : settings_.audioInput;
    config.output = settings_.audioOutput;
    static const int latencies[] = {40, 80, 160};
    config.latencyMs = latencies[std::clamp(settings_.audioLatency, 0, 2)];
    config.videoLink = audioVideoLink_;
    for (const DeviceInfo& d : devices_)
        if (d.link == audioVideoLink_)
            config.allowAuto = d.kind != L"Webcam" && d.kind != L"Virtual camera";
    audio_.Configure(config);
}

void Ui::AdjustVolume(int delta)
{
    settings_.audioVolume = std::clamp(settings_.audioVolume + delta, 0, 200);
    if (delta > 0)
        settings_.audioMuted = false;
    volumeUntil_ = now_ + 1.8;
}

void Ui::ToggleMute()
{
    settings_.audioMuted = !settings_.audioMuted;
    volumeUntil_ = now_ + 1.8;
}

void Ui::BuildAudio(std::vector<Item>& items)
{
    items.push_back(MakeToggle(L"Audio", L"Play the source's sound through your speakers", settings_.audioEnabled,
                               [this] { settings_.audioEnabled = !settings_.audioEnabled; }));
    items.push_back(MakeSlider(L"Volume", L"Software gain; above 100% boosts quiet capture cards", settings_.audioVolume,
                               0, 200, 5, [](int v) { return std::format(L"{}%", v); }));
    items.push_back(MakeToggle(L"Mute", L"Silence the audio without stopping capture", settings_.audioMuted,
                               [this] { settings_.audioMuted = !settings_.audioMuted; }));

    // A choice that cycles through "default" followed by every device.
    auto deviceItem = [&](const wchar_t* label, const wchar_t* detail, std::wstring* setting,
                          const std::vector<AudioDevice>* list, const std::wstring& defaultId,
                          const std::wstring& defaultLabel) {
        int index = 0;
        if (*setting != defaultId)
        {
            index = -1;
            for (size_t i = 0; i < list->size(); ++i)
                if ((*list)[i].id == *setting)
                    index = static_cast<int>(i) + 1;
        }
        Item it;
        it.kind = Kind::Choice;
        it.label = label;
        it.detail = detail;
        it.value = index < 0 ? L"Not connected" : index == 0 ? defaultLabel : (*list)[index - 1].name;
        it.adjust = [setting, list, defaultId, index](int d) {
            const int count = static_cast<int>(list->size()) + 1;
            const int next = ((std::max(index, 0) + d) % count + count) % count;
            *setting = next == 0 ? defaultId : (*list)[next - 1].id;
        };
        items.push_back(std::move(it));
    };
    deviceItem(L"Input", L"Auto picks the audio that belongs to your capture card", &settings_.audioInput, &audioIn_,
               kAutoAudioInput, L"Auto (capture card)");
    deviceItem(L"Output", L"Where the sound is played", &settings_.audioOutput, &audioOut_, std::wstring(),
               L"System default");

    Item latency;
    latency.kind = Kind::Choice;
    latency.label = L"Latency";
    latency.detail = L"Lower stays in sync with the picture; higher avoids crackling";
    static const wchar_t* latencyNames[] = {L"Low (40 ms)", L"Normal (80 ms)", L"High (160 ms)"};
    const int latencyIndex = std::clamp(settings_.audioLatency, 0, 2);
    latency.value = latencyNames[latencyIndex];
    latency.adjust = [this, latencyIndex](int d) { settings_.audioLatency = std::clamp(latencyIndex + d, 0, 2); };
    items.push_back(std::move(latency));

    // Input level in dB, mapped from -60..0 to the bar.
    const float peak = std::max(audio_.Level(), 1e-4f);
    Item level;
    level.kind = Kind::Info;
    level.meter = true;
    level.label = L"Input level";
    level.detail = audio_.Status();
    level.fraction = std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
    items.push_back(std::move(level));
}

void Ui::BuildDisplay(std::vector<Item>& items)
{
    auto percent = [](int v) { return std::format(L"{}%", v); };

    Item palette;
    palette.kind = Kind::Choice;
    palette.label = L"Color palette";
    palette.detail = L"Recolors the menus, background and highlights";
    palette.value = GetPalette(settings_.palette).name;
    palette.adjust = [this](int d) { settings_.palette = (settings_.palette + d + kPaletteCount) % kPaletteCount; };
    palette.activate = [this] { settings_.palette = (settings_.palette + 1) % kPaletteCount; };
    items.push_back(std::move(palette));

    items.push_back(MakeToggle(L"CRT filter on video", L"Scanlines and screen glow over the live picture",
                               settings_.crtOnVideo, [this] { settings_.crtOnVideo = !settings_.crtOnVideo; }));
    items.push_back(MakeToggle(L"CRT effect in menus", L"Old-TV look for the menus and splash screen",
                               settings_.crtInMenus, [this] { settings_.crtInMenus = !settings_.crtInMenus; }));
    items.push_back(MakeSlider(L"Scanlines", L"Strength of scanlines and phosphor mask", settings_.scanlines, 0, 100, 5, percent));
    items.push_back(MakeSlider(L"Screen curvature", L"How rounded the virtual tube is", settings_.curvature, 0, 100, 5, percent));
    items.push_back(MakeToggle(L"Fullscreen", L"F11 or Alt+Enter also toggles", settings_.fullscreen, [this] {
        if (onToggleFullscreen)
            onToggleFullscreen();
    }));
    items.push_back(MakeToggle(L"Show FPS counter", L"Measured capture frame rate in the corner", settings_.showFps,
                               [this] { settings_.showFps = !settings_.showFps; }));
}

// ---- Input -------------------------------------------------------------------

void Ui::OnAction(Action action)
{
    if (exiting_)
        return;
    countdownEnd_ = 0; // any input cancels the auto-start
    if (splash_)
    {
        if (now_ - splashStart_ > 0.3)
            EndSplash();
        countdownEnd_ = 0;
        return;
    }
    if (stack_.empty())
    {
        if (action == Action::Up)
            AdjustVolume(5);
        else if (action == Action::Down)
            AdjustVolume(-5);
        else if (action == Action::Left || action == Action::Right)
            ToggleMute();
        else
            OpenMenu();
        return;
    }
    if (action == Action::Menu)
    {
        if (launching_)
            Push(Page::Main);
        else
            CloseMenu();
        return;
    }

    auto items = BuildItems(stack_.back());
    ClampSelection(items);
    const int sel = Selected();
    Item* item = items.empty() ? nullptr : &items[sel];
    switch (action)
    {
    case Action::Up: Move(items, -1); break;
    case Action::Down: Move(items, 1); break;
    case Action::Left:
    case Action::Right:
        if (item && item->adjust)
            item->adjust(action == Action::Left ? -1 : 1);
        break;
    case Action::Confirm:
        if (item && item->activate)
            item->activate();
        else if (item && item->kind == Kind::Choice && item->adjust)
            item->adjust(1);
        break;
    case Action::Back: Back(); break;
    default: break;
    }
}

D2D1_POINT_2F Ui::ToVirtual(int x, int y) const
{
    return {(x - offsetX_) / scale_, (y - offsetY_) / scale_};
}

void Ui::OnMouseMove(int x, int y)
{
    if (splash_ || stack_.empty() || exiting_)
        return;
    const D2D1_POINT_2F p = ToVirtual(x, y);
    if (dragging_)
    {
        for (const RowHit& hit : hits_)
            if (hit.index == dragIndex_ && hit.hasBar)
            {
                auto items = BuildItems(stack_.back());
                const float f = (p.x - (hit.bar.left + 10)) / (hit.bar.right - hit.bar.left - 20);
                if (dragIndex_ < static_cast<int>(items.size()) && items[dragIndex_].setFraction)
                    items[dragIndex_].setFraction(std::clamp(f, 0.0f, 1.0f));
            }
        return;
    }
    for (const RowHit& hit : hits_)
        if (Inside(hit.row, p))
        {
            auto items = BuildItems(stack_.back());
            if (hit.index < static_cast<int>(items.size()) && items[hit.index].kind != Kind::Info &&
                Selected() != hit.index)
            {
                Selected() = hit.index;
                countdownEnd_ = 0;
            }
            return;
        }
}

void Ui::OnMouseDown(int x, int y)
{
    if (exiting_)
        return;
    countdownEnd_ = 0;
    if (splash_)
    {
        OnAction(Action::Confirm);
        return;
    }
    if (stack_.empty())
        return;
    const D2D1_POINT_2F p = ToVirtual(x, y);
    for (const RowHit hit : hits_) // copy: activating may clear hits_
    {
        if (!Inside(hit.row, p))
            continue;
        auto items = BuildItems(stack_.back());
        if (hit.index >= static_cast<int>(items.size()) || items[hit.index].kind == Kind::Info)
            return;
        Selected() = hit.index;
        Item& item = items[hit.index];
        if (item.kind == Kind::Slider && hit.hasBar && Inside(hit.bar, p))
        {
            dragging_ = true;
            dragIndex_ = hit.index;
            OnMouseMove(x, y);
        }
        else if (item.kind == Kind::Choice && p.x >= hit.valueX && item.adjust)
            item.adjust(p.x < (hit.valueX + hit.row.right) * 0.5f ? -1 : 1);
        else if (item.activate)
            item.activate();
        else if (item.adjust)
            item.adjust(1);
        return;
    }
}

void Ui::OnMouseUp()
{
    dragging_ = false;
    dragIndex_ = -1;
}

void Ui::OnWheel(int delta)
{
    if (!splash_ && stack_.empty() && !exiting_ && delta)
        AdjustVolume(delta > 0 ? 5 : -5);
    if (splash_ || stack_.empty() || exiting_ || !delta)
        return;
    countdownEnd_ = 0;
    auto items = BuildItems(stack_.back());
    ClampSelection(items);
    if (items.empty())
        return;
    Item& item = items[Selected()];
    if ((item.kind == Kind::Slider || item.kind == Kind::Choice) && item.adjust)
        item.adjust(delta > 0 ? 1 : -1);
    else
        Move(items, delta > 0 ? -1 : 1);
}

void Ui::OnRightClick()
{
    if (splash_)
        OnAction(Action::Confirm);
    else if (stack_.empty())
        OpenMenu();
    else
        OnAction(Action::Back);
}

// ---- Drawing helpers ---------------------------------------------------------

void Ui::ApplyPalette()
{
    const Palette& p = GetPalette(settings_.palette);
    auto c = [](const Rgb& v) { return Color(v.r, v.g, v.b); };
    cText_ = c(p.text);
    cDim_ = c(p.dim);
    cAccent_ = c(p.accent);
    cLive_ = c(p.live);
    cPanel_ = c(p.panel);
}

D2D1_COLOR_F Ui::WithAlpha(D2D1_COLOR_F color, float alpha)
{
    return {color.r, color.g, color.b, alpha};
}

void Ui::UpdateTransform()
{
    const float w = static_cast<float>(renderer_.Width());
    const float h = static_cast<float>(renderer_.Height());
    scale_ = std::min(w / kVirtualW, h / kVirtualH);
    offsetX_ = (w - kVirtualW * scale_) * 0.5f;
    offsetY_ = (h - kVirtualH * scale_) * 0.5f;
}

void Ui::EnsureResources()
{
    ID2D1DeviceContext* dc = renderer_.D2D();
    if (!brush_)
        dc->CreateSolidColorBrush(cText_, &brush_);
    if (lineBrush_ && resourcePalette_ == settings_.palette)
        return;

    // The glowing divider line is a gradient in the accent color.
    const D2D1_GRADIENT_STOP lineStops[] = {{0.0f, WithAlpha(cAccent_, 0.0f)},
                                            {0.15f, WithAlpha(cAccent_, 0.9f)},
                                            {0.6f, WithAlpha(cAccent_, 0.45f)},
                                            {1.0f, WithAlpha(cAccent_, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> stops;
    dc->CreateGradientStopCollection(lineStops, ARRAYSIZE(lineStops), &stops);
    lineBrush_.Reset();
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}), stops.Get(), &lineBrush_);

    // Fill of the selected row's bar.
    const D2D1_GRADIENT_STOP barStops[] = {{0.0f, WithAlpha(cAccent_, 0.42f)},
                                           {0.7f, WithAlpha(cAccent_, 0.16f)},
                                           {1.0f, WithAlpha(cAccent_, 0.03f)}};
    stops.Reset();
    dc->CreateGradientStopCollection(barStops, ARRAYSIZE(barStops), &stops);
    barBrush_.Reset();
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}), stops.Get(), &barBrush_);
    resourcePalette_ = settings_.palette;
}

void Ui::Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect, D2D1_COLOR_F color,
              DWRITE_TEXT_ALIGNMENT align, float glow, DWRITE_PARAGRAPH_ALIGNMENT valign, bool wrap)
{
    if (text.empty() || alpha_ <= 0.0f)
        return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(renderer_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                                    std::max(1.0f, rect.right - rect.left),
                                                    std::max(1.0f, rect.bottom - rect.top), &layout)))
        return;
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(valign);
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (!wrap && SUCCEEDED(renderer_.DWrite()->CreateEllipsisTrimmingSign(format, &ellipsis)))
    {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.Get());
    }

    ID2D1DeviceContext* dc = renderer_.D2D();
    const auto options = D2D1_DRAW_TEXT_OPTIONS_CLIP;
    if (glow > 0.0f)
    {
        brush_->SetColor(Color(cAccent_.r, cAccent_.g, cAccent_.b, color.a * alpha_ * glow * 0.14f));
        for (int k = 0; k < 8; ++k)
        {
            const float a = k * 0.785398f;
            dc->DrawTextLayout({rect.left + 2.5f * std::cos(a), rect.top + 2.5f * std::sin(a)}, layout.Get(),
                               brush_.Get(), options);
        }
    }
    brush_->SetColor(Color(color.r, color.g, color.b, color.a * alpha_));
    dc->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush_.Get(), options);
}

float Ui::Measure(const std::wstring& text, IDWriteTextFormat* format)
{
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(renderer_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4000, 200,
                                                    &layout)) ||
        FAILED(layout->GetMetrics(&metrics)))
        return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

void Ui::FillRound(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color)
{
    brush_->SetColor(Color(color.r, color.g, color.b, color.a * alpha_));
    renderer_.D2D()->FillRoundedRectangle({rect, radius, radius}, brush_.Get());
}

void Ui::StrokeRound(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float width)
{
    brush_->SetColor(Color(color.r, color.g, color.b, color.a * alpha_));
    renderer_.D2D()->DrawRoundedRectangle({rect, radius, radius}, brush_.Get(), width);
}

void Ui::Orb(D2D1_POINT_2F center, float radius, D2D1_COLOR_F color)
{
    ID2D1DeviceContext* dc = renderer_.D2D();
    for (int k = 3; k >= 1; --k)
    {
        const float r = radius * (1.0f + k * 0.7f);
        brush_->SetColor(Color(color.r, color.g, color.b, color.a * alpha_ * 0.10f));
        dc->FillEllipse({center, r, r}, brush_.Get());
    }
    brush_->SetColor(Color(color.r, color.g, color.b, color.a * alpha_));
    dc->FillEllipse({center, radius, radius}, brush_.Get());
    brush_->SetColor(Color(1, 1, 1, 0.8f * color.a * alpha_));
    dc->FillEllipse({center, radius * 0.45f, radius * 0.45f}, brush_.Get());
}

void Ui::GlowLine(float x0, float x1, float y, float opacity)
{
    lineBrush_->SetStartPoint({x0, y});
    lineBrush_->SetEndPoint({x1, y});
    lineBrush_->SetOpacity(opacity * alpha_);
    renderer_.D2D()->FillRectangle({x0, y - 1.0f, x1, y + 1.0f}, lineBrush_.Get());
    lineBrush_->SetOpacity(opacity * alpha_ * 0.25f);
    renderer_.D2D()->FillRectangle({x0, y - 4.0f, x1, y + 4.0f}, lineBrush_.Get());
}

// ---- Screens -----------------------------------------------------------------

void Ui::Draw(double now)
{
    ApplyPalette();
    EnsureResources();
    UpdateTransform();
    renderer_.D2D()->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                                  D2D1::Matrix3x2F::Translation(offsetX_, offsetY_));

    alpha_ = 1.0f;
    if (splash_)
        DrawSplash(now - splashStart_);
    else if (stack_.empty())
        DrawViewing(now);
    else
        DrawMenu(now);

    alpha_ = 1.0f;
    renderer_.D2D()->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Ui::DrawSplash(double t)
{
    const float out = 1.0f - Smooth(kSplashLength - 0.6, kSplashLength, t);
    const wchar_t letters[] = L"CAPTURA";
    constexpr int count = 7;
    constexpr float cell = 96.0f;
    const float startX = kVirtualW * 0.5f - count * cell * 0.5f;
    for (int i = 0; i < count; ++i)
    {
        const float a = Smooth(1.2 + i * 0.09, 1.7 + i * 0.09, t);
        alpha_ = a * out;
        const float dy = (1.0f - a) * 14.0f;
        Text(std::wstring(1, letters[i]), fLogo_.Get(), {startX + i * cell, 240 + dy, startX + (i + 1) * cell, 390 + dy},
             cText_, DWRITE_TEXT_ALIGNMENT_CENTER, 1.6f);
    }

    alpha_ = out;
    GlowLine(300, 980, 396, Smooth(2.0, 2.6, t));
    const float sweep = Smooth(2.1, 3.1, t);
    if (sweep > 0.0f && sweep < 1.0f)
        Orb({300 + 680 * sweep, 396}, 4.0f, WithAlpha(cText_, 1.0f - sweep * 0.5f));

    alpha_ = Smooth(2.3, 3.0, t) * out;
    Text(L"U S B   C A P T U R E   V I E W E R", fSubtitle_.Get(), {0, 410, kVirtualW, 440}, cDim_,
         DWRITE_TEXT_ALIGNMENT_CENTER, 0.6f);

    alpha_ = out * Smooth(2.8, 3.2, t) * (0.35f + 0.65f * Pulse(t, 4.0));
    Text(L"Press any button", fHint_.Get(), {0, 600, kVirtualW, 630}, cDim_, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Ui::DrawRow(const Item& item, D2D1_RECT_F row, bool selected, double now, RowHit& hit)
{
    const float cy = (row.top + row.bottom) * 0.5f;
    const float valueX = kValueX + (row.left - kListX);
    hit.valueX = valueX;

    if (selected)
    {
        const D2D1_RECT_F bar{row.left - 26, row.top + 3, row.right, row.bottom - 3};
        const float p = Pulse(now, 3.0);
        barBrush_->SetStartPoint({bar.left, 0});
        barBrush_->SetEndPoint({bar.right, 0});
        barBrush_->SetOpacity(alpha_ * (0.75f + 0.25f * p));
        renderer_.D2D()->FillRoundedRectangle({bar, 14, 14}, barBrush_.Get());
        for (int k = 1; k <= 3; ++k)
        {
            const float g = k * 2.5f;
            StrokeRound({bar.left - g, bar.top - g, bar.right + g, bar.bottom + g}, 14 + g,
                        WithAlpha(cAccent_, (0.28f - k * 0.07f) * (0.6f + 0.4f * p)), 1.5f);
        }
    }

    Orb({row.left, cy}, selected ? 6.0f : 3.5f, selected ? cAccent_ : WithAlpha(cDim_, 0.7f));

    const bool hasValue = item.kind == Kind::Slider || item.kind == Kind::Choice || item.meter || !item.value.empty();
    const float textX = row.left + 24;
    const float labelRight = hasValue ? valueX - 10 : row.right - 10;
    const D2D1_COLOR_F labelColor = item.kind == Kind::Info ? cDim_ : cText_;
    const float glow = selected ? 1.0f : 0.0f;
    if (item.detail.empty())
    {
        Text(item.label, fItem_.Get(), {textX, row.top, labelRight, row.bottom}, labelColor,
             DWRITE_TEXT_ALIGNMENT_LEADING, glow, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    else
    {
        Text(item.label, fItem_.Get(), {textX, row.top + 3, labelRight, row.top + 35}, labelColor,
             DWRITE_TEXT_ALIGNMENT_LEADING, glow);
        Text(item.detail, fDetail_.Get(), {textX, row.top + 34, labelRight, row.bottom}, cDim_);
    }

    hit.hasBar = false;
    if (item.kind == Kind::Slider)
    {
        const D2D1_RECT_F track{valueX + 40, cy - 3, valueX + 330, cy + 3};
        FillRound(track, 3, WithAlpha(cDim_, 0.3f));
        D2D1_RECT_F fill = track;
        fill.right = track.left + (track.right - track.left) * std::clamp(item.fraction, 0.0f, 1.0f);
        FillRound(fill, 3, cAccent_);
        Orb({fill.right, cy}, selected ? 7.0f : 5.0f, cText_);
        Text(item.value, fItem_.Get(), {track.right + 10, row.top, row.right - 12, row.bottom}, cText_,
             DWRITE_TEXT_ALIGNMENT_TRAILING, glow, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hit.bar = {track.left - 10, row.top, track.right + 10, row.bottom};
        hit.hasBar = true;
    }
    else if (item.kind == Kind::Choice)
    {
        const D2D1_COLOR_F arrow = selected ? cAccent_ : WithAlpha(cDim_, 0.6f);
        Text(L"◀", fDetail_.Get(), {valueX + 40, row.top, valueX + 70, row.bottom}, arrow,
             DWRITE_TEXT_ALIGNMENT_LEADING, 0, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Text(item.value, fItem_.Get(), {valueX + 70, row.top, row.right - 50, row.bottom}, cText_,
             DWRITE_TEXT_ALIGNMENT_CENTER, glow, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Text(L"▶", fDetail_.Get(), {row.right - 50, row.top, row.right - 20, row.bottom}, arrow,
             DWRITE_TEXT_ALIGNMENT_TRAILING, 0, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    else if (item.meter)
    {
        const D2D1_RECT_F track{valueX + 40, cy - 5, row.right - 16, cy + 5};
        FillRound(track, 5, WithAlpha(cDim_, 0.3f));
        D2D1_RECT_F fill = track;
        fill.right = track.left + (track.right - track.left) * std::clamp(item.fraction, 0.0f, 1.0f);
        if (fill.right - fill.left > 1.0f)
            FillRound(fill, 5, item.fraction > 0.92f ? WithAlpha(cAccent_, 1.0f) : cLive_);
    }
    else if (!item.value.empty())
    {
        Text(item.value, fHint_.Get(), {valueX, row.top, row.right - 16, row.bottom}, item.good ? cLive_ : cAccent_,
             DWRITE_TEXT_ALIGNMENT_TRAILING, 0.8f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void Ui::DrawHints(float y, bool back, bool adjust)
{
    float x = kListX;
    auto hint = [&](const wchar_t* key, const wchar_t* label) {
        Text(key, fHint_.Get(), {x, y - 14, x + 200, y + 14}, cDim_, DWRITE_TEXT_ALIGNMENT_LEADING, 0,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        x += Measure(key, fHint_.Get()) + 8;
        Text(label, fHint_.Get(), {x, y - 14, x + 200, y + 14}, cText_, DWRITE_TEXT_ALIGNMENT_LEADING, 0,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        x += Measure(label, fHint_.Get()) + 34;
    };
    hint(L"Enter", L"Select");
    if (back)
        hint(L"Esc", L"Back");
    if (adjust)
        hint(L"\u2190\u2192", L"Adjust");
    hint(L"F11", L"Fullscreen");
}

void Ui::DrawMenu(double now)
{
    const Page page = stack_.back();
    auto items = BuildItems(page);
    ClampSelection(items);
    const int sel = Selected();
    const int n = static_cast<int>(items.size());

    alpha_ = Smooth(fadeStart_, fadeStart_ + 0.3, now);
    const float slide = (1.0f - alpha_) * 30.0f;

    // Header with page title and clock.
    Orb({kListX - 4 + slide, 93}, 9.0f, cAccent_);
    Text(PageTitle(static_cast<int>(page)), fTitle_.Get(), {kListX + 22 + slide, 62, 900, 124}, cText_,
         DWRITE_TEXT_ALIGNMENT_LEADING, 1.0f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    GlowLine(kListX - 30, 900, 130, 0.9f);
    SYSTEMTIME st;
    GetLocalTime(&st);
    Text(std::format(L"{:02}:{:02}", st.wHour, st.wMinute), fClock_.Get(), {1000, 66, kListRight, 118}, cDim_,
         DWRITE_TEXT_ALIGNMENT_TRAILING, 0.6f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Rows.
    int& scroll = scroll_[static_cast<int>(page)];
    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + kVisibleRows)
        scroll = sel - kVisibleRows + 1;
    scroll = std::clamp(scroll, 0, std::max(0, n - kVisibleRows));

    hits_.clear();
    for (int i = scroll; i < std::min(n, scroll + kVisibleRows); ++i)
    {
        const float top = kListTop + (i - scroll) * kRowH;
        RowHit hit{{kListX + slide - 30, top, kListRight + slide, top + kRowH}, {}, 0, i, false};
        DrawRow(items[i], {kListX + slide, top, kListRight + slide, top + kRowH}, i == sel, now, hit);
        hits_.push_back(hit);
    }
    if (scroll > 0)
        Text(L"▲", fDetail_.Get(), {kListX, kListTop - 26, kListRight, kListTop - 4}, cDim_,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    if (scroll + kVisibleRows < n)
        Text(L"▼", fDetail_.Get(), {kListX, kListTop + kVisibleRows * kRowH + 2, kListRight,
                                         kListTop + kVisibleRows * kRowH + 24},
             cDim_, DWRITE_TEXT_ALIGNMENT_CENTER);

    alpha_ = 1.0f;

    // Auto-start countdown.
    if (countdownEnd_ > 0 && page == Page::Sources && sel < n)
    {
        const double left = countdownEnd_ - now;
        const D2D1_RECT_F pill{240, 594, 1040, 632};
        FillRound(pill, 19, WithAlpha(cPanel_, 0.7f));
        StrokeRound(pill, 19, WithAlpha(cAccent_, 0.5f), 1.2f);
        const float progress = static_cast<float>(1.0 - left / kCountdownLength);
        GlowLine(pill.left + 20, pill.left + 20 + (pill.right - pill.left - 40) * progress, pill.bottom - 4, 0.8f);
        Text(std::format(L"Starting {} in {}…   press any button to stay here", items[sel].label,
                         static_cast<int>(std::ceil(std::max(0.0, left)))),
             fHint_.Get(), pill, cText_, DWRITE_TEXT_ALIGNMENT_CENTER, 0.6f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const bool adjustable = page == Page::Quality || page == Page::Picture || page == Page::Audio || page == Page::Display;
    DrawHints(668, !(launching_ && stack_.size() == 1), adjustable);

    const std::wstring live = engine_.ActiveName();
    if (!live.empty())
        Text(L"● " + live, fHint_.Get(), {800, 654, kListRight, 682}, cLive_, DWRITE_TEXT_ALIGNMENT_TRAILING,
             0.4f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void Ui::DrawViewing(double now)
{
    if (!renderer_.HasVideo())
    {
        Text(engine_.Status(), fItem_.Get(), {140, 290, 1140, 380}, cText_, DWRITE_TEXT_ALIGNMENT_CENTER, 1.0f,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        for (int k = 0; k < 3; ++k)
            Orb({610.0f + k * 30.0f, 410.0f}, 3.0f + 3.0f * Pulse(now, 4.0, -k * 0.8), cAccent_);
        Text(L"Esc  Menu", fHint_.Get(), {0, 655, kVirtualW, 685}, cDim_, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    if (now < osdUntil_)
    {
        alpha_ = static_cast<float>(std::min(1.0, (osdUntil_ - now) / 0.6)) *
                 Smooth(osdUntil_ - kOsdLength, osdUntil_ - kOsdLength + 0.3, now);
        const D2D1_RECT_F box{40, 36, 620, 176};
        FillRound(box, 14, WithAlpha(cPanel_, 0.78f));
        StrokeRound(box, 14, WithAlpha(cAccent_, 0.5f), 1.5f);
        Orb({68, 72}, 6.0f, cLive_);
        Text(engine_.ActiveName(), fOsd_.Get(), {86, 52, 610, 92}, cText_, DWRITE_TEXT_ALIGNMENT_LEADING, 1.0f,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        FormatInfo f{};
        if (engine_.CurrentFormat(f))
            Text(std::format(L"{}×{}  ·  {}  ·  {} fps", f.width, f.height, SubtypeName(f.subtype),
                             FpsText(f.fpsX100)),
                 fDetail_.Get(), {86, 94, 610, 116}, cDim_);
        Text(L"Esc / right-click \u2014 open menu", fDetail_.Get(), {86, 118, 610, 140}, cDim_);
        const std::wstring audioName = audio_.ActiveInputName();
        Text(!settings_.audioEnabled ? L"Audio: off"
             : audioName.empty()     ? audio_.Status()
                                     : L"Audio: " + audioName + (settings_.audioMuted ? L" (muted)" : L""),
             fDetail_.Get(), {86, 142, 610, 164}, cDim_);
        alpha_ = 1.0f;
    }

    if (now < volumeUntil_)
    {
        alpha_ = static_cast<float>(std::min(1.0, (volumeUntil_ - now) / 0.4));
        const D2D1_RECT_F pill{430, 598, 850, 652};
        FillRound(pill, 27, WithAlpha(cPanel_, 0.82f));
        StrokeRound(pill, 27, WithAlpha(cAccent_, 0.5f), 1.4f);
        Text(settings_.audioMuted ? L"Muted" : std::format(L"Volume {}%", settings_.audioVolume), fItem_.Get(),
             {460, 598, 640, 652}, cText_, DWRITE_TEXT_ALIGNMENT_LEADING, 0.8f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const D2D1_RECT_F track{650, 622, 820, 628};
        FillRound(track, 3, WithAlpha(cDim_, 0.3f));
        D2D1_RECT_F fill = track;
        fill.right = track.left + (track.right - track.left) * (settings_.audioMuted ? 0.0f : settings_.audioVolume / 200.0f);
        if (fill.right - fill.left > 1.0f)
            FillRound(fill, 3, cAccent_);
        alpha_ = 1.0f;
    }

    if (settings_.showFps)
    {
        const D2D1_RECT_F pill{1100, 30, 1250, 64};
        FillRound(pill, 17, WithAlpha(cPanel_, 0.7f));
        Text(std::format(L"{:.1f} fps", engine_.MeasuredFps()), fHint_.Get(), pill, cText_,
             DWRITE_TEXT_ALIGNMENT_CENTER, 0.5f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}
