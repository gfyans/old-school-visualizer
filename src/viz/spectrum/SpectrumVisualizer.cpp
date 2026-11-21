#include "viz/spectrum/SpectrumVisualizer.h"
#include "render/Renderer.h"
#include "g_spectrum_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

struct SpectrumVisualizer::Constants {   // matches cbuffer Spectrum in spectrum_ps.hlsl
    float camPos[3];   float focal;
    float camRight[3]; float time;
    float camUp[3];    float look;
    float camFwd[3];   float kick;
    float buildUp, flash, intensity, sparkCount;
    float rotation, pad1[3];
    float lo[3];       float fireworks;
    float hi[3];       float pad;
    float echoAge[kEchoes];
    float bars[kBars];
    float caps[kBars];
    float echo[kEchoes][kBars];
    float sparks[kSparks][4];   // bar-space position, brightness
};

namespace {

const wchar_t* kLooks[] = { L"Studio", L"Radial" };
const wchar_t* kShots[] = { L"Wide", L"Low", L"Hero", L"High", L"Orbit" };

struct Palette { const wchar_t* name; float lo[3], hi[3]; };
const Palette kPalettes[] = {
    { L"Neon",  { 0.10f, 0.45f, 1.00f }, { 1.00f, 0.20f, 0.70f } },
    { L"Ember", { 0.85f, 0.10f, 0.03f }, { 1.00f, 0.80f, 0.25f } },
    { L"Ice",   { 0.10f, 0.30f, 0.95f }, { 0.80f, 0.95f, 1.00f } },
    { L"Acid",  { 0.10f, 0.80f, 0.30f }, { 1.00f, 0.95f, 0.20f } },
};
constexpr int kPaletteCount = 4;

constexpr float kPi = 3.14159265f;
constexpr float kStudioHeight = 22.0f;   // world height of a full bar (bars are 1 unit apart)

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(Dot(a, a), 1e-12f))); }

} // namespace

SpectrumVisualizer::SpectrumVisualizer() : m_rng(std::random_device{}()), m_constants(sizeof(Constants) / sizeof(float)) {
    m_look = std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng);
    m_palette = std::uniform_int_distribution<int>(0, kPaletteCount - 1)(m_rng);
    m_seed = std::uniform_real_distribution<float>(0.0f, 2.0f * kPi)(m_rng);
    for (float& a : m_echoAge) a = 99.0f;
    // Developer switch: VISUALIZER_SPECTRUM=<look 0-1>[,<shot or colours>] fixes the look for comparisons.
    wchar_t env[32];
    if (GetEnvironmentVariableW(L"VISUALIZER_SPECTRUM", env, ARRAYSIZE(env)) > 0) {
        int look = 0, variant = 0;
        if (swscanf_s(env, L"%d,%d", &look, &variant) >= 1 && look >= 0 && look < kLookCount) {
            m_look = look;
            m_shot = std::clamp(variant, 0, kShotCount - 1);
            m_palette = std::clamp(variant, 0, kPaletteCount - 1);
            m_locked = true;
        }
    }
}

bool SpectrumVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_spectrum_ps, sizeof(g_spectrum_ps), nullptr, &m_ps))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
}

void SpectrumVisualizer::ReleaseResources() {
    m_cb.Reset();
    m_ps.Reset();
}

void SpectrumVisualizer::SetLook(int look) {
    m_look = look % kLookCount;
    m_sparkCount = 0;
    m_shotTime = 0.0f;
    m_beatsInShot = 0;
}

void SpectrumVisualizer::Spark(float x, float y, float z, float vx, float vy, float vz, float life) {
    if (m_sparkCount >= kSparks) return;
    m_sparks[m_sparkCount++] = { x, y, z, vx, vy, vz, life, life };
}

// A drop: every peak cap launches; every other one is a rocket that bursts into sparks at the top.
void SpectrumVisualizer::Fireworks() {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int i = 0; i < kBars; ++i) {
        m_cap[i] = std::max(m_cap[i], m_bar[i]);
        m_capV[i] = 1.0f + 0.9f * u(m_rng) + 0.5f * m_bar[i];
        m_capHold[i] = 0.0f;
        m_rocket[i] = (i % 2) == 0;
    }
    m_flash = 1.0f;
    m_fireworks = 3.0f;
    // The ring bursts outward: a volley of echoes.
    for (int k = 0; k < 4; ++k) {
        memcpy(m_echo[m_nextEcho], m_bar, sizeof(m_bar));
        m_echoAge[m_nextEcho] = -0.08f * k;
        m_nextEcho = (m_nextEcho + 1) % kEchoes;
    }
}

