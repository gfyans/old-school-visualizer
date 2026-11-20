#include "platform/FrameLimiter.h"
#include <cmath>

FrameLimiter::FrameLimiter() {
    QueryPerformanceFrequency(&m_freq);
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!m_timer)   // pre-1803 Windows: ordinary timer (~1 ms granularity is still fine)
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    SetTargetFps(60.0);
}

FrameLimiter::~FrameLimiter() {
    if (m_timer) CloseHandle(m_timer);
}

void FrameLimiter::SetTargetFps(double fps) {
    if (fps < 1.0) fps = 1.0;
    m_fps = fps;
    m_periodTicks = static_cast<long long>(double(m_freq.QuadPart) / fps);
}

void FrameLimiter::Wait() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // First frame, or we fell more than a frame behind: resync instead of bursting to catch up.
    if (m_nextDeadline == 0 || now.QuadPart - m_nextDeadline > m_periodTicks)
        m_nextDeadline = now.QuadPart;
    m_nextDeadline += m_periodTicks;

    const long long remaining = m_nextDeadline - now.QuadPart;
    if (remaining <= 0 || !m_timer) return;

    LARGE_INTEGER due;   // negative = relative, in 100 ns units
    due.QuadPart = -static_cast<long long>(double(remaining) * 1e7 / double(m_freq.QuadPart));
    if (SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0))
        WaitForSingleObject(m_timer, INFINITE);
}

double FrameLimiter::PickTargetFps(double refreshHz) {
    if (refreshHz < 30.0) refreshHz = 60.0;
    const double divisor = std::floor(refreshHz / 60.0);
    return refreshHz / (divisor < 1.0 ? 1.0 : divisor);
}

double FrameLimiter::DisplayRefreshHz(HWND hwnd) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return 60.0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return static_cast<double>(dm.dmDisplayFrequency);
    return 60.0;
}
