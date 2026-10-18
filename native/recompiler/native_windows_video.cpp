#define NOMINMAX
#include <windows.h>
#include "native_windows_video.h"

#include <stdexcept>
#include <vector>

struct NativeWindowsVideo::Impl {
    HWND window = nullptr;
    BITMAPINFO info{};
    std::vector<uint32_t> pixels;
    bool closed = false;
    std::string error;

    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (!self->pixels.empty()) {
                const int width = self->info.bmiHeader.biWidth, height = -self->info.bmiHeader.biHeight;
                int w = rect.right, h = w * height / width;
                if (h > rect.bottom) { h = rect.bottom; w = h * width / height; }
                if (StretchDIBits(dc, (rect.right - w) / 2, (rect.bottom - h) / 2, w, h,
                                 0, 0, width, height, self->pixels.data(), &self->info,
                                 DIB_RGB_COLORS, SRCCOPY) == GDI_ERROR)
                    self->error = "Native Windows EFB presentation failed.";
            }
            EndPaint(window, &paint);
            return 0;
        }
        if (self && message == WM_DESTROY) { self->closed = true; self->window = nullptr; return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

NativeWindowsVideo::NativeWindowsVideo() : impl(std::make_unique<Impl>()) {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Impl::Procedure;
    wc.hInstance = instance;
    wc.lpszClassName = L"HmawlNativeEfb";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register native Windows EFB window.");
    impl->info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    impl->info.bmiHeader.biWidth = NativeEfb::width;
    impl->info.bmiHeader.biHeight = -int(NativeEfb::height);
    impl->info.bmiHeader.biPlanes = 1;
    impl->info.bmiHeader.biBitCount = 32;
    impl->info.bmiHeader.biCompression = BI_RGB;
    RECT rect{0, 0, int(NativeEfb::width), int(NativeEfb::height)};
    if (!AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE))
        throw std::runtime_error("Cannot size native Windows EFB window.");
    impl->window = CreateWindowExW(0, wc.lpszClassName,
        L"Harvest Moon AWL - native EFB preview (not VI scanout)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, impl.get());
    if (!impl->window) throw std::runtime_error("Cannot create native Windows EFB window.");
    ShowWindow(impl->window, SW_SHOW);
}

NativeWindowsVideo::~NativeWindowsVideo() {
    if (impl->window) DestroyWindow(impl->window);
}

bool NativeWindowsVideo::Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!impl->error.empty()) throw std::runtime_error(impl->error);
    return !impl->closed;
}

void NativeWindowsVideo::Present(const NativeEfb& efb) {
    if (impl->closed) return;
    const auto count = size_t(NativeEfb::width) * NativeEfb::height;
    if (efb.color.size() != count || efb.defined.size() != count)
        throw std::runtime_error("Invalid native EFB presentation storage.");
    const auto activeHeight = efb.lastCopy ? efb.lastCopy->height : NativeEfb::height;
    if (activeHeight == 0 || activeHeight > NativeEfb::height)
        throw std::runtime_error("Invalid native EFB preview height.");
    impl->info.bmiHeader.biHeight = -int(activeHeight);
    impl->pixels.resize(size_t(NativeEfb::width) * activeHeight);
    for (size_t i = 0; i < impl->pixels.size(); ++i) {
        // Undefined pixels remain visibly diagnostic, never passed off as rendered content.
        impl->pixels[i] = (efb.defined[i] & 1) ? efb.color[i] >> 8 : 0xff00ffu;
    }
    if (!InvalidateRect(impl->window, nullptr, FALSE) || !UpdateWindow(impl->window))
        throw std::runtime_error("Cannot update native Windows EFB window.");
    Pump();
}

void NativeWindowsVideo::Wait() {
    while (Pump()) {
        if (!WaitMessage()) throw std::runtime_error("Native Windows video message wait failed.");
    }
}
