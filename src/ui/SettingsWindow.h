#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

// One row of the settings window. The window knows nothing about the app: each row reads and
// writes its setting through these callbacks, so changes apply as soon as they're made.
struct SettingItem {
    enum Kind { Heading, Choice, Toggle, Action, Column } kind = Heading;
    std::wstring label;                                   // Heading/Choice/Toggle text; Action's default text
    std::function<std::vector<std::wstring>()> options;   // Choice: the list (re-read when the window opens)
    std::function<int()> get;                             // Choice: selected index; Toggle: 0 or 1
    std::function<void(int)> set;                         // Choice/Toggle: the new value; Action: the button
    std::function<bool()> enabled;                        // optional (default: always)
    std::function<std::wstring()> text;                   // Action: the text beside the button (optional)
    std::function<std::wstring()> button;                 // Action: the button's label

    static SettingItem MakeHeading(std::wstring label) { SettingItem i; i.kind = Heading; i.label = std::move(label); return i; }
    static SettingItem MakeColumn() { SettingItem i; i.kind = Column; return i; }
};

// The settings window: a normal window (not a menu) showing every setting and its current value.
// Created on first Show and hidden (not destroyed) when closed. Owned by the main window, so it
// stays in front of it, fullscreen included.
class SettingsWindow {
public:
    ~SettingsWindow();
    void SetItems(std::vector<SettingItem> items) { m_items = std::move(items); }
    void Show(HWND owner);
    void Hide();
    bool Visible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    HWND Handle() const { return m_hwnd; }

    // Re-read every value (e.g. after a key changed one). full = also re-read the Choice lists.
    void Refresh(bool full = false);
    // For the app's message loop: Tab, arrows, Esc and Enter for the controls. True if handled.
    bool HandleDialogMessage(MSG& msg) { return Visible() && IsDialogMessageW(m_hwnd, &msg); }

    std::function<void()> onHidden;   // closed by the user (a good moment to save)

private:
    struct Row {
        HWND label = nullptr;    // Choice/Action/Heading text
        HWND control = nullptr;  // combo box, check box or button
        std::vector<std::wstring> options;
        std::wstring text, button;
    };
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    bool Create(HWND owner);
    void CreateFonts(UINT dpi);
    void Layout(UINT dpi, SIZE* clientSize);
    void OnCommand(int id, int code, HWND control);

    std::vector<SettingItem> m_items;
    std::vector<Row> m_rows;
    HWND m_hwnd = nullptr;
    HWND m_close = nullptr;
    HWND m_hint = nullptr;
    HFONT m_font = nullptr, m_headingFont = nullptr;
};
