#include "media/NowPlaying.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Control;

namespace {
constexpr DWORD kPollMs = 1500;
constexpr int kArtSize = 48;      // album art is scaled down to this before analysis
constexpr int kArtRetries = 6;    // players often publish the art a few seconds after the title

bool PaletteFromThumbnail(const GlobalSystemMediaTransportControlsSessionMediaProperties& props, TrackPalette& out) {
    auto thumbnail = props.Thumbnail();
    if (!thumbnail) return false;
    auto stream = thumbnail.OpenReadAsync().get();
    auto decoder = BitmapDecoder::CreateAsync(stream).get();
    BitmapTransform transform;
    transform.ScaledWidth(kArtSize);
    transform.ScaledHeight(kArtSize);
    transform.InterpolationMode(BitmapInterpolationMode::Fant);
    auto pixels = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, transform,
        ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage).get();
    auto data = pixels.DetachPixelData();
    return data.size() >= kArtSize * kArtSize * 4 && ExtractPalette(data.data(), kArtSize, kArtSize, out);
}
} // namespace

void NowPlaying::Start() {
    if (m_thread.joinable()) return;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = std::thread(&NowPlaying::ThreadMain, this);
}

void NowPlaying::Stop() {
    if (!m_thread.joinable()) return;
    SetEvent(m_stopEvent);
    m_thread.join();
    CloseHandle(m_stopEvent);
    CloseHandle(m_commandEvent);
    m_stopEvent = m_commandEvent = nullptr;
}

void NowPlaying::Send(Command command) {
    if (!m_commandEvent) return;
    m_command.store(static_cast<int>(command));
    SetEvent(m_commandEvent);
}

TrackInfo NowPlaying::Get() const {
    std::lock_guard lock(m_mutex);
    return m_info;
}

void NowPlaying::ThreadMain() {
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (...) {
        return;
    }

    GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
    std::wstring currentKey;
    int artAttempts = 0;
    bool haveArt = false;

    do {
        try {
            if (!manager) manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            // A command from the keys or the menu: the player that's playing, else Windows' current one.
            if (const auto command = static_cast<Command>(m_command.exchange(0)); command != Command::None) {
                auto target = manager.GetCurrentSession();
                for (const auto& s : manager.GetSessions())
                    if (s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) { target = s; break; }
                if (!target) {
                    m_noPlayer.store(true);
                } else if (command == Command::PlayPause) {
                    target.TryTogglePlayPauseAsync().get();
                } else if (command == Command::Next) {
                    target.TrySkipNextAsync().get();
                } else {
                    target.TrySkipPreviousAsync().get();
                }
            }
            // Prefer a session that is actually playing (e.g. Spotify playing while a browser tab
            // is paused); fall back to Windows' notion of the current session.
            auto session = manager.GetCurrentSession();
            for (const auto& s : manager.GetSessions()) {
                if (s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                    session = s;
                    break;
                }
            }
            if (!session) continue;
            auto props = session.TryGetMediaPropertiesAsync().get();
            const std::wstring title(props.Title());
            const std::wstring artist(props.Artist());
            if (title.empty()) continue;

            const std::wstring key = title + L'\x1f' + artist;
            const bool newTrack = key != currentKey;
            if (newTrack) {
                currentKey = key;
                artAttempts = 0;
                haveArt = false;
                std::lock_guard lock(m_mutex);
                m_info.title = title;
                m_info.artist = artist;
                m_info.hasPalette = false;
                ++m_info.trackSerial;
                m_serial.fetch_add(1, std::memory_order_release);
            }
            if (!haveArt && artAttempts < kArtRetries) {
                ++artAttempts;
                TrackPalette palette{};
                bool ok = false;
                try { ok = PaletteFromThumbnail(props, palette); } catch (...) {}
                if (ok) {
                    haveArt = true;
                    std::lock_guard lock(m_mutex);
                    m_info.hasPalette = true;
                    m_info.palette = palette;
                    ++m_info.paletteSerial;
                    m_serial.fetch_add(1, std::memory_order_release);
                }
            }
        } catch (...) {
            manager = nullptr;   // e.g. the media service restarted: reconnect next poll
        }
    } while ([&] {   // until stopped: poll, or wake at once for a command
        const HANDLE events[2] = { m_stopEvent, m_commandEvent };
        return WaitForMultipleObjects(2, events, FALSE, kPollMs) != WAIT_OBJECT_0;
    }());

    manager = nullptr;
    uninit_apartment();
}
