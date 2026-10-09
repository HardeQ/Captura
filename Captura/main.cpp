// Captura — USB capture card viewer.
//
// Boots with a PS2-style splash, then a source menu listing capture cards
// first. Media Foundation captures, Direct3D 11 renders (with color controls
// and a CRT filter) and Direct2D draws the menus.
//
// Keyboard: arrows navigate/adjust, Enter selects, Esc goes back / opens the
// menu, Tab opens the menu, F11 or Alt+Enter toggles fullscreen.
// Gamepad (XInput): D-pad/left stick, A select, B back, Start menu.

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <mfapi.h>
#include <Xinput.h>

#include <memory>
#include <string>

#include "Capture.h"
#include "Renderer.h"
#include "Settings.h"
#include "Ui.h"

namespace
{
constexpr wchar_t kWindowClass[] = L"CapturaWindow";
constexpr wchar_t kAppName[] = L"Captura";
constexpr UINT_PTR kRescanTimer = 1;
constexpr UINT_PTR kSizeMoveTimer = 2;

Settings g_settings;
std::unique_ptr<Renderer> g_renderer;
std::unique_ptr<CaptureEngine> g_engine;
std::unique_ptr<Ui> g_ui;
HDEVNOTIFY g_deviceNotify = nullptr;
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};
bool g_fullscreen = false;
bool g_cursorHidden = false;
double g_lastMouseMove = 0;

double Now()
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency.QuadPart;
}

void SetFullscreen(HWND hwnd, bool fullscreen)
{
    if (fullscreen == g_fullscreen)
        return;
    const DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    if (fullscreen)
    {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &g_windowedPlacement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_windowedPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = fullscreen;
    g_settings.fullscreen = fullscreen;
}

// Polls the first XInput controller and turns it into menu actions, with key
// repeat on the directions.
void PollGamepad(double now)
{
    static WORD previous = 0;
    static double nextRepeat[4] = {};
    static double nextConnectCheck = 0;
    static bool connected = true;

    // XInputGetState is slow for absent controllers, so only retry occasionally.
    if (!connected && now < nextConnectCheck)
        return;
    XINPUT_STATE state{};
    connected = XInputGetState(0, &state) == ERROR_SUCCESS;
    if (!connected)
    {
        previous = 0;
        nextConnectCheck = now + 1.0;
        return;
    }

    WORD buttons = state.Gamepad.wButtons;
    constexpr SHORT kStick = 16000;
    if (state.Gamepad.sThumbLY > kStick) buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (state.Gamepad.sThumbLY < -kStick) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (state.Gamepad.sThumbLX < -kStick) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (state.Gamepad.sThumbLX > kStick) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;

    auto pressed = [&](WORD mask) { return (buttons & mask) && !(previous & mask); };
    if (pressed(XINPUT_GAMEPAD_A)) g_ui->OnAction(Action::Confirm);
    if (pressed(XINPUT_GAMEPAD_B) || pressed(XINPUT_GAMEPAD_BACK)) g_ui->OnAction(Action::Back);
    if (pressed(XINPUT_GAMEPAD_START)) g_ui->OnAction(Action::Menu);

    const WORD directions[4] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
                                XINPUT_GAMEPAD_DPAD_RIGHT};
    const Action actions[4] = {Action::Up, Action::Down, Action::Left, Action::Right};
    for (int i = 0; i < 4; ++i)
    {
        if (pressed(directions[i]))
        {
            g_ui->OnAction(actions[i]);
            nextRepeat[i] = now + 0.4;
        }
        else if ((buttons & directions[i]) && now >= nextRepeat[i])
        {
            g_ui->OnAction(actions[i]);
            nextRepeat[i] = now + 0.08;
        }
    }
    previous = buttons;
}

