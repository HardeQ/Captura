#pragma once

#include <windows.h>
#include <cguid.h>

#include <string>

// Requested capture mode. Zero / GUID_NULL fields mean "pick automatically".
struct FormatRequest
{
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsX100 = 0; // frame rate * 100
    GUID subtype = GUID_NULL;

    bool operator==(const FormatRequest& o) const
    {
        return width == o.width && height == o.height && fpsX100 == o.fpsX100 && IsEqualGUID(subtype, o.subtype);
    }
};

struct PictureSettings
{
    int brightness = 0;   // -100..100
    int contrast = 100;   // 0..200 %
    int saturation = 100; // 0..200 %
    int hue = 0;          // -180..180 degrees
    int gamma = 100;      // 50..250 (gamma * 100)
    int sharpness = 0;    // 0..100
};

// Persisted in %APPDATA%\Captura\settings.ini.
struct Settings
{
    PictureSettings picture;
    int palette = 0; // index into kPalettes
    bool crtOnVideo = false;
    bool crtInMenus = true;
    int scanlines = 60; // 0..100
    int curvature = 50; // 0..100
    bool fullscreen = false;
    bool showFps = false;
    // Audio
    bool audioEnabled = true;
    int audioVolume = 100;             // 0..200 %
    bool audioMuted = false;
    std::wstring audioInput = L"auto"; // "auto" or an endpoint id
    std::wstring audioOutput;          // empty = system default
    int audioLatency = 1;              // 0 low, 1 normal, 2 high

    std::wstring lastDevice; // symbolic link, "auto", or empty if never chosen
    FormatRequest format;

    void Load();
    void Save() const;
};
