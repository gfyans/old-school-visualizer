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
// It can also control the player, like the keyboard's media keys.
class NowPlaying {
public:
    enum class Command { None, PlayPause, Next, Previous };

    ~NowPlaying() { Stop(); }
    void Start();
    void Stop();

    // Sends a command to the music player (done on the background thread, so it never stalls a frame).
    void Send(Command command);
    // True once if the last command found no player to control.
    bool TakeNoPlayer() { return m_noPlayer.exchange(false); }

    uint32_t Serial() const { return m_serial.load(std::memory_order_acquire); }   // bumps on any change
    TrackInfo Get() const;

private:
    void ThreadMain();

    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_commandEvent = nullptr;
    std::atomic<int> m_command{ 0 };
    std::atomic<bool> m_noPlayer{ false };
    mutable std::mutex m_mutex;
    TrackInfo m_info;
    std::atomic<uint32_t> m_serial{ 0 };
};
