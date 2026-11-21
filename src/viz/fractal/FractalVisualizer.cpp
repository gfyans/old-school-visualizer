#include "viz/fractal/FractalVisualizer.h"
#include "render/Renderer.h"
#include "g_fractal_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct FractalConstants {   // matches cbuffer Fractal in fractal_ps.hlsl
    float camPos[3];   float focal;
    float camRight[3]; float breath;
    float camUp[3];    float spin;
    float camFwd[3];   float glowAmt;
    float albedo[3];   float emit;
    float emitCol[3];  float sparkle;
    float fog[3];      float flash;
    float pulses[4][4];
    float world, animTime, hyperspeed, tunnel;
    float chase, pad0, pad1, pad2;
};
static_assert(sizeof(FractalConstants) % 16 == 0);
static_assert(sizeof(FractalConstants) <= sizeof(float) * 52);

const wchar_t* kWorldNames[] = { L"Menger halls", L"Mandelbox", L"Apollonian foam", L"Coral", L"Cathedral" };
constexpr int kWorldCount = static_cast<int>(std::size(kWorldNames));
const float kTunnel[kWorldCount] = { 0.32f, 0.30f, 0.28f, 0.30f, 0.30f };

struct NamedPalette { const wchar_t* name; FractalVisualizer::Palette p; };
// surface, glowing veins, fog
const NamedPalette kPalettes[] = {
    { L"Molten",      { { 0.30f, 0.24f, 0.20f }, { 1.00f, 0.42f, 0.10f }, { 0.040f, 0.015f, 0.010f } } },
    { L"Neon",        { { 0.18f, 0.20f, 0.26f }, { 0.15f, 0.80f, 1.00f }, { 0.005f, 0.015f, 0.040f } } },
    { L"Ultraviolet", { { 0.24f, 0.18f, 0.30f }, { 0.90f, 0.20f, 1.00f }, { 0.025f, 0.008f, 0.045f } } },
    { L"Toxic",       { { 0.18f, 0.24f, 0.18f }, { 0.40f, 1.00f, 0.30f }, { 0.008f, 0.035f, 0.015f } } },
    { L"Ice",         { { 0.55f, 0.65f, 0.75f }, { 0.35f, 0.70f, 1.00f }, { 0.040f, 0.060f, 0.090f } } },
    { L"Gold",        { { 0.60f, 0.45f, 0.25f }, { 1.00f, 0.75f, 0.35f }, { 0.035f, 0.025f, 0.015f } } },
};
constexpr int kPaletteCount = static_cast<int>(std::size(kPalettes));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

FractalVisualizer::Palette FromArt(const TrackPalette& a, const FractalVisualizer::Palette& base) {
    FractalVisualizer::Palette p = base;   // keep the fog dark; surfaces and veins from the album
    for (int i = 0; i < 3; ++i) p.albedo[i] = 0.25f + 0.35f * a.mid[i];
    Copy3(p.emit, a.hot);
    return p;
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(Dot(a, a), 1e-12f))); }

// The flight path (matches Path() in fractal_ps.hlsl).
V3 Path(float z) {
    return { std::sin(z * 0.11f) * 1.1f + std::sin(z * 0.037f) * 1.9f,
             std::sin(z * 0.07f) * 0.7f + std::cos(z * 0.029f) * 0.9f, z };
}

} // namespace

FractalVisualizer::FractalVisualizer() : m_rng(std::random_device{}()) {
    m_world = std::uniform_int_distribution<int>(0, kWorldCount - 1)(m_rng);
    m_scheme = std::uniform_int_distribution<int>(0, kPaletteCount - 1)(m_rng);
    m_from = m_to = kPalettes[m_scheme].p;
    m_z = std::uniform_real_distribution<float>(0.0f, 500.0f)(m_rng);
}

bool FractalVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_fractal_ps, sizeof(g_fractal_ps), nullptr, &m_ps))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FractalConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
}

void FractalVisualizer::ReleaseResources() {
    m_cb.Reset();
    m_ps.Reset();
}