void RenderFrame(HWND hwnd)
{
    if (!g_renderer || !g_ui || !g_renderer->CanRender())
        return;
    const double now = Now();
    PollGamepad(now);
    g_ui->Update(now);
    g_engine->WithFrame([](const VideoFrame& frame) { g_renderer->UploadFrame(frame); });

    // Hide the cursor while watching fullscreen.
    if (g_fullscreen && g_ui->IsViewing() && now - g_lastMouseMove > 2.5 && !g_cursorHidden)
    {
        POINT pt;
        RECT rc;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        GetClientRect(hwnd, &rc);
        if (PtInRect(&rc, pt))
        {
            g_cursorHidden = true;
            SetCursor(nullptr);
        }
    }

    const RenderParams params = g_ui->Params(now);
    g_renderer->BeginFrame(params);
    g_ui->Draw(now);
    if (g_renderer->EndFrame(params) == DXGI_STATUS_OCCLUDED)
        Sleep(16);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // Get told about any device interface arriving or leaving.
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        g_deviceNotify = RegisterDeviceNotificationW(hwnd, &filter,
                                                     DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
        return 0;
    }
    case WM_APP_STATUS:
        if (g_engine)
            SetWindowTextW(hwnd, (std::wstring(kAppName) + L" — " + g_engine->Status()).c_str());
        return 0;
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED)
            SetTimer(hwnd, kRescanTimer, 750, nullptr); // debounce bursts of notifications
        return TRUE;
    case WM_TIMER:
        if (wParam == kRescanTimer)
        {
            KillTimer(hwnd, kRescanTimer);
            if (g_ui)
                g_ui->OnDevicesChanged();
        }
        else if (wParam == kSizeMoveTimer)
        {
            RenderFrame(hwnd); // keep animating inside the modal size/move loop
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        SetTimer(hwnd, kSizeMoveTimer, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, kSizeMoveTimer);
        return 0;
    case WM_KEYDOWN:
        if (!g_ui)
            return 0;
        switch (wParam)
        {
        case VK_UP: g_ui->OnAction(Action::Up); break;
        case VK_DOWN: g_ui->OnAction(Action::Down); break;
        case VK_LEFT: g_ui->OnAction(Action::Left); break;
        case VK_RIGHT: g_ui->OnAction(Action::Right); break;
        case VK_RETURN:
        case VK_SPACE: g_ui->OnAction(Action::Confirm); break;
        case VK_ESCAPE:
        case VK_BACK: g_ui->OnAction(Action::Back); break;
        case VK_TAB:
        case 'M': g_ui->OnAction(Action::Menu); break;
        case VK_F11: SetFullscreen(hwnd, !g_fullscreen); break;
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN))
        {
            SetFullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
    {
        // Windows also sends this when the window appears under a resting
        // cursor; only real movement should hover-select menu items.
        static LPARAM lastPosition = -1;
        if (lParam == lastPosition)
            return 0;
        const bool first = lastPosition == -1;
        lastPosition = lParam;
        if (first)
            return 0;
        g_lastMouseMove = Now();
        g_cursorHidden = false;
        if (g_ui)
            g_ui->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(g_cursorHidden ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_ui)
            g_ui->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (g_ui)
            g_ui->OnMouseUp();
        return 0;
    case WM_LBUTTONDBLCLK:
        if (g_ui && g_ui->IsViewing())
            SetFullscreen(hwnd, !g_fullscreen);
        else if (g_ui)
            g_ui->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_RBUTTONDOWN:
        if (g_ui)
            g_ui->OnRightClick();
        return 0;
    case WM_MOUSEWHEEL:
        if (g_ui)
            g_ui->OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_renderer)
            g_renderer->Resize();
        return 0;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        RenderFrame(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_deviceNotify)
            UnregisterDeviceNotification(g_deviceNotify);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;
    if (FAILED(MFStartup(MF_VERSION)))
    {
        MessageBoxW(nullptr, L"Media Foundation is not available on this system.", kAppName, MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    g_settings.Load();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = nullptr; // set in WM_SETCURSOR
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // 1280x720 client area.
    RECT rect{0, 0, 1280, 720};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
    {
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    auto renderer = std::make_unique<Renderer>();
    if (!renderer->Init(hwnd))
    {
        std::wstring message = L"Could not initialize Direct3D 11.";
        if (!renderer->Error().empty())
            message += L"\n\nShader compiler output:\n" + renderer->Error();
        MessageBoxW(hwnd, message.c_str(), kAppName, MB_ICONERROR);
        DestroyWindow(hwnd);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    g_renderer = std::move(renderer);
    g_engine = std::make_unique<CaptureEngine>(hwnd);
    g_ui = std::make_unique<Ui>(hwnd, *g_engine, *g_renderer, g_settings);
    g_ui->onToggleFullscreen = [hwnd] { SetFullscreen(hwnd, !g_fullscreen); };

    ShowWindow(hwnd, showCommand);
    if (g_settings.fullscreen)
    {
        g_settings.fullscreen = false;
        SetFullscreen(hwnd, true);
    }
    g_renderer->Resize();
    g_engine->Start();
    g_ui->Start(Now());

    MSG msg{};
    for (;;)
    {
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit)
            break;
        if (IsIconic(hwnd))
        {
            WaitMessage();
            continue;
        }
        RenderFrame(hwnd);
    }

    g_settings.Save();
    g_engine->Stop();
    g_ui.reset();
    g_engine.reset();
    g_renderer.reset();
    MFShutdown();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
