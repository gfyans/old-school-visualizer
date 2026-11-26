#include "ui/SettingsWindow.h"

#include <algorithm>

// Common Controls 6: the current Windows look for the buttons, check boxes and drop-downs
// (without it they're drawn in the Windows 95 style).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr wchar_t kClassName[] = L"VisualizerSettings";
constexpr wchar_t kTitle[] = L"Visualizer settings";
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;
constexpr int kFirstId = 1000;

// Layout, in 96-dpi pixels.
constexpr int kMargin = 22;
constexpr int kLabelWidth = 150;
constexpr int kControlWidth = 270;
constexpr int kColumnWidth = kLabelWidth + 10 + kControlWidth;
constexpr int kColumnGap = 36;
constexpr int kRowHeight = 32;
constexpr int kToggleHeight = 28;
constexpr int kHeadingHeight = 30;
constexpr int kHeadingGap = 14;   // above every heading but a column's first
constexpr int kButtonWidth = 120;
constexpr int kButtonHeight = 26;
}

SettingsWindow::~SettingsWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_font) DeleteObject(m_font);
    if (m_headingFont) DeleteObject(m_headingFont);
}

bool SettingsWindow::Create(HWND owner) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
    }
    // Start on the owner's monitor so the first layout uses that monitor's DPI.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    m_hwnd = CreateWindowExW(kExStyle, kClassName, kTitle, kStyle, mi.rcWork.left + 100, mi.rcWork.top + 100, 400, 300,
                             owner, nullptr, instance, this);
    if (!m_hwnd) return false;

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, m_hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    m_rows.assign(m_items.size(), Row{});
    for (size_t i = 0; i < m_items.size(); ++i) {
        const SettingItem& item = m_items[i];
        Row& row = m_rows[i];
        const int id = kFirstId + static_cast<int>(i);
        switch (item.kind) {
        case SettingItem::Heading:
            row.label = make(L"STATIC", item.label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, -1);
            break;
        case SettingItem::Choice:
            row.label = make(L"STATIC", item.label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, -1);
            row.control = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
            break;
        case SettingItem::Toggle:
            row.control = make(L"BUTTON", item.label.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, id);
            break;
        case SettingItem::Action:
            row.label = make(L"STATIC", item.label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, -1);
            row.control = make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, id);
            break;
        case SettingItem::Column:
            break;
        }
    }
    m_hint = make(L"STATIC", L"Quick actions are still on the right-click menu, and H lists the keys.",
                  SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, -1);
    m_close = make(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);

    const UINT dpi = GetDpiForWindow(m_hwnd);
    CreateFonts(dpi);
    SIZE client{};
    Layout(dpi, &client);
    RECT r{ 0, 0, client.cx, client.cy };
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, dpi);
    SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void SettingsWindow::CreateFonts(UINT dpi) {
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW body = ncm.lfMessageFont;
    LOGFONTW heading = body;
    heading.lfHeight = MulDiv(body.lfHeight, 6, 5);
    heading.lfWeight = FW_SEMIBOLD;
    if (m_font) DeleteObject(m_font);
    if (m_headingFont) DeleteObject(m_headingFont);
    m_font = CreateFontIndirectW(&body);
    m_headingFont = CreateFontIndirectW(&heading);
}

