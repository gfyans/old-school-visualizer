#include "viz/detonation/DetonationVisualizer.h"
#include "render/Renderer.h"
#include "g_detonation_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct DetonationConstants {   // matches cbuffer Detonation in detonation_ps.hlsl
    float camPos[3];   float focal;
    float camRight[3]; float time;
    float camUp[3];    float look;
    float camFwd[3];   float charge;
    float domeR, domeWhite, domeOn, flash;
    float ruinR, regrow, swirl, cloudRing;
    float ringAge[4];
    float ringHit[4];
    float shockR, lights, flicker, spire;
    float glowCol[3];  float intensity;
    float pulse, energy, pad[2];
};
static_assert(sizeof(DetonationConstants) == sizeof(float) * 44);

const wchar_t* kLooks[] = { L"Night", L"Ink", L"Dusk" };
constexpr int kLookCount = 3;
const wchar_t* kShots[] = { L"Orbit", L"Avenue", L"Rooftops", L"Overhead", L"Witness" };
// Window and street light colour per look.
const float kGlow[kLookCount][3] = { { 1.0f, 0.62f, 0.3f }, { 1.0f, 0.62f, 0.3f }, { 1.0f, 0.78f, 0.5f } };

constexpr float kPi = 3.14159265f;

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}
float Smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(Dot(a, a), 1e-12f))); }

} // namespace

DetonationVisualizer::DetonationVisualizer() : m_rng(std::random_device{}()) {
    m_look = 0;
    m_seedA = std::uniform_real_distribution<float>(0.0f, 2.0f * kPi)(m_rng);
    m_seedB = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
    // Developer switch: VISUALIZER_DETONATION=<look 0-2>,<shot 0-4>[,<seconds into a blast>] fixes the look
    // and shot (and optionally freezes a moment of the blast) for comparisons.
    wchar_t env[32];
    if (GetEnvironmentVariableW(L"VISUALIZER_DETONATION", env, ARRAYSIZE(env)) > 0) {
        int look = 0, shot = 0;
        float blast = -1.0f;
        if (swscanf_s(env, L"%d,%d,%f", &look, &shot, &blast) >= 2 && look >= 0 && look < kLookCount && shot >= 0 && shot < kShotCount) {
            m_look = look;
            m_shot = shot;
            m_locked = true;
            if (blast >= 0.0f) m_freeze = blast;
        }
    }
}

bool DetonationVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_detonation_ps, sizeof(g_detonation_ps), nullptr, &m_ps))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(DetonationConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
}

void DetonationVisualizer::ReleaseResources() {
    m_cb.Reset();
    m_ps.Reset();
}

void DetonationVisualizer::Detonate() {
    // A drop while a blast is still going (white dome, void or collapse) erupts again from where the dome
    // is, bigger, with a fresh shockwave.
    const bool again = m_phase != Phase::Calm;
    m_domeStart = again ? m_domeR : 0.0f;
    m_domeMax = again ? std::min(m_domeMax + 8.0f, 46.0f) : 30.0f;
    m_phase = Phase::Blast;
    m_phaseTime = 0.0f;
    m_domeWhite = 1.0f;
    m_flash = 0.7f;
    m_shake = 1.0f;
    m_charge = 0.0f;
    m_peakCharge = 0.0f;
    m_regrow = 0.0f;
    m_sinceBlast = 0.0f;
    m_phraseBeats = 0;
    m_kick = 1.0f;
    if (!m_locked && m_shot != Witness && m_shot != Orbit) Cut(Witness);   // watch it from a distance
    else if (!m_locked && m_shot == Witness) m_shotTime = std::min(m_shotTime, 4.0f);
}

void DetonationVisualizer::Cut(int shot) {
    m_shot = shot;
    m_shotTime = 0.0f;
    m_camTime = 0.0f;
    m_beatsInShot = 0;
    m_seedA = std::uniform_real_distribution<float>(0.0f, 2.0f * kPi)(m_rng);
    m_seedB = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
}

void DetonationVisualizer::PickShot() {
    // Down the avenue only while there's no dome to fly into.
    const bool domeUp = m_phase != Phase::Calm;
    int next;
    do next = std::uniform_int_distribution<int>(0, kShotCount - 1)(m_rng);
    while (next == m_shot || (domeUp && next == Avenue));
    Cut(next);
}