void SpectrumVisualizer::Update(const AudioFeatures& audio, float dt) {
    const float tf = audio.tempoFactor;
    m_time += dt;
    m_shotTime += dt * tf;
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.3f, 0.8f);
    m_kick *= std::exp(-dt / 0.18f);
    m_kickSmooth = Smooth(m_kickSmooth, m_kick, dt, 0.03f, 0.05f);
    m_flash *= std::exp(-dt / 0.3f);
    m_fireworks = std::max(0.0f, m_fireworks - dt);
    m_rotation += dt * tf * (0.05f + 0.25f * m_buildUp + 0.1f * m_intensity);
    for (float& a : m_echoAge) a += dt * tf;

    if (audio.beatCount != m_lastBeat) {
        m_lastBeat = audio.beatCount;
        m_kick = std::max(m_kick, std::min(1.0f, 0.45f + 0.8f * audio.bass));
        memcpy(m_echo[m_nextEcho], m_bar, sizeof(m_bar));
        m_echoAge[m_nextEcho] = 0.0f;
        m_nextEcho = (m_nextEcho + 1) % kEchoes;
    }
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        Fireworks();
    }

    // Bars: rise at once, fall steadily (the classic analyser feel); the kick thumps the bass end.
    for (int i = 0; i < kBars; ++i) {
        const float thump = m_kickSmooth * 0.14f * std::max(0.0f, 1.0f - i / 12.0f);
        const float target = std::min(1.05f, std::pow(std::max(audio.spectrum[i], 0.0f), 0.85f) * (0.92f + 0.12f * m_buildUp) + thump);
        m_bar[i] = target > m_bar[i] ? m_bar[i] + (target - m_bar[i]) * (1.0f - std::exp(-dt / 0.025f))
                                     : std::max(target, m_bar[i] - dt * 1.3f);
    }

    // Peak caps: pushed up by their bar, held a moment, then they fall under gravity and bounce on it.
    // Build-ups make them hover; a drop launches them.
    const float gravity = 3.2f * (1.0f - 0.8f * m_buildUp);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < kBars; ++i) {
        const float top = m_bar[i] + 0.012f;
        if (m_cap[i] <= top && m_capV[i] <= 0.0f) {
            // Caught by the bar: a fast-rising bar throws it upward a little.
            const float push = (top - m_cap[i]) / std::max(dt, 1e-3f);
            m_cap[i] = top;
            m_capV[i] = push > 2.0f ? std::min(push * 0.15f, 0.8f) : 0.0f;
            m_capHold[i] = 0.35f;
            m_rocket[i] = false;
            continue;
        }
        if (m_capHold[i] > 0.0f && m_capV[i] <= 0.0f) { m_capHold[i] -= dt; continue; }
        const float vBefore = m_capV[i];
        m_capV[i] -= gravity * dt;
        m_cap[i] += m_capV[i] * dt;
        if (m_rocket[i] && vBefore > 0.0f && m_capV[i] <= 0.0f) {   // the top of its flight: burst
            const float z = u(m_rng) * 0.3f;
            for (int k = 0; k < 3; ++k)
                Spark(i + 0.5f, m_cap[i], z, u(m_rng) * 7.0f, 0.2f + u(m_rng) * 0.5f, u(m_rng) * 3.0f, 1.0f + 0.6f * std::abs(u(m_rng)));
            m_rocket[i] = false;
            m_cap[i] = top;
            m_capV[i] = 0.0f;
        }
        if (m_cap[i] < top) {   // landed on the bar: a small bounce
            m_cap[i] = top;
            m_capV[i] = m_capV[i] < -0.4f ? -m_capV[i] * 0.3f : 0.0f;
        }
        if (m_buildUp > 0.2f) m_cap[i] += 0.004f * m_buildUp * std::sin(m_time * 31.0f + i * 1.7f);   // jitter
    }

    // Sparks: gravity, a little drag, fading out.
    for (int i = 0; i < m_sparkCount;) {
        Particle& p = m_sparks[i];
        p.life -= dt;
        if (p.life <= 0.0f) { p = m_sparks[--m_sparkCount]; continue; }
        const float drag = std::exp(-dt * 1.2f);
        p.vx *= drag; p.vz *= drag;
        p.vy = p.vy * drag - 1.4f * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        ++i;
    }

    // Studio's camera: a new shot every 8 bars (on the downbeat), or every 20 s without a beat grid.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; if (gridLocked) ++m_beatsInShot; }
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && m_look == Studio &&
        ((m_beatsInShot >= 32 && onDownbeat && m_shotTime > 8.0f) || (!gridLocked && m_shotTime > 20.0f))) {
        m_shot = (m_shot + 1 + std::uniform_int_distribution<int>(0, kShotCount - 2)(m_rng)) % kShotCount;
        m_shotTime = 0.0f;
        m_beatsInShot = 0;
        m_seed = std::uniform_real_distribution<float>(0.0f, 2.0f * kPi)(m_rng);
    }

    // Constants.
    Constants& c = *reinterpret_cast<Constants*>(m_constants.data());
    float cam[12];
    Camera(cam);
    memcpy(c.camPos, cam, 3 * sizeof(float));
    memcpy(c.camRight, cam + 3, 3 * sizeof(float));
    memcpy(c.camUp, cam + 6, 3 * sizeof(float));
    memcpy(c.camFwd, cam + 9, 3 * sizeof(float));
    c.focal = (m_shot == High ? 1.5f : m_shot == Hero ? 1.25f : 1.7f) * (1.0f + 0.02f * m_kickSmooth * m_intensity);
    c.time = std::fmod(m_time, 1000.0f);
    c.look = static_cast<float>(m_look);
    c.kick = m_kickSmooth;
    c.buildUp = m_buildUp;
    c.flash = m_flash;
    c.intensity = m_intensity;
    c.sparkCount = static_cast<float>(m_sparkCount);
    c.rotation = std::fmod(m_rotation, 2.0f * kPi);
    const Palette& pal = kPalettes[m_palette];
    for (int i = 0; i < 3; ++i) {
        c.lo[i] = m_hasArt ? 0.5f * pal.lo[i] + 0.6f * m_art.mid[i] : pal.lo[i];
        c.hi[i] = m_hasArt ? 0.4f * pal.hi[i] + 0.7f * m_art.hot[i] : pal.hi[i];
    }
    c.fireworks = std::min(1.0f, m_fireworks);
    memcpy(c.echoAge, m_echoAge, sizeof(c.echoAge));
    memcpy(c.bars, m_bar, sizeof(c.bars));
    memcpy(c.caps, m_cap, sizeof(c.caps));
    memcpy(c.echo, m_echo, sizeof(c.echo));
    for (int i = 0; i < m_sparkCount; ++i) {
        const Particle& p = m_sparks[i];
        c.sparks[i][0] = p.x; c.sparks[i][1] = p.y; c.sparks[i][2] = p.z;
        c.sparks[i][3] = p.life / p.maxLife;
    }
}

