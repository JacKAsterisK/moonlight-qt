#pragma once

#include "SDL_compat.h"
#include <memory>

// Optional per-stream WinTab capture. The installed driver supplies the DLL;
// Moonlight does not install it or change global tablet preferences.
class WinTabTablet
{
public:
    WinTabTablet(SDL_Window* window, int streamWidth, int streamHeight);
    ~WinTabTablet();
    bool available() const;
    bool isCapturing() const;
    void setActive(bool active);
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
