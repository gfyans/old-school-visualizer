#include "update/Updater.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifndef VISUALIZER_VERSION
#define VISUALIZER_VERSION "0.0.0"
#endif

namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kLatest[] = L"/repos/gfyans/old-school-visualizer/releases/latest";
constexpr wchar_t kAgent[] = L"old-school-visualizer-updater";   // GitHub requires a User-Agent
constexpr int64_t kCheckEvery = 24 * 60 * 60;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII only here
std::string Narrow(const std::wstring& s) {   // ASCII only here (version numbers)
    std::string r;
    for (wchar_t c : s) r += static_cast<char>(c);
    return r;
}

// This build's version. Developer switch for testing updates: VISUALIZER_PRETEND_VERSION=1.0.0
// makes the app behave as an older build (it then offers the latest release).
const std::string& Current() {
    static const std::string v = [] {
        char env[32];
        const DWORD n = GetEnvironmentVariableA("VISUALIZER_PRETEND_VERSION", env, sizeof(env));
        return n > 0 && n < sizeof(env) ? std::string(env, n) : std::string(VISUALIZER_VERSION);
    }();
    return v;
}

// --- Paths next to the running exe -------------------------------------------------------------
std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}
std::wstring Sibling(const wchar_t* suffix) {   // Visualizer.exe -> Visualizer<suffix>
    std::wstring p = ExePath();
    const size_t dot = p.find_last_of(L'.');
    return (dot == std::wstring::npos ? p : p.substr(0, dot)) + suffix;
}
std::wstring UpdatePath() { return Sibling(L".update.exe"); }
std::wstring HashPath() { return Sibling(L".update.sha256"); }
std::wstring OldPath() { return Sibling(L".old.exe"); }

// --- Versions ------------------------------------------------------------------------------------
// "v1.10.2" -> {1, 10, 2}; compares part by part.
bool Newer(const std::string& candidate, const std::string& current) {
    auto parse = [](const std::string& s) {
        int v[3] = {};
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        for (int part = 0; part < 3 && i < s.size(); ++part) {
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) v[part] = v[part] * 10 + (s[i++] - '0');
            if (i < s.size() && s[i] == '.') ++i; else break;
        }
        return std::vector<int>(v, v + 3);
    };
    return parse(candidate) > parse(current);
}

// --- HTTPS GET (WinHTTP: built into Windows; follows GitHub's redirect to its file host) ------
bool HttpGet(const std::wstring& host, const std::wstring& path, std::string& body, bool json) {
    body.clear();
    HINTERNET session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request) {
        const wchar_t* headers = json ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
        DWORD status = 0, size = sizeof(status);
        if (WinHttpSendRequest(request, headers, json ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            ok = true;
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
                if (available == 0) break;
                const size_t at = body.size();
                body.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, body.data() + at, available, &read)) { ok = false; break; }
                body.resize(at + read);
                if (body.size() > 64u * 1024 * 1024) { ok = false; break; }   // nothing we expect is this big
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

bool HttpGetUrl(const std::string& url, std::string& body) {
    const std::string prefix = "https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;   // HTTPS only
    const size_t slash = url.find('/', prefix.size());
    if (slash == std::string::npos) return false;
    return HttpGet(Widen(url.substr(prefix.size(), slash - prefix.size())), Widen(url.substr(slash)), body, false);
}

// --- Tiny JSON helpers (the release JSON is GitHub's; we only need a few strings) ----------------
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* end = nullptr) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return {};
    size_t q = json.find('"', json.find(':', k + needle.size()) + 1);
    if (q == std::string::npos) return {};
    std::string out;
    for (size_t i = q + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { out += json[++i]; continue; }
        if (json[i] == '"') { if (end) *end = i; return out; }
        out += json[i];
    }
    return {};
}

// The download URL of the release asset with this exact name.
std::string AssetUrl(const std::string& json, const std::string& name) {
    size_t at = 0;
    for (;;) {
        size_t end = 0;
        const std::string url = JsonString(json, "browser_download_url", at, &end);
        if (url.empty()) return {};
        const size_t slash = url.find_last_of('/');
        if (slash != std::string::npos && url.substr(slash + 1) == name) return url;
        at = end + 1;
    }
}

// --- SHA-256 (CNG, built into Windows) -------------------------------------------------------------
std::string Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        static const char* kHex = "0123456789abcdef";
        for (unsigned char b : digest) { hex += kHex[b >> 4]; hex += kHex[b & 15]; }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// The waiting update, if it's there and still matches the hash noted when it was verified.
