#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Keeps the app up to date from the GitHub releases of gfyans/old-school-visualizer.
//
//   1. Start(): in the background, asks GitHub's API for the latest release (at most once a day
//      unless forced). If it's newer than this build, downloads its Visualizer.exe next to this
//      one as "<name>.update.exe" and checks it against the release's Visualizer.exe.sha256.
//      Only a download that matches is kept; its hash is noted in "<name>.update.sha256".
//   2. When the app closes, ApplyOnExit() swaps it in (a running exe can be renamed, not
//      overwritten), so the next launch is the new version. If the app didn't close cleanly,
//      ApplyAtStartup() does the swap on the next start and relaunches. RestartNow() swaps and
//      relaunches straight away (the U key).
//
// Before every swap the downloaded file is hashed again and must still match.
class Updater {
public:
    Updater() = default;
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    static const wchar_t* CurrentVersion();   // this build, e.g. L"1.5.0"

    // Begin a background check (and download) unless one ran in the last day; `force` checks
    // anyway (and reports "up to date" too). lastCheck: unix seconds of the last check (from
    // settings); returns the time to store if a check was started, else lastCheck unchanged.
    int64_t Start(int64_t lastCheck, bool force);

    bool Ready() const { return m_ready.load(); }      // a verified update is waiting
    std::wstring ReadyVersion() const;
    std::wstring TakeMessage();                        // one-off notices, cleared when read

    // Swap in the waiting update and start it (the caller then closes this instance).
    bool RestartNow();
    // On exit: swap in a waiting update so the next launch runs it.
    void ApplyOnExit();

    // At the very start of the program: finish a swap left over from last time (returns true if
    // it relaunched the new version, so this process should exit), and tidy up the old exe.
    static bool ApplyAtStartup();

private:
    void Run(bool force);
    void Post(const std::wstring& message);

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::wstring m_message, m_readyVersion;
    std::atomic<bool> m_ready{ false }, m_busy{ false };
};
