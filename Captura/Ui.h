#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

#include "Capture.h"
#include "Palette.h"
#include "Renderer.h"
#include "Settings.h"

enum class Action { Up, Down, Left, Right, Confirm, Back, Menu };

// PS2-style splash screen, menus and on-screen display. Everything is laid out
// in a virtual 1280x720 space that is scaled to the window.
class Ui
{
public:
    Ui(HWND hwnd, CaptureEngine& engine, Renderer& renderer, Settings& settings);

    void Start(double now);
    void Update(double now);
    RenderParams Params(double now) const;
    void Draw(double now);

    void OnAction(Action action);
    void OnMouseMove(int x, int y);
    void OnMouseDown(int x, int y);
    void OnMouseUp();
    void OnWheel(int delta);
    void OnRightClick();
    void OnDevicesChanged();

    bool IsViewing() const { return !splash_ && stack_.empty(); }

    std::function<void()> onToggleFullscreen;

private:
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    enum class Page { Sources, Main, Quality, Picture, Display, Count };
    enum class Kind { Action, Choice, Slider, Info };

    struct Item
    {
        std::wstring label;
        std::wstring detail;
        std::wstring value;
        bool good = false; // draw the value in the "live" color
        Kind kind = Kind::Action;
        float fraction = 0;
        std::function<void()> activate;
        std::function<void(int)> adjust;
        std::function<void(float)> setFraction;
    };

    struct RowHit
    {
        D2D1_RECT_F row;
        D2D1_RECT_F bar;
        float valueX;
        int index;
        bool hasBar;
    };

    std::vector<Item> BuildItems(Page page);
    void BuildSources(std::vector<Item>& items);
    void BuildMain(std::vector<Item>& items);
    void BuildQuality(std::vector<Item>& items);
    void BuildPicture(std::vector<Item>& items);
    void BuildDisplay(std::vector<Item>& items);
    static Item MakeInfo(std::wstring label, std::wstring detail);
    static Item MakeSlider(std::wstring label, std::wstring detail, int& value, int lo, int hi, int step,
                           const std::function<std::wstring(int)>& format);
    static Item MakeToggle(std::wstring label, std::wstring detail, bool value, std::function<void()> flip);

    void Push(Page page);
    void Back();
    void OpenMenu();
    void CloseMenu();
    void EndSplash();
    void BeginExit();
    void StartDevice(const std::wstring& link, const std::wstring& name);
    void RefreshDevices();
    void ValidatePendingFormat();
    int& Selected();
    void ClampSelection(const std::vector<Item>& items);
    void Move(const std::vector<Item>& items, int direction);

    void ApplyPalette();
    void UpdateTransform();
    void EnsureResources();
    void DrawSplash(double t);
    void DrawMenu(double now);
    void DrawViewing(double now);
    void DrawRow(const Item& item, D2D1_RECT_F row, bool selected, double now, RowHit& hit);
    void DrawHints(float y, bool back, bool adjust);
    static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha);
    void Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect, D2D1_COLOR_F color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float glow = 0.0f,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, bool wrap = false);
    float Measure(const std::wstring& text, IDWriteTextFormat* format);
    void FillRound(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color);
    void StrokeRound(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float width);
    void Orb(D2D1_POINT_2F center, float radius, D2D1_COLOR_F color);
    void GlowLine(float x0, float x1, float y, float opacity);
    D2D1_POINT_2F ToVirtual(int x, int y) const;

    HWND hwnd_;
    CaptureEngine& engine_;
    Renderer& renderer_;
    Settings& settings_;

    ComPtr<IDWriteTextFormat> fTitle_, fItem_, fDetail_, fHint_, fLogo_, fSubtitle_, fClock_, fOsd_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1LinearGradientBrush> lineBrush_, barBrush_;
    int resourcePalette_ = -1;
    D2D1_COLOR_F cText_{}, cDim_{}, cAccent_{}, cLive_{}, cPanel_{};

    float alpha_ = 1.0f; // opacity multiplier for everything being drawn
    float scale_ = 1.0f, offsetX_ = 0.0f, offsetY_ = 0.0f;

    double now_ = 0;
    double splashStart_ = 0;
    bool splash_ = true;
    bool launching_ = false; // first source menu after the splash; no "back"
    std::vector<Page> stack_;
    int selected_[static_cast<int>(Page::Count)] = {};
    int scroll_[static_cast<int>(Page::Count)] = {};
    double fadeStart_ = 0;
    double countdownEnd_ = 0;
    double osdUntil_ = 0;
    double appliedUntil_ = 0;
    double exitStart_ = 0;
    bool exiting_ = false;
    bool closePosted_ = false;

    std::vector<DeviceInfo> devices_;
    FormatRequest pending_;
    std::wstring lastActiveName_;
    std::vector<RowHit> hits_;
    bool dragging_ = false;
    int dragIndex_ = -1;
};
