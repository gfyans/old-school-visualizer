#include "platform/Window.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>

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
        const RECT m = m_hasArea ? m_area : mi.rcMonitor;

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
    if (onFullscreenChanged) onFullscreenChanged(m_fullscreen);
}

void Window::SetClientSize(int w, int h) {
    if (m_fullscreen) return;
    if (!m_hasSavedSize) { GetWindowPlacement(m_hwnd, &m_sizeBefore); m_hasSavedSize = true; }
    if (IsZoomed(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
    RECT r{ 0, 0, w, h };
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE)), FALSE, 0, GetDpiForWindow(m_hwnd));
    SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::RestoreSize() {
    if (!m_hasSavedSize) return;
    m_hasSavedSize = false;
    if (!m_fullscreen) SetWindowPlacement(m_hwnd, &m_sizeBefore);
}

void Window::SetFullscreenArea(const RECT* area) {
    m_hasArea = area != nullptr;
    if (area) m_area = *area;
    if (!m_fullscreen) return;
    RECT m = m_area;
    if (!m_hasArea) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        m = mi.rcMonitor;
    }
    SetWindowPos(m_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (onFullscreenChanged) onFullscreenChanged(true);
}

std::wstring Window::PickFolder(HWND owner, const wchar_t* title, const std::wstring& start) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dialog->SetFolder(folder.Get());
    if (FAILED(dialog->Show(owner))) return {};   // cancelled
    Microsoft::WRL::ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    std::wstring chosen;
    if (SUCCEEDED(dialog->GetResult(&result)) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) chosen = path;
    CoTaskMemFree(path);
    return chosen;
}

std::vector<MonitorDesc> Window::Monitors() {
    std::vector<MonitorDesc> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM user) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi))
            reinterpret_cast<std::vector<MonitorDesc>*>(user)->push_back({ mi.rcMonitor, mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left < b.rect.left;
    });
    // Developer switch for testing multi-monitor modes on one screen: VISUALIZER_FAKE_MONITORS=<n>
    // splits the primary monitor into n side-by-side "monitors".
    wchar_t env[8];
    if (!list.empty() && GetEnvironmentVariableW(L"VISUALIZER_FAKE_MONITORS", env, ARRAYSIZE(env)) > 0) {
        const int n = std::clamp(_wtoi(env), 1, 4);
        const RECT whole = list[0].rect;
        list.clear();
        for (int i = 0; i < n; ++i) {
            const LONG w = whole.right - whole.left;
            RECT r{ whole.left + w * i / n, whole.top, whole.left + w * (i + 1) / n, whole.bottom };
            list.push_back({ r, L"\\\\.\\FAKE" + std::to_wstring(i + 1), i == 0 });
        }
    }
    return list;
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
        // Fullscreen hides the pointer, until the mouse moves (so the right-click menu can be found).
        if (m_fullscreen && LOWORD(lp) == HTCLIENT && GetTickCount64() - m_lastMouseMove > kCursorHideMs) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        const POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        if (pt.x != m_lastMousePos.x || pt.y != m_lastMousePos.y) {   // ignore the synthetic moves Windows sends
            m_lastMousePos = pt;
            m_lastMouseMove = GetTickCount64();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            SetTimer(m_hwnd, kCursorTimer, kCursorHideMs + 100, nullptr);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kCursorTimer) {
            KillTimer(m_hwnd, kCursorTimer);
            POINT pt;
            GetCursorPos(&pt);
            if (m_fullscreen && WindowFromPoint(pt) == m_hwnd) SetCursor(nullptr);
            return 0;
        }
        break;

    case WM_CONTEXTMENU:
        if (onContextMenu) {
            POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
            if (pt.x == -1 && pt.y == -1) {   // from the keyboard (menu key, Shift+F10): the middle of the window
                RECT rc;
                GetClientRect(m_hwnd, &rc);
                pt = { rc.right / 2, rc.bottom / 3 };
                ClientToScreen(m_hwnd, &pt);
            }
            onContextMenu(pt);
            return 0;
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
