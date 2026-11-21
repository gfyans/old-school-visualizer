#include "platform/Settings.h"

#include <shlobj.h>
#include <algorithm>
#include <cwchar>

namespace {
constexpr wchar_t kSection[] = L"Visualizer";

std::wstring ReadString(const std::wstring& file, const wchar_t* key) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(kSection, key, L"", buf, ARRAYSIZE(buf), file.c_str());
    return buf;
}

int ReadInt(const std::wstring& file, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, file.c_str()));
}

void WriteString(const std::wstring& file, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), file.c_str());
}

void WriteInt(const std::wstring& file, const wchar_t* key, long value) {
    WriteString(file, key, std::to_wstring(value));
}
}

std::wstring Settings::FilePath() {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\Visualizer";
        CoTaskMemFree(appData);
    }
    if (path.empty()) return {};
    CreateDirectoryW(path.c_str(), nullptr);   // fine if it already exists
    return path + L"\\settings.ini";
}

void Settings::Load() {
    const std::wstring file = FilePath();
    if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    const int l = ReadInt(file, L"WindowLeft", 0), t = ReadInt(file, L"WindowTop", 0);
    const int r = ReadInt(file, L"WindowRight", 0), b = ReadInt(file, L"WindowBottom", 0);
    hasPlacement = r - l >= 200 && b - t >= 150;
    windowRect = { l, t, r, b };
    maximized = ReadInt(file, L"Maximized", 0) != 0;
    fullscreen = ReadInt(file, L"Fullscreen", 0) != 0;
    visualizer = ReadString(file, L"Visualizer");
    audioDevice = ReadString(file, L"AudioDevice");
    monitor = ReadString(file, L"Monitor");
    videoFolder = ReadString(file, L"VideoFolder");
    displayMode = ReadInt(file, L"DisplayMode", 0);
    if (displayMode < 0 || displayMode > 2) displayMode = 0;
    showBars = ReadInt(file, L"ShowBars", 0) != 0;
    albumColours = ReadInt(file, L"AlbumColours", 1) != 0;
    trackTitles = ReadInt(file, L"TrackTitles", 1) != 0;
    sceneChanges = std::clamp(ReadInt(file, L"AutoCycle", 0), 0, 2);
    instantReplay = ReadInt(file, L"InstantReplay", 0) != 0;
    moodColour = ReadInt(file, L"MoodColour", 1) != 0;
    glitchFilter = ReadInt(file, L"GlitchFilter", 0);
    if (glitchFilter < 0 || glitchFilter > 4) glitchFilter = 0;
    autoUpdate = ReadInt(file, L"AutoUpdate", 1) != 0;
    lastUpdateCheck = _wtoi64(ReadString(file, L"LastUpdateCheck").c_str());
}

void Settings::Save() const {
    const std::wstring file = FilePath();
    if (file.empty()) return;
    if (hasPlacement) {
        WriteInt(file, L"WindowLeft", windowRect.left);
        WriteInt(file, L"WindowTop", windowRect.top);
        WriteInt(file, L"WindowRight", windowRect.right);
        WriteInt(file, L"WindowBottom", windowRect.bottom);
        WriteInt(file, L"Maximized", maximized ? 1 : 0);
    }
    WriteInt(file, L"Fullscreen", fullscreen ? 1 : 0);
    WriteString(file, L"Visualizer", visualizer);
    WriteString(file, L"AudioDevice", audioDevice);
    WriteString(file, L"Monitor", monitor);
    WriteString(file, L"VideoFolder", videoFolder);
    WriteInt(file, L"DisplayMode", displayMode);
    WriteInt(file, L"ShowBars", showBars ? 1 : 0);
    WriteInt(file, L"AlbumColours", albumColours ? 1 : 0);
    WriteInt(file, L"TrackTitles", trackTitles ? 1 : 0);
    WriteInt(file, L"AutoCycle", sceneChanges);
    WriteInt(file, L"InstantReplay", instantReplay ? 1 : 0);
    WriteInt(file, L"MoodColour", moodColour ? 1 : 0);
    WriteInt(file, L"GlitchFilter", glitchFilter);
    WriteInt(file, L"AutoUpdate", autoUpdate ? 1 : 0);
    WriteString(file, L"LastUpdateCheck", std::to_wstring(lastUpdateCheck));
}