void DetonationVisualizer::Update(const AudioFeatures& audio, float dt) {
    const float tf = audio.tempoFactor;
    m_time += dt;
    m_shotTime += dt;
    m_camTime += dt * tf * (0.7f + 0.8f * m_intensity);   // the camera drifts faster when the song is big
    m_phaseTime += dt;
    m_sinceBlast += dt;
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);
    m_energy = Smooth(m_energy, audio.silent ? 0.0f : std::min(1.0f, audio.bass * 1.3f), dt, 0.1f, 0.6f);
    m_pulse = Smooth(m_pulse, audio.beat, dt, 0.03f, 0.25f);
    m_kick *= std::exp(-dt / 0.2f);
    m_kickSmooth = Smooth(m_kickSmooth, m_kick, dt, 0.035f, 0.06f);
    m_flash *= std::exp(-dt / 0.25f);
    m_shake *= std::exp(-dt / 1.0f);
    for (float& a : m_ringAge) a += dt;

    // Kicks: a ring out from the spire (or the dome's edge), and a punch through the dome.
    if (audio.beatCount != m_lastBeat) {
        m_lastBeat = audio.beatCount;
        for (int i = 3; i > 0; --i) { m_ringAge[i] = m_ringAge[i - 1]; m_ringHit[i] = m_ringHit[i - 1]; }
        m_ringAge[0] = 0.0f;
        m_ringHit[0] = std::min(1.2f, 0.4f + 0.7f * audio.bass + 0.3f * m_intensity);
        m_kick = std::max(m_kick, std::min(1.0f, 0.45f + 0.8f * audio.bass));
    }

    // The beat grid, for phrase-timed cuts and blasts.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    if (audio.gridBeat != m_lastGrid) {
        m_lastGrid = audio.gridBeat;
        if (gridLocked) { ++m_beatsInShot; ++m_phraseBeats; }
    }
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;

    // What sets off a blast: a drop; the release of a big build-up; a big section going on for a few
    // phrases without one; or, failing all that, a minute of music.
    bool blast = false;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        blast = true;
    }
    if (m_phase == Phase::Calm && !audio.silent) {
        m_peakCharge = std::max(m_peakCharge, m_charge);
        if (m_peakCharge > 0.5f && audio.buildUp < 0.2f && m_intensity > 0.35f) blast = true;   // the build-up let go
        if (m_peakCharge > 0.0f && m_charge < 0.05f && !blast) m_peakCharge = 0.0f;
        const bool phrase = gridLocked ? (onDownbeat && m_phraseBeats >= 32) : true;
        if (phrase && m_sinceBlast > 22.0f && m_regrow > 0.5f && m_intensity > 0.5f) blast = true;
        if (m_sinceBlast > 55.0f && m_intensity > 0.2f && (onDownbeat || !gridLocked)) blast = true;
    }
    if (blast && m_freeze < 0.0f) Detonate();   // even mid-blast: it erupts again

    // Developer switch: hold one moment of a blast.
    if (m_freeze >= 0.0f) {
        m_phase = m_freeze < 4.2f ? Phase::Blast : Phase::Void;
        m_phaseTime = m_freeze < 4.2f ? m_freeze : std::min(m_freeze - 4.2f, 7.9f);
        m_domeStart = 0.0f;
        m_domeMax = 30.0f;
        m_ruinR = 0.0f;
    }

    // The blast's life.
    switch (m_phase) {
    case Phase::Calm:
        m_charge = Smooth(m_charge, std::clamp((audio.buildUp - 0.1f) / 0.8f, 0.0f, 1.0f), dt, 0.6f, 1.5f);
        m_regrow = std::min(1.0f, m_regrow + dt / 16.0f);   // the city rebuilds from the rim inwards
        m_domeR = 0.0f;
        break;
    case Phase::Blast: {
        const float t = m_phaseTime;
        m_domeR = m_domeStart + (m_domeMax - m_domeStart) * (1.0f - std::exp(-t * 1.4f)) + 0.8f * m_kickSmooth;
        m_shockR = m_domeR * 1.1f + t * 26.0f;
        m_cloudRing = t * 45.0f;
        m_domeWhite = 1.0f - Smoothstep(3.0f, 4.2f, t);   // inverts to the void
        m_ruinR = std::max(m_ruinR, m_domeR * 1.06f);
        m_charge = 0.0f;
        if (t > 4.2f) { m_phase = Phase::Void; m_phaseTime = 0.0f; }
        break;
    }
    case Phase::Void: {
        // Hangs there, punching out with every kick and swelling with the bass, while the song stays big.
        m_domeR = m_domeMax * (1.0f + 0.06f * m_kickSmooth + 0.03f * m_energy);
        m_domeWhite = 0.0f;
        m_cloudRing = m_shockR = 0.0f;
        const float t = m_phaseTime;
        if ((t > 4.0f && m_intensity < 0.45f) || t > 18.0f) { m_phase = Phase::Collapse; m_phaseTime = 0.0f; }
        break;
    }
    case Phase::Collapse: {
        const float k = 1.0f - Smoothstep(0.0f, 1.0f, m_phaseTime / 1.4f);
        m_domeR = m_domeMax * k * k;
        m_domeWhite = 0.6f * (1.0f - k);   // it burns white as it implodes
        if (m_phaseTime > 1.4f) {
            m_phase = Phase::Calm;
            m_phaseTime = 0.0f;
            m_flash = std::max(m_flash, 0.4f);
            m_regrow = 0.0f;
            m_domeR = 0.0f;
        }
        break;
    }
    }
    if (m_cloudRing > 240.0f) m_cloudRing = 0.0f;
    if (m_shockR > 220.0f) m_shockR = 0.0f;
    if (m_phase == Phase::Calm && m_ruinR > 0.0f && m_regrow >= 1.0f) m_ruinR = 0.0f;
    // The orbit eases back to keep the dome in frame.
    m_pullBack = Smooth(m_pullBack, m_phase == Phase::Calm ? 0.0f : 18.0f, dt, 2.0f, 4.0f);
    m_swirl += dt * tf * (0.03f + 0.5f * m_charge * m_charge + (m_phase == Phase::Blast ? 0.15f : 0.0f));

    // The director: cut on phrases (8 bars, on the downbeat; 4 when the song is big), or after a while
    // without a beat grid.
    const bool big = m_intensity > 0.6f;
    const int phraseLen = big ? 16 : 32;
    const bool afterBlast = m_phase == Phase::Blast && m_phaseTime < 6.0f;
    if (!m_locked && !afterBlast &&
        ((m_beatsInShot >= phraseLen && onDownbeat && m_shotTime > (big ? 6.0f : 10.0f)) ||
         (!gridLocked && m_shotTime > (big ? 12.0f : 20.0f)) || m_shotTime > 40.0f))
        PickShot();
    if (!m_locked && m_shot == Avenue && m_phase != Phase::Calm) PickShot();

    DetonationConstants c{};
    float cam[12];
    Camera(cam);
    memcpy(c.camPos, cam, 3 * sizeof(float));
    memcpy(c.camRight, cam + 3, 3 * sizeof(float));
    memcpy(c.camUp, cam + 6, 3 * sizeof(float));
    memcpy(c.camFwd, cam + 9, 3 * sizeof(float));
    c.focal = m_shot == Avenue ? 1.3f : m_shot == Witness ? 2.2f : 1.7f;
    c.time = std::fmod(m_time, 2000.0f);
    c.look = static_cast<float>(m_look);
    c.charge = m_charge;
    c.domeR = m_domeR;
    c.domeWhite = m_domeWhite;
    c.domeOn = m_phase == Phase::Calm ? 0.0f : 1.0f;
    c.flash = m_flash;
    c.ruinR = m_ruinR;
    c.regrow = m_regrow;
    c.swirl = std::fmod(m_swirl, 1000.0f);
    c.cloudRing = m_cloudRing;
    memcpy(c.ringAge, m_ringAge, sizeof(c.ringAge));
    memcpy(c.ringHit, m_ringHit, sizeof(c.ringHit));
    c.shockR = m_shockR;
    c.lights = 0.3f + 0.35f * m_intensity;
    c.flicker = m_charge * m_charge * 0.8f;
    c.spire = m_phase == Phase::Calm ? (m_ruinR > 0.0f ? Smoothstep(0.85f, 1.0f, m_regrow) : 1.0f) : 0.0f;
    memcpy(c.glowCol, kGlow[m_look], sizeof(c.glowCol));
    if (m_hasArt && m_look != 1)
        for (int i = 0; i < 3; ++i) c.glowCol[i] = 0.5f * c.glowCol[i] + 0.6f * m_art.hot[i];
    c.intensity = m_intensity;
    c.pulse = m_kickSmooth;
    c.energy = m_energy;
    // A soft push in on the kicks when the song is big: a breath, not a jolt.
    c.focal *= 1.0f + 0.025f * m_kickSmooth * Smoothstep(0.3f, 0.8f, m_intensity);
    memcpy(m_constants, &c, sizeof(c));
}

