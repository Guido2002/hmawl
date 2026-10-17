#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "renderer_d3d11.h"

#include <algorithm>
#include <chrono>
#include <cwchar>

namespace {

constexpr wchar_t kWindowClass[] = L"HmawlWindowsHost";
constexpr wchar_t kWindowTitle[] =
    L"Harvest Moon: A Wonderful Life - Direct3D Port Prototype";
constexpr wchar_t kWindowDescription[] =
    L"Direct3D 11 renderer prototype | Move: arrows or WASD | Quit: Esc";
constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMs = 16;

using Clock = std::chrono::steady_clock;

Renderer* g_renderer = nullptr;
Clock::time_point g_lastFrameTime;
bool g_keys[256] = {};
float g_playerX = 0.50f;
float g_playerY = 0.72f;
unsigned long long g_frameCount = 0;

void ShowError(HWND owner, const wchar_t* title, const std::wstring& message) {
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");
    MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

void LogKeyEvent(const wchar_t* action, WPARAM key) {
    wchar_t message[96];
    std::swprintf(message, sizeof(message) / sizeof(message[0]),
                  L"[hmawl] Key %ls: 0x%02llX\n", action,
                  static_cast<unsigned long long>(key));
    OutputDebugStringW(message);
}

void UpdateAndRender(HWND window) {
    const Clock::time_point now = Clock::now();
    const float elapsed =
        std::chrono::duration<float>(now - g_lastFrameTime).count();
    g_lastFrameTime = now;

    float horizontal = 0.0f;
    float vertical = 0.0f;
    if (g_keys[VK_LEFT] || g_keys['A']) {
        horizontal -= 1.0f;
    }
    if (g_keys[VK_RIGHT] || g_keys['D']) {
        horizontal += 1.0f;
    }
    if (g_keys[VK_UP] || g_keys['W']) {
        vertical -= 1.0f;
    }
    if (g_keys[VK_DOWN] || g_keys['S']) {
        vertical += 1.0f;
    }

    constexpr float kWalkSpeed = 0.38f;
    g_playerX = std::clamp(g_playerX + horizontal * kWalkSpeed * elapsed,
                           0.025f, 0.975f);
    g_playerY = std::clamp(g_playerY + vertical * kWalkSpeed * elapsed,
                           0.36f, 0.92f);

    std::wstring error;
    if (!g_renderer->Render(g_playerX, g_playerY, error)) {
        ShowError(window, kWindowTitle, error);
        PostQuitMessage(1);
        return;
    }

    ++g_frameCount;
    if (g_frameCount % 600 == 0) {
        wchar_t message[80];
        std::swprintf(message, sizeof(message) / sizeof(message[0]),
                      L"[hmawl] Rendered %llu frames.\n", g_frameCount);
        OutputDebugStringW(message);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (SetTimer(window, kFrameTimerId, kFrameIntervalMs, nullptr) == 0) {
            ShowError(window, kWindowTitle,
                      L"Could not start the frame timer.");
            return -1;
        }
        g_lastFrameTime = Clock::now();
        OutputDebugStringW(L"[hmawl] Native Direct3D prototype started.\n");
        return 0;

    case WM_TIMER:
        if (wParam == kFrameTimerId && g_renderer != nullptr) {
            UpdateAndRender(window);
        }
        return 0;

    case WM_SIZE:
        if (g_renderer != nullptr) {
            std::wstring error;
            if (!g_renderer->Resize(LOWORD(lParam), HIWORD(lParam), error)) {
                ShowError(window, kWindowTitle, error);
                PostQuitMessage(1);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam < 256) {
            g_keys[wParam] = true;
        }
        if ((lParam & (1LL << 30)) == 0) {
            LogKeyEvent(L"down", wParam);
        }
        return 0;

    case WM_KEYUP:
        if (wParam < 256) {
            g_keys[wParam] = false;
        }
        LogKeyEvent(L"up", wParam);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        KillTimer(window, kFrameTimerId);
        OutputDebugStringW(L"[hmawl] Native Direct3D prototype stopped.\n");
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (RegisterClassExW(&windowClass) == 0) {
        ShowError(nullptr, kWindowTitle,
                  L"Could not register the application window.");
        return 1;
    }

    RECT windowRect = {0, 0, 960, 540};
    constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    if (!AdjustWindowRect(&windowRect, windowStyle, FALSE)) {
        ShowError(nullptr, kWindowTitle,
                  L"Could not calculate the application window size.");
        return 1;
    }

    Renderer renderer;
    g_renderer = &renderer;

    HWND window = CreateWindowExW(
        0, kWindowClass, kWindowTitle, windowStyle, CW_USEDEFAULT,
        CW_USEDEFAULT, windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        ShowError(nullptr, kWindowTitle,
                  L"Could not create the application window.");
        return 1;
    }

    std::wstring error;
    if (!renderer.Initialize(window, error)) {
        ShowError(window, kWindowTitle, error);
        DestroyWindow(window);
        return 1;
    }

    SetWindowTextW(window, kWindowDescription);
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    UpdateAndRender(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
