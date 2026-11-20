#pragma once
#include <windows.h>

// Sleeps (not spins) until the next frame is due, using a high-resolution
// waitable timer. Needed because drivers can force vsync off, in which case
// Present() returns immediately and we'd render thousands of frames a second.
class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    void SetTargetFps(double fps);
    double TargetFps() const { return m_fps; }
    void Wait();

    // Even divisor of the display refresh rate, kept at or above ~60 fps:
    // 60 Hz -> 60, 143 Hz -> 71.5, 144 -> 72, 165 -> 82.5, 240 -> 60.
    static double PickTargetFps(double refreshHz);
    static double DisplayRefreshHz(HWND hwnd);

private:
    HANDLE m_timer = nullptr;
    LARGE_INTEGER m_freq{};
    long long m_periodTicks = 0;
    long long m_nextDeadline = 0;
    double m_fps = 0;
};
