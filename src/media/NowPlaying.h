#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "media/TrackPalette.h"

struct TrackInfo {
    std::wstring title;
    std::wstring artist;
    bool hasPalette = false;
    TrackPalette palette{};
    uint32_t trackSerial = 0;     // changes when the song changes
    uint32_t paletteSerial = 0;   // changes when album colours arrive (may lag the title)
};

// Watches Windows' media sessions (what Spotify, browsers etc. publish to the
// volume flyout / lock screen) on a background thread: current title, artist,
// and a palette extracted from the album art. Polls every 1.5 s; very cheap.
class NowPlaying {
public:
    ~NowPlaying() { Stop(); }
    void Start();
    void Stop();

    uint32_t Serial() const { return m_serial.load(std::memory_order_acquire); }   // bumps on any change
    TrackInfo Get() const;

private:
    void ThreadMain();

    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    mutable std::mutex m_mutex;
    TrackInfo m_info;
    std::atomic<uint32_t> m_serial{ 0 };
};
