#pragma once
#include <windows.h>
#include <functional>

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
    void ToggleFullscreen() { SetFullscreen(!m_fullscreen); }
    bool IsFullscreen() const { return m_fullscreen; }
    bool IsMinimized() const { return m_minimized; }

    HWND Handle() const { return m_hwnd; }
    void ClientSize(int& w, int& h) const;
    void SetTitle(const wchar_t* title) { SetWindowTextW(m_hwnd, title); }
    void Close() { SendMessageW(m_hwnd, WM_CLOSE, 0, 0); }

    std::function<void(int w, int h)> onResize;
    std::function<void(UINT vk)> onKeyDown;   // non-repeating key presses only
    std::function<void()> onClose;            // just before the window is destroyed (also on log-off)

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    HWND m_hwnd = nullptr;
    bool m_fullscreen = false;
    bool m_minimized = false;
    LONG_PTR m_savedStyle = 0;
    WINDOWPLACEMENT m_savedPlacement{ sizeof(WINDOWPLACEMENT) };
};
