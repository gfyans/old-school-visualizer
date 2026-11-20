#pragma once
#include <windows.h>
#include <string>

// User preferences, remembered between runs in %APPDATA%\Visualizer\settings.ini.
// The exe itself stays portable: delete the folder to reset.
struct Settings {
    bool hasPlacement = false;
    RECT windowRect{};          // normal (restored) window rectangle, screen coordinates
    bool maximized = false;
    bool fullscreen = false;
    std::wstring visualizer;    // name of the last visualiser shown
    std::wstring audioDevice;   // endpoint ID; empty = follow the system default
    bool showBars = false;      // equaliser bars along the bottom
    bool albumColours = true;   // take palettes from the playing track's album art
    bool trackTitles = true;    // show title/artist when a song starts

    void Load();
    void Save() const;
    static std::wstring FilePath();
};
