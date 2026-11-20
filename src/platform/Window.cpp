#include "platform/Window.h"

namespace {
constexpr wchar_t kClassName[] = L"VisualizerWindow";
constexpr bool IsRepeat(LPARAM lp) { return (lp & (1 << 30)) != 0; }
constexpr bool AltDown(LPARAM lp) { return (lp & (1 << 29)) != 0; }
}

bool Window::Create(HINSTANCE instance, const wchar_t* title, int clientWidth, int clientHeight) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));   // res/app.rc
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{ 0, 0, clientWidth, clientHeight };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());

    m_hwnd = CreateWindowExW(0, kClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, instance, this);
    return m_hwnd != nullptr;
}

void Window::Show(bool maximized) {
    ShowWindow(m_hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
}

void Window::GetPlacement(RECT& normalRect, bool& maximized) const {
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (m_fullscreen) wp = m_savedPlacement;
    else GetWindowPlacement(m_hwnd, &wp);
    normalRect = wp.rcNormalPosition;   // workspace coordinates == screen coords on the primary taskbar layout
    maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);
}

bool Window::RestorePlacement(const RECT& normalRect) {
    if (!MonitorFromRect(&normalRect, MONITOR_DEFAULTTONULL)) return false;   // monitor gone
    WINDOWPLACEMENT wp{ sizeof(wp) };
    GetWindowPlacement(m_hwnd, &wp);
    wp.rcNormalPosition = normalRect;
    wp.showCmd = SW_HIDE;   // position only; Show() decides normal vs maximised
    return SetWindowPlacement(m_hwnd, &wp) != FALSE;
}

void Window::ClientSize(int& w, int& h) const {
    RECT r{};
    GetClientRect(m_hwnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
}

void Window::SetFullscreen(bool on) {
    if (on == m_fullscreen) return;

    if (on) {
        m_savedStyle = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        GetWindowPlacement(m_hwnd, &m_savedPlacement);

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& m = mi.rcMonitor;

        m_fullscreen = true;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, (m_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(m_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        m_fullscreen = false;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_savedStyle);
        SetWindowPlacement(m_hwnd, &m_savedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        m_minimized = (wp == SIZE_MINIMIZED);
        if (!m_minimized && onResize) onResize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_KEYDOWN:
        if (!IsRepeat(lp) && onKeyDown) onKeyDown(static_cast<UINT>(wp));
        return 0;

    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && AltDown(lp)) {
            if (!IsRepeat(lp)) ToggleFullscreen();
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;   // suppress the Alt+Enter "ding"
        break;

    case WM_LBUTTONDBLCLK:
        ToggleFullscreen();
        return 0;

    case WM_SETCURSOR:
        if (m_fullscreen && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = { 320, 200 };
        return 0;

    case WM_DPICHANGED:
        if (!m_fullscreen) {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_CLOSE:
        if (onClose) onClose();
        DestroyWindow(m_hwnd);
        return 0;

    case WM_ENDSESSION:   // shutting down / logging off: WM_CLOSE never arrives
        if (wp && onClose) onClose();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