FractalVisualizer::Palette FractalVisualizer::CurrentPalette() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Palette r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void FractalVisualizer::NextWorld(float flash, bool barrelRoll) {
    m_world = (m_world + 1 + std::uniform_int_distribution<int>(0, kWorldCount - 2)(m_rng)) % kWorldCount;
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    m_from = m_to = m_hasArt && (m_scheme % 2 == 1) ? FromArt(m_art, kPalettes[m_scheme].p) : kPalettes[m_scheme].p;
    m_blend = 1.0f;   // the flash hides the cut
    m_flash = std::max(m_flash, flash);
    m_spinDir = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f;
    if (barrelRoll) {
        m_barrel = 0.0f;
        m_barrelDir = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f;
    }
    for (auto& p : m_pulses) p[1] = 0.0f;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
}

void FractalVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_silent = audio.silent;
    m_bass = Smooth(m_bass, audio.silent ? 0.0f : audio.bass, dt, 0.05f, 0.35f);
    m_mids = Smooth(m_mids, 0.5f * (audio.lowMid + audio.highMid), dt, 0.3f, 1.0f);
    m_treble = Smooth(m_treble, audio.treble, dt, 0.04f, 0.25f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.6f, 0.4f);
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);

    // Beats: a ring of light ahead, a speed surge and a punch of the field of view.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    bool beat = false;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; beat |= !gridLocked; }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; beat |= gridLocked; }
    if (beat && !audio.silent) {
        m_pulses[m_nextPulse][0] = m_z + 3.0f + 2.5f * m_speed;
        m_pulses[m_nextPulse][1] = 0.5f + 0.9f * audio.bass;
        m_nextPulse = (m_nextPulse + 1) % 4;
        m_kick = std::max(m_kick, 0.3f + 0.5f * audio.bass);
        ++m_beatsSinceChange;
    }
    for (auto& p : m_pulses) p[1] *= std::exp(-dt / 1.6f);
    m_kick *= std::exp(-dt / 0.35f);   // a soft swell, not a jolt

    // Drops: hyperspeed, a barrel roll and a new world.
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop || m_fireDrop) {
        m_lastDrop = audio.dropCount;
        m_fireDrop = false;
        m_hyper = 1.0f;
        if (!m_locked) NextWorld(1.0f, true);
        else { m_flash = 1.0f; m_barrel = 0.0f; }
    }
    m_hyper *= std::exp(-dt / 1.2f);
    m_flash *= std::exp(-dt / 0.3f);
    m_barrel = std::min(1.0f, m_barrel + dt / 1.4f);
    if (m_beatsSinceChange >= 64 && m_timeSinceChange > 30.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    // Every 4 bars in intense sections, the world turns 45 degrees, landing on the downbeat.
    if (onDownbeat && gridLocked && ++m_bars % 4 == 0 && m_intensity > 0.45f && !audio.silent)
        m_turnTarget += m_spinDir * 0.7853982f;
    m_turn = Smooth(m_turn, m_turnTarget, dt, 0.3f, 0.3f);
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 80.0f)) NextWorld(0.8f, false);
    m_blend = std::min(1.0f, m_blend + dt / 4.0f);

    // The flight: faster with intensity and build-ups, surging on kicks, hyperspeed on drops.
    const float cruise = audio.silent ? 0.25f : 0.45f + 0.8f * m_intensity + 0.9f * m_buildUp;
    const float tf = audio.tempoFactor;   // faster songs animate faster
    m_speed = Smooth(m_speed, cruise * tf, dt, 1.5f, 2.5f);
    m_z += dt * (m_speed + 0.25f * m_kick + 6.0f * m_hyper);
    m_spin += dt * tf * m_spinDir * (audio.silent ? 0.03f : 0.04f + 0.2f * m_mids + 0.25f * m_buildUp);

    // Bank into the curves.
    const float ahead = 1.5f;
    const float curve = Path(m_z + ahead).x - 2.0f * Path(m_z).x + Path(m_z - ahead).x;
    m_roll = Smooth(m_roll, std::clamp(-curve * 2.5f, -0.7f, 0.7f), dt, 0.6f, 0.6f);

    // Camera
    const V3 pos = Path(m_z);
    const V3 fwd = Normalize(Path(m_z + 1.2f) - pos);
    V3 right = Normalize(Cross(V3{ 0, 1, 0 }, fwd));
    V3 up = Cross(fwd, right);
    const float e = m_barrel * m_barrel * (3.0f - 2.0f * m_barrel);
    const float roll = m_roll + m_barrelDir * 6.2831853f * (m_barrel < 1.0f ? e : 0.0f) +
                       // a gentle sway in time with the bar
                       (audio.silent ? 0.0f : (gridLocked ? 0.05f + 0.07f * m_intensity : 0.03f) *
                           std::sin(6.2831853f * (gridLocked ? audio.barPhase : m_time * 0.25f)));
    const float cr = std::cos(roll), sr = std::sin(roll);
    const V3 r2 = right * cr + up * sr, u2 = up * cr - right * sr;

    const Palette pal = CurrentPalette();
    FractalConstants c{};
    c.camPos[0] = pos.x; c.camPos[1] = pos.y; c.camPos[2] = pos.z;
    c.camRight[0] = r2.x; c.camRight[1] = r2.y; c.camRight[2] = r2.z;
    c.camUp[0] = u2.x; c.camUp[1] = u2.y; c.camUp[2] = u2.z;
    c.camFwd[0] = fwd.x; c.camFwd[1] = fwd.y; c.camFwd[2] = fwd.z;
    c.focal = 1.25f * (1.0f - 0.35f * m_hyper - 0.12f * m_buildUp) * (1.0f - 0.012f * m_kick);
    c.breath = m_bass;
    c.spin = m_spin + m_turn;
    c.glowAmt = (audio.silent ? 0.5f : 0.8f + 0.8f * m_buildUp + 0.15f * m_kick) + 1.5f * m_hyper;
    Copy3(c.albedo, pal.albedo);
    Copy3(c.emitCol, pal.emit);
    Copy3(c.fog, pal.fog);
    c.emit = (audio.silent ? 0.4f : 0.6f + 1.4f * m_bass + 0.6f * m_buildUp) + 1.5f * m_hyper;
    c.sparkle = m_treble;
    c.flash = m_flash;
    for (int i = 0; i < 4; ++i) { c.pulses[i][0] = m_pulses[i][0]; c.pulses[i][1] = m_pulses[i][1]; }
    c.world = static_cast<float>(m_world);
    c.animTime = m_time;
    c.hyperspeed = m_hyper;
    c.tunnel = kTunnel[m_world];
    c.chase = audio.silent ? 0.0f : (0.35f + 0.9f * m_intensity) * std::clamp((audio.tempoConfidence - 0.3f) * 3.0f, 0.0f, 1.0f);
    memcpy(m_constants, &c, sizeof(c));
}

void FractalVisualizer::Render(Renderer& renderer, float, float) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(FractalConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    renderer.DrawFullscreen(m_ps.Get());
}

bool FractalVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextWorld(0.8f, false);
        m_message = kWorldNames[m_world];
        return true;
    }
    if (vk == 'Q') {
        m_fireDrop = true;
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"World locked" : L"World unlocked";
        return true;
    }
    return false;
}

std::wstring FractalVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring FractalVisualizer::Status() const {
    return std::wstring(kWorldNames[m_world]) + L", " +
           (m_hasArt && (m_scheme % 2 == 1) ? L"album colours" : kPalettes[m_scheme].name) +
           (m_locked ? L" (locked)" : L"");
}

void FractalVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Palette p = CurrentPalette();
    Copy3(lo, p.albedo);
    Copy3(hi, p.emit);
}

void FractalVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked && m_timeSinceChange > 10.0f) NextWorld(0.8f, false);
}