// Studio's camera. The bars stand along x (-32..32), 1 unit apart, up to kStudioHeight tall.
void SpectrumVisualizer::Camera(float out[12]) const {
    const float s = m_shotTime;
    const float a = m_seed;
    V3 pos{}, target{};
    switch (m_shot) {
    case Wide:
        pos = { 7.0f * std::sin(s * 0.05f + a), 10.0f + 1.5f * std::sin(s * 0.07f), -50.0f + 3.0f * std::sin(s * 0.04f) };
        target = { 0.0f, 8.0f, 0.0f };
        break;
    case Low: {   // low along the bars from one end, drifting along them
        const float side = std::sin(a) > 0.0f ? 1.0f : -1.0f;
        pos = { side * (44.0f - s * 0.5f), 2.2f, -15.0f };
        target = { side * (10.0f - s * 0.5f), 6.0f, 0.0f };
        break;
    }
    case Hero:   // close in at the bass end, looking up at the bars
        pos = { -27.0f + 2.0f * std::sin(s * 0.08f), 1.4f, -10.0f };
        target = { -18.0f, 9.0f, 0.0f };
        break;
    case High:
        pos = { 10.0f * std::sin(s * 0.03f + a), 42.0f, -34.0f };
        target = { 0.0f, 2.0f, 0.0f };
        break;
    default: {   // Orbit
        const float ang = a + s * 0.05f;
        pos = { std::sin(ang) * 52.0f, 13.0f, -std::abs(std::cos(ang)) * 52.0f - 4.0f };
        target = { 0.0f, 7.0f, 0.0f };
        break;
    }
    }
    const V3 fwd = Normalize(target - pos);
    const V3 right = Normalize(Cross(V3{ 0, 1, 0 }, fwd));
    const V3 up = Cross(fwd, right);
    const V3 v[4] = { pos, right, up, fwd };
    for (int i = 0; i < 4; ++i) { out[i * 3] = v[i].x; out[i * 3 + 1] = v[i].y; out[i * 3 + 2] = v[i].z; }
}

void SpectrumVisualizer::Render(Renderer& renderer, float, float) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants.data(), sizeof(Constants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    renderer.DrawFullscreen(m_ps.Get());
}

bool SpectrumVisualizer::OnKey(UINT vk) {
    switch (vk) {
    case 'N':
        SetLook(m_look + 1);
        m_message = kLooks[m_look];
        return true;
    case 'M':
        if (m_look == Studio) {
            m_shot = (m_shot + 1) % kShotCount;
            m_shotTime = 0.0f;
            m_beatsInShot = 0;
            m_message = kShots[m_shot];
        } else {
            m_palette = (m_palette + 1) % kPaletteCount;
            m_message = m_hasArt ? std::wstring(kPalettes[m_palette].name) + L" + album colours" : kPalettes[m_palette].name;
        }
        return true;
    case 'Q':   // fireworks, as on a drop
        Fireworks();
        return true;
    case VK_SPACE:
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    default:
        return false;
    }
}

std::wstring SpectrumVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring SpectrumVisualizer::Status() const {
    std::wstring s = kLooks[m_look];
    if (m_look == Studio) s += std::wstring(L" · ") + kShots[m_shot];
    s += std::wstring(L" · ") + kPalettes[m_palette].name;
    return s + (m_locked ? L" (locked)" : L"");
}

void SpectrumVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const float* c = reinterpret_cast<const Constants*>(m_constants.data())->lo;
    memcpy(lo, c, 3 * sizeof(float));
    memcpy(hi, reinterpret_cast<const Constants*>(m_constants.data())->hi, 3 * sizeof(float));
}

void SpectrumVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) {
        SetLook(std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng));
        m_palette = std::uniform_int_distribution<int>(0, kPaletteCount - 1)(m_rng);
    }
}
