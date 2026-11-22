#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

struct MonitorDesc {
    RECT rect;            // physical pixels, virtual-screen coordinates
    std::wstring name;    // e.g. \.\DISPLAY2 (stable while plugged in)
    bool primary;
};

// Top-level app window. Fullscreen is borderless (covers the monitor, no
// display mode change), so toggling is instant and never flickers.
class Window {
public:
    // Created hidden; call Show() after restoring placement so it doesn't visibly jump.
    bool Create(HINSTANCE instance, const wchar_t* title, int clientWidth, int clientHeight);
    void Show(bool maximized);

    // Normal (restored) rectangle and maximised state, valid even while fullscreen.
    void GetPlacement(RECT& normalRect, bool& maximized) const;
    // Applies a saved rectangle if it is still on a connected monitor; returns false otherwise.
    bool RestorePlacement(const RECT& normalRect);

    void SetFullscreen(bool on);
    // What fullscreen covers: a monitor's rectangle, or several monitors' union; nullptr = the
    // monitor the window is on. Applied immediately if already fullscreen.
    void SetFullscreenArea(const RECT* area);
    // The standard Windows folder picker; returns the chosen folder, or empty if cancelled.
    static std::wstring PickFolder(HWND owner, const wchar_t* title, const std::wstring& start);
    // Connected monitors, primary first, then left to right.
    static std::vector<MonitorDesc> Monitors();
    void ToggleFullscreen() { SetFullscreen(!m_fullscreen); }
    bool IsFullscreen() const { return m_fullscreen; }
    bool IsMinimized() const { return m_minimized; }

    // Swaps in a new window at the same place, size and state, with (or without) a see-through
    // client area: transparent windows have no redirection surface, so a DirectComposition swap
    // chain's alpha shows the desktop behind. Only possible at creation, hence a new window. The
    // caller must point its swap chain at the new Handle(). Returns false (old window kept) on failure.
    bool Recreate(bool transparent);
    bool IsTransparent() const { return m_transparent; }

    HWND Handle() const { return m_hwnd; }
    void ClientSize(int& w, int& h) const;
    void SetTitle(const wchar_t* title) { SetWindowTextW(m_hwnd, title); }
    // Windowed only: resize so the client area is exactly w x h (restore = put the old size back).
    void SetClientSize(int w, int h);
    void RestoreSize();
    void Close() { SendMessageW(m_hwnd, WM_CLOSE, 0, 0); }

    std::function<void(int w, int h)> onResize;
    std::function<void(UINT vk)> onKeyDown;   // non-repeating key presses only
    std::function<void()> onClose;            // just before the window is destroyed (also on log-off)
    std::function<void(bool)> onFullscreenChanged;
    std::function<void(POINT screen)> onContextMenu;   // right-click (or the menu key)

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    static constexpr UINT_PTR kCursorTimer = 1;
    static constexpr ULONGLONG kCursorHideMs = 2000;   // fullscreen: hide the pointer after this long still
    ULONGLONG m_lastMouseMove = 0;
    POINT m_lastMousePos{ -100000, -100000 };

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    bool m_transparent = false;
    bool m_fullscreen = false;
    bool m_minimized = false;
    LONG_PTR m_savedStyle = 0;
    bool m_hasArea = false;
    bool m_hasSavedSize = false;
    WINDOWPLACEMENT m_sizeBefore{ sizeof(WINDOWPLACEMENT) };
    RECT m_area{};
    WINDOWPLACEMENT m_savedPlacement{ sizeof(WINDOWPLACEMENT) };
};