bool PendingIsValid(std::wstring* version = nullptr) {
    std::string exe, note;
    if (!ReadFileBytes(UpdatePath(), exe) || !ReadFileBytes(HashPath(), note) || exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z')
        return false;
    // Note format: "<sha256 hex> <version>"
    const size_t space = note.find(' ');
    if (space == std::string::npos || note.substr(0, space) != Sha256Hex(exe)) return false;
    if (version) {
        std::string v = note.substr(space + 1);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        *version = Widen(v);
    }
    return true;
}

// Swap the waiting update in: rename the running exe aside, move the update into its place.
bool Swap() {
    if (!PendingIsValid()) return false;
    const std::wstring exe = ExePath(), old = OldPath(), update = UpdatePath();
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    if (!MoveFileExW(update.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);   // put it back
        return false;
    }
    DeleteFileW(HashPath().c_str());
    return true;
}

bool Launch(const std::wstring& exe, const wchar_t* args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

const wchar_t* Updater::CurrentVersion() {
    static const std::wstring v = Widen(Current());
    return v.c_str();
}

Updater::~Updater() {
    if (m_thread.joinable()) m_thread.join();   // a check takes seconds at most (timeouts above)
}

void Updater::Post(const std::wstring& message) {
    std::lock_guard lock(m_mutex);
    m_message = message;
}

std::wstring Updater::TakeMessage() {
    std::lock_guard lock(m_mutex);
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring Updater::ReadyVersion() const {
    std::lock_guard lock(m_mutex);
    return m_readyVersion;
}

int64_t Updater::Start(int64_t lastCheck, bool force) {
    // An update downloaded last time but not yet applied still counts.
    std::wstring pending;
    if (!m_ready.load() && PendingIsValid(&pending) && Newer(Narrow(pending), Current())) {
        std::lock_guard lock(m_mutex);
        m_readyVersion = pending;
        m_ready.store(true);
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (m_busy.load() || (!force && now - lastCheck < kCheckEvery)) return lastCheck;
    if (m_thread.joinable()) m_thread.join();
    m_busy.store(true);
    m_thread = std::thread(&Updater::Run, this, force);
    return now;
}

void Updater::Run(bool force) {
    std::string json;
    if (!HttpGet(kHost, kLatest, json, true)) {
        if (force) Post(L"Couldn't check for updates (no connection to GitHub?)");
        m_busy.store(false);
        return;
    }
    const std::string tag = JsonString(json, "tag_name");
    if (tag.empty() || !Newer(tag, Current())) {
        if (force) Post(std::wstring(L"You're on the latest version (") + CurrentVersion() + L")");
        m_busy.store(false);
        return;
    }
    const std::wstring version = Widen(tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag);
    if (m_ready.load() && ReadyVersion() == version) {   // already downloaded
        if (force) Post(L"Version " + version + L" is ready: press U to restart into it");
        m_busy.store(false);
        return;
    }

    // Download the exe and its checksum; keep it only if they match.
    const std::string exeUrl = AssetUrl(json, "Visualizer.exe"), sumUrl = AssetUrl(json, "Visualizer.exe.sha256");
    std::string exe, sum;
    if (exeUrl.empty() || sumUrl.empty()) {
        if (force) Post(L"Version " + version + L" is out, but without a checksum to verify it: download it from GitHub");
        m_busy.store(false);
        return;
    }
    if (force) Post(L"Downloading version " + version + L"…");
    if (!HttpGetUrl(exeUrl, exe) || !HttpGetUrl(sumUrl, sum)) {
        if (force) Post(L"Downloading the update failed");
        m_busy.store(false);
        return;
    }
    std::string expected = sum.substr(0, std::min<size_t>(64, sum.size()));
    std::transform(expected.begin(), expected.end(), expected.begin(), [](char c) { return static_cast<char>(tolower(c)); });
    const std::string actual = Sha256Hex(exe);
    if (expected.size() != 64 || actual != expected || exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z') {
        Post(L"The downloaded update didn't verify, so it was discarded");
        m_busy.store(false);
        return;
    }
    const std::wstring part = UpdatePath() + L".part";
    if (!WriteFileBytes(part, exe) || !MoveFileExW(part.c_str(), UpdatePath().c_str(), MOVEFILE_REPLACE_EXISTING) ||
        !WriteFileBytes(HashPath(), actual + " " + Narrow(version))) {
        DeleteFileW(part.c_str());
        Post(L"Can't save the update next to the app (is its folder writable?)");
        m_busy.store(false);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_readyVersion = version;
    }
    m_ready.store(true);
    Post(L"Version " + version + L" is ready: it'll start next time, or press U to restart into it now");
    m_busy.store(false);
}

bool Updater::RestartNow() {
    if (!m_ready.load() || !Swap()) return false;
    m_ready.store(false);
    return Launch(ExePath(), L"--updated");
}

void Updater::ApplyOnExit() {
    if (m_thread.joinable()) m_thread.join();
    if (m_ready.load() && Swap()) m_ready.store(false);
}

bool Updater::ApplyAtStartup() {
    // Tidy up the previous version left behind by the last swap (it may still be closing).
    for (int i = 0; i < 10 && GetFileAttributesW(OldPath().c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        if (DeleteFileW(OldPath().c_str())) break;
        Sleep(200);
    }
    std::wstring pending;
    if (!PendingIsValid(&pending)) {
        DeleteFileW(UpdatePath().c_str());   // incomplete or tampered: start again later
        DeleteFileW(HashPath().c_str());
        return false;
    }
    if (!Newer(Narrow(pending), Current())) {   // already running it (or newer)
        DeleteFileW(UpdatePath().c_str());
        DeleteFileW(HashPath().c_str());
        return false;
    }
    return Swap() && Launch(ExePath(), L"--updated");
}