// Where the camera is for the current shot, with a gentle rumble after a blast.
void DetonationVisualizer::Camera(float out[12]) const {
    const float s = m_camTime;
    const float a = m_seedA;
    const float domeLift = m_phase == Phase::Calm ? 0.0f : m_domeR * 0.3f;
    V3 pos{}, target{}, up{ 0, 1, 0 };
    switch (m_shot) {
    case Orbit: {
        const float ang = a + s * 0.04f;
        const float radius = 42.0f + 6.0f * std::sin(s * 0.03f) + m_pullBack;
        pos = { std::cos(ang) * radius, 19.0f + 3.0f * std::sin(s * 0.05f), std::sin(ang) * radius };
        target = { 0.0f, 4.0f + domeLift, 0.0f };
        break;
    }
    case Avenue: {
        // Down an avenue (every 7th row of cells, the 3rd in each), low between the buildings.
        const int k = static_cast<int>(m_seedB * 6.0f) - 3;
        const float lane = 7.0f * k + 3.5f + 0.12f * std::sin(s * 0.4f);
        const float along = -70.0f + s * 2.4f;
        const float height = 0.9f + 0.25f * std::sin(s * 0.3f);
        const bool alongX = a > kPi;
        pos = alongX ? V3{ along, height, lane } : V3{ lane, height, along };
        const V3 ahead = alongX ? V3{ 10.0f, 0.6f, 0.0f } : V3{ 0.0f, 0.6f, 10.0f };
        target = pos + ahead;
        break;
    }
    case Rooftops: {
        const float ang = a + s * 0.018f;
        const float radius = 46.0f;
        pos = { std::cos(ang) * radius, 7.5f + 0.6f * std::sin(s * 0.2f), std::sin(ang) * radius };
        const V3 tangent{ -std::sin(ang), 0.0f, std::cos(ang) };
        target = pos + tangent * 6.0f + V3{ -pos.x, 0.0f, -pos.z } * 0.12f + V3{ 0.0f, -0.6f, 0.0f };
        break;
    }
    case Overhead: {
        const float ang = a + s * 0.02f;
        pos = { 3.0f * std::sin(s * 0.05f), 25.0f, 3.0f * std::cos(s * 0.04f) };
        target = pos + V3{ 0.0f, -1.0f, 0.0f };
        up = { std::cos(ang), 0.0f, std::sin(ang) };
        break;
    }
    default: {   // Witness: far out, above the low suburbs, slowly pushing in
        const float dist = 95.0f - std::min(s, 40.0f) * 0.6f;
        pos = { std::cos(a) * dist, 9.0f, std::sin(a) * dist };
        target = { 0.0f, 9.0f + domeLift * 0.8f, 0.0f };
        break;
    }
    }
    if (m_shake > 0.01f && m_shot != Avenue) {
        pos.x += m_shake * 0.25f * std::sin(m_time * 21.0f);
        pos.y += m_shake * 0.2f * std::sin(m_time * 27.0f + 1.0f);
    }
    const V3 fwd = Normalize(target - pos);
    const V3 right = Normalize(Cross(up, fwd));
    const V3 camUp = Cross(fwd, right);
    const V3 v[4] = { pos, right, camUp, fwd };
    for (int i = 0; i < 4; ++i) { out[i * 3] = v[i].x; out[i * 3 + 1] = v[i].y; out[i * 3 + 2] = v[i].z; }
}

