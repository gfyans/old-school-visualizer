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
    bool autoCycle = false;     // new visualiser with each song
    bool instantReplay = false; // keep the last 30 s ready to save as a video
    std::wstring videoFolder;   // where recordings go (empty = Videos\Visualizer)
    bool moodColour = true;     // tint the picture by the music's mood and chords
    int displayMode = 0;        // fullscreen with several monitors: 0 one monitor, 1 each its own visualiser, 2 span
    std::wstring monitor;       // the monitor for single-monitor fullscreen (device name; empty = where the window is)

    void Load();
    void Save() const;
    static std::wstring FilePath();
};
