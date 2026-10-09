#include "Settings.h"

#include <objbase.h>

#include <algorithm>
#include <cwchar>

#include "Palette.h"

namespace
{
constexpr wchar_t kPicture[] = L"Picture";
constexpr wchar_t kDisplay[] = L"Display";
constexpr wchar_t kSource[] = L"Source";
constexpr wchar_t kAudio[] = L"Audio";

std::wstring IniPath()
{
    wchar_t appData[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    const std::wstring dir = (n && n < MAX_PATH) ? std::wstring(appData, n) + L"\\Captura" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key)
{
    wchar_t buffer[1024] = {};
    GetPrivateProfileStringW(section, key, L"", buffer, 1024, path.c_str());
    return buffer;
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback, int lo, int hi)
{
    const std::wstring text = ReadString(path, section, key);
    if (text.empty())
        return fallback;
    return std::clamp(static_cast<int>(std::wcstol(text.c_str(), nullptr, 10)), lo, hi);
}

void Write(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

void WriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int value)
{
    Write(path, section, key, std::to_wstring(value));
}
} // namespace

void Settings::Load()
{
    const std::wstring path = IniPath();
    picture.brightness = ReadInt(path, kPicture, L"Brightness", 0, -100, 100);
    picture.contrast = ReadInt(path, kPicture, L"Contrast", 100, 0, 200);
    picture.saturation = ReadInt(path, kPicture, L"Saturation", 100, 0, 200);
    picture.hue = ReadInt(path, kPicture, L"Hue", 0, -180, 180);
    picture.gamma = ReadInt(path, kPicture, L"Gamma", 100, 50, 250);
    picture.sharpness = ReadInt(path, kPicture, L"Sharpness", 0, 0, 100);

    palette = ReadInt(path, kDisplay, L"Palette", 0, 0, kPaletteCount - 1);
    crtOnVideo = ReadInt(path, kDisplay, L"CrtOnVideo", 0, 0, 1) != 0;
    crtInMenus = ReadInt(path, kDisplay, L"CrtInMenus", 1, 0, 1) != 0;
    scanlines = ReadInt(path, kDisplay, L"Scanlines", 60, 0, 100);
    curvature = ReadInt(path, kDisplay, L"Curvature", 50, 0, 100);
    fullscreen = ReadInt(path, kDisplay, L"Fullscreen", 0, 0, 1) != 0;
    showFps = ReadInt(path, kDisplay, L"ShowFps", 0, 0, 1) != 0;

    audioEnabled = ReadInt(path, kAudio, L"Enabled", 1, 0, 1) != 0;
    audioVolume = ReadInt(path, kAudio, L"Volume", 100, 0, 200);
    audioMuted = ReadInt(path, kAudio, L"Muted", 0, 0, 1) != 0;
    audioInput = ReadString(path, kAudio, L"Input");
    if (audioInput.empty())
        audioInput = L"auto";
    audioOutput = ReadString(path, kAudio, L"Output");
    audioLatency = ReadInt(path, kAudio, L"Latency", 1, 0, 2);

    lastDevice = ReadString(path, kSource, L"Device");
    format.width = static_cast<UINT32>(ReadInt(path, kSource, L"Width", 0, 0, 16384));
    format.height = static_cast<UINT32>(ReadInt(path, kSource, L"Height", 0, 0, 16384));
    format.fpsX100 = static_cast<UINT32>(ReadInt(path, kSource, L"FpsX100", 0, 0, 100000));
    const std::wstring subtype = ReadString(path, kSource, L"Subtype");
    if (subtype.empty() || FAILED(IIDFromString(subtype.c_str(), &format.subtype)))
        format.subtype = GUID_NULL;
}

void Settings::Save() const
{
    const std::wstring path = IniPath();
    WriteInt(path, kPicture, L"Brightness", picture.brightness);
    WriteInt(path, kPicture, L"Contrast", picture.contrast);
    WriteInt(path, kPicture, L"Saturation", picture.saturation);
    WriteInt(path, kPicture, L"Hue", picture.hue);
    WriteInt(path, kPicture, L"Gamma", picture.gamma);
    WriteInt(path, kPicture, L"Sharpness", picture.sharpness);

    WriteInt(path, kDisplay, L"Palette", palette);
    WriteInt(path, kDisplay, L"CrtOnVideo", crtOnVideo);
    WriteInt(path, kDisplay, L"CrtInMenus", crtInMenus);
    WriteInt(path, kDisplay, L"Scanlines", scanlines);
    WriteInt(path, kDisplay, L"Curvature", curvature);
    WriteInt(path, kDisplay, L"Fullscreen", fullscreen);
    WriteInt(path, kDisplay, L"ShowFps", showFps);

    WriteInt(path, kAudio, L"Enabled", audioEnabled);
    WriteInt(path, kAudio, L"Volume", audioVolume);
    WriteInt(path, kAudio, L"Muted", audioMuted);
    Write(path, kAudio, L"Input", audioInput);
    Write(path, kAudio, L"Output", audioOutput);
    WriteInt(path, kAudio, L"Latency", audioLatency);

    Write(path, kSource, L"Device", lastDevice);
    WriteInt(path, kSource, L"Width", static_cast<int>(format.width));
    WriteInt(path, kSource, L"Height", static_cast<int>(format.height));
    WriteInt(path, kSource, L"FpsX100", static_cast<int>(format.fpsX100));
    wchar_t guid[64] = {};
    if (format.subtype != GUID_NULL)
        StringFromGUID2(format.subtype, guid, 64);
    Write(path, kSource, L"Subtype", guid);
}
