#ifndef HMAWL_NATIVE_WINDOWS_VIDEO_H
#define HMAWL_NATIVE_WINDOWS_VIDEO_H

#ifdef _WIN32
#include "native_efb.h"
#include <memory>

class NativeWindowsVideo {
public:
    NativeWindowsVideo();
    ~NativeWindowsVideo();
    NativeWindowsVideo(const NativeWindowsVideo&) = delete;
    NativeWindowsVideo& operator=(const NativeWindowsVideo&) = delete;
    bool Pump();
    void Present(const NativeEfb& efb);
    void Wait();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif

#endif
