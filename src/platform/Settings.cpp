#include "platform/Settings.h"

#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {
constexpr wchar_t kSection[] = L"Visualizer";
constexpr wchar_t kScaleSection[] = L"RenderScale";

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
    filter = ReadString(file, L"Filter");
    if (filter.empty()) {   // saved by 1.10 and earlier: a number in the menu's order
        static const wchar_t* kOld[] = { L"", L"ASCII", L"Dither", L"LED", L"Scan lines", L"VHS", L"Polaroid", L"Halftone", L"Thermal" };
        const int old = ReadInt(file, L"GlitchFilter", 0);
        if (old > 0 && old < static_cast<int>(ARRAYSIZE(kOld))) filter = kOld[old];
    }
    autoUpdate = ReadInt(file, L"AutoUpdate", 1) != 0;
    lastUpdateCheck = _wtoi64(ReadString(file, L"LastUpdateCheck").c_str());

    // [RenderScale]: "Name=percent" lines, double-null terminated.
    std::wstring section(8192, L'\0');
    const DWORD n = GetPrivateProfileSectionW(kScaleSection, section.data(), static_cast<DWORD>(section.size()), file.c_str());
    for (const wchar_t* p = section.c_str(); p < section.c_str() + n && *p; p += wcslen(p) + 1) {
        const std::wstring line = p;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const int percent = _wtoi(line.c_str() + eq + 1);
        if (percent >= 10 && percent <= 100) renderScales[line.substr(0, eq)] = percent / 100.0f;
    }
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
    WriteString(file, L"Filter", filter.empty() ? std::wstring(L"Off") : filter);
    WritePrivateProfileStringW(kSection, L"GlitchFilter", nullptr, file.c_str());   // replaced by Filter
    WriteInt(file, L"AutoUpdate", autoUpdate ? 1 : 0);
    WriteString(file, L"LastUpdateCheck", std::to_wstring(lastUpdateCheck));
    for (const auto& [name, scale] : renderScales)
        WritePrivateProfileStringW(kScaleSection, name.c_str(), std::to_wstring(std::lround(scale * 100.0f)).c_str(), file.c_str());
}