void SettingsWindow::Layout(UINT dpi, SIZE* clientSize) {
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    auto place = [&](HWND h, int x, int y, int w, int hgt, HFONT font) {
        if (!h) return;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SetWindowPos(h, nullptr, S(x), S(y), S(w), S(hgt), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    int column = 0, y = kMargin, bottom = kMargin;
    bool columnTop = true;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const SettingItem& item = m_items[i];
        const Row& row = m_rows[i];
        const int x = kMargin + column * (kColumnWidth + kColumnGap);
        switch (item.kind) {
        case SettingItem::Column:
            ++column;
            y = kMargin;
            columnTop = true;
            continue;
        case SettingItem::Heading:
            if (!columnTop) y += kHeadingGap;
            place(row.label, x, y, kColumnWidth, kHeadingHeight, m_headingFont);
            y += kHeadingHeight + 2;
            break;
        case SettingItem::Choice:
            place(row.label, x, y, kLabelWidth, kRowHeight - 6, m_font);
            // A drop-down list's height is its open list's; the closed box sizes itself to the font.
            place(row.control, x + kLabelWidth + 10, y, kControlWidth, 320, m_font);
            y += kRowHeight;
            break;
        case SettingItem::Toggle:
            place(row.control, x, y, kColumnWidth, kToggleHeight - 4, m_font);
            y += kToggleHeight;
            break;
        case SettingItem::Action:
            place(row.label, x, y, kColumnWidth - kButtonWidth - 10, kButtonHeight, m_font);
            place(row.control, x + kColumnWidth - kButtonWidth, y, kButtonWidth, kButtonHeight, m_font);
            y += kRowHeight;
            break;
        }
        columnTop = false;
        bottom = std::max(bottom, y);
    }
    const int width = kMargin * 2 + (column + 1) * kColumnWidth + column * kColumnGap;
    const int footer = bottom + 18;
    place(m_hint, kMargin, footer, width - kMargin * 3 - 96, kButtonHeight, m_font);
    place(m_close, width - kMargin - 96, footer, 96, kButtonHeight, m_font);
    if (clientSize) *clientSize = { S(width), S(footer + kButtonHeight + kMargin) };
}

void SettingsWindow::Show(HWND owner) {
    if (!m_hwnd && !Create(owner)) return;
    if (GetWindow(m_hwnd, GW_OWNER) != owner) SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    Refresh(true);
    if (!IsWindowVisible(m_hwnd)) {
        // Centred on the monitor the visualiser is on.
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
        RECT r;
        GetWindowRect(m_hwnd, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(m_hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOW);
        for (const Row& row : m_rows) if (row.control && IsWindowEnabled(row.control)) { SetFocus(row.control); break; }
    }
    SetForegroundWindow(m_hwnd);
}

void SettingsWindow::Hide() {
    if (!Visible()) return;
    if (HWND owner = GetWindow(m_hwnd, GW_OWNER)) SetForegroundWindow(owner);
    ShowWindow(m_hwnd, SW_HIDE);
    if (onHidden) onHidden();
}

void SettingsWindow::Refresh(bool full) {
    if (!m_hwnd) return;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const SettingItem& item = m_items[i];
        Row& row = m_rows[i];
        if (!row.control) continue;
        const bool enabled = item.enabled ? item.enabled() : true;
        if (!IsWindowEnabled(row.control) != !enabled) {
            EnableWindow(row.control, enabled);
            if (row.label) EnableWindow(row.label, enabled);
        }
        switch (item.kind) {
        case SettingItem::Choice: {
            if (SendMessageW(row.control, CB_GETDROPPEDSTATE, 0, 0)) break;   // don't move it under the user
            if ((full || row.options.empty()) && item.options) {
                std::vector<std::wstring> options = item.options();
                if (options != row.options) {
                    row.options = std::move(options);
                    SendMessageW(row.control, CB_RESETCONTENT, 0, 0);
                    for (const std::wstring& o : row.options) SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(o.c_str()));
                }
            }
            const int value = item.get ? item.get() : -1;
            if (SendMessageW(row.control, CB_GETCURSEL, 0, 0) != value) SendMessageW(row.control, CB_SETCURSEL, value, 0);
            break;
        }
        case SettingItem::Toggle: {
            const WPARAM state = item.get && item.get() ? BST_CHECKED : BST_UNCHECKED;
            if (static_cast<WPARAM>(SendMessageW(row.control, BM_GETCHECK, 0, 0)) != state) SendMessageW(row.control, BM_SETCHECK, state, 0);
            break;
        }
        case SettingItem::Action: {
            std::wstring text = item.text ? item.text() : item.label;
            if (text != row.text) { row.text = text; SetWindowTextW(row.label, text.c_str()); }
            std::wstring button = item.button ? item.button() : std::wstring(L"…");
            if (button != row.button) { row.button = button; SetWindowTextW(row.control, button.c_str()); }
            break;
        }
        default:
            break;
        }
    }
}

void SettingsWindow::OnCommand(int id, int code, HWND control) {
    if (id == IDOK || id == IDCANCEL) { Hide(); return; }
    const int index = id - kFirstId;
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;
    const SettingItem& item = m_items[index];
    if (!item.set) return;
    switch (item.kind) {
    case SettingItem::Choice:
        if (code != CBN_SELCHANGE) return;
        item.set(static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0)));
        break;
    case SettingItem::Toggle:
        if (code != BN_CLICKED) return;
        item.set(SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
        break;
    case SettingItem::Action:
        if (code != BN_CLICKED) return;
        item.set(0);
        break;
    default:
        return;
    }
    Refresh();   // one change can move others (e.g. a stream mode turns the bars on)
}

LRESULT CALLBACK SettingsWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) Refresh(true);   // devices or monitors may have come and gone
        break;

    case WM_CTLCOLORSTATIC: {   // labels and check boxes on the window colour, the hint greyed
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(reinterpret_cast<HWND>(lp) == m_hint ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wp);
        CreateFonts(dpi);
        SIZE client{};
        Layout(dpi, &client);
        RECT r{ 0, 0, client.cx, client.cy };
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, dpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        m_rows.clear();
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