void DetonationVisualizer::Render(Renderer& renderer, float, float) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(DetonationConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    renderer.DrawFullscreen(m_ps.Get());
}

bool DetonationVisualizer::OnKey(UINT vk) {
    switch (vk) {
    case 'N':
        m_look = (m_look + 1) % kLookCount;
        m_message = kLooks[m_look];
        return true;
    case 'M':
        PickShot();
        m_message = kShots[m_shot];
        return true;
    case 'Q':
        Detonate();
        return true;
    case VK_SPACE:
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    default:
        return false;
    }
}

std::wstring DetonationVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring DetonationVisualizer::Status() const {
    const wchar_t* phase = m_phase == Phase::Calm ? (m_regrow < 1.0f ? L"rebuilding" : m_charge > 0.1f ? L"charging" : L"calm")
                         : m_phase == Phase::Blast ? L"blast" : m_phase == Phase::Void ? L"void" : L"collapse";
    return std::wstring(kLooks[m_look]) + L" · " + kShots[m_shot] + L" · " + phase + (m_locked ? L" (locked)" : L"");
}

void DetonationVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const float cool[3] = { 0.35f, 0.75f, 1.0f }, hot[3] = { 1.0f, 0.35f, 0.1f };
    memcpy(lo, cool, sizeof(cool));
    memcpy(hi, hot, sizeof(hot));
}

void DetonationVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) m_look = std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng);
}
