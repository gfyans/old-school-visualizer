#include "viz/aurora/AuroraVisualizer.h"
#include "render/Renderer.h"
#include "g_aurora_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct AuroraConstants {   // matches cbuffer Aurora in aurora_ps.hlsl
    float animTime, brightness, sway, heightScale;
    float low[3];        float flare;
    float high[3];       float flareX;
    float skyTop[3];     float stars;
    float skyHorizon[3]; float ripple;
    float land[3];       float yaw;
    float pitch, pad0, pad1, pad2;
};
static_assert(sizeof(AuroraConstants) % 16 == 0);
static_assert(sizeof(AuroraConstants) <= sizeof(float) * 28);

struct NamedScheme { const wchar_t* name; AuroraVisualizer::Scheme s; };
// low curtain colour, high curtain colour, sky top, sky horizon, land
const NamedScheme kSchemes[] = {
    { L"Classic",     { { 0.15f, 1.00f, 0.50f }, { 0.60f, 0.25f, 0.90f }, { 0.00f, 0.01f, 0.03f }, { 0.02f, 0.05f, 0.09f }, { 0.010f, 0.014f, 0.020f } } },
    { L"Solar storm", { { 0.20f, 1.00f, 0.35f }, { 1.00f, 0.15f, 0.20f }, { 0.01f, 0.01f, 0.03f }, { 0.05f, 0.04f, 0.08f }, { 0.012f, 0.012f, 0.018f } } },
    { L"Arctic",      { { 0.20f, 0.90f, 1.00f }, { 0.70f, 0.90f, 1.00f }, { 0.00f, 0.01f, 0.04f }, { 0.03f, 0.06f, 0.12f }, { 0.010f, 0.014f, 0.024f } } },
    { L"Violet",      { { 0.50f, 0.30f, 1.00f }, { 1.00f, 0.30f, 0.70f }, { 0.01f, 0.00f, 0.04f }, { 0.06f, 0.03f, 0.10f }, { 0.014f, 0.010f, 0.022f } } },
    { L"Emerald",     { { 0.10f, 1.00f, 0.30f }, { 0.20f, 1.00f, 0.75f }, { 0.00f, 0.02f, 0.02f }, { 0.02f, 0.07f, 0.07f }, { 0.008f, 0.016f, 0.016f } } },
};
constexpr int kSchemeCount = static_cast<int>(std::size(kSchemes));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

AuroraVisualizer::Scheme FromArt(const TrackPalette& a, const AuroraVisualizer::Scheme& base) {
    AuroraVisualizer::Scheme s = base;   // keep the night sky and land; take the light from the album
    memcpy(s.low, a.mid, sizeof(s.low));
    memcpy(s.high, a.hot, sizeof(s.high));
    return s;
}

void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

} // namespace

AuroraVisualizer::AuroraVisualizer() : m_rng(std::random_device{}()) {
    m_from = m_to = kSchemes[m_scheme].s;   // start on the classic green and violet
    m_anim = std::uniform_real_distribution<float>(0.0f, 300.0f)(m_rng);
}

bool AuroraVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_aurora_ps, sizeof(g_aurora_ps), nullptr, &m_ps))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(AuroraConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
}

void AuroraVisualizer::ReleaseResources() {
    m_cb.Reset();
    m_ps.Reset();
}

AuroraVisualizer::Scheme AuroraVisualizer::CurrentScheme() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Scheme r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Scheme) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void AuroraVisualizer::NextScheme(float blendSeconds) {
    m_from = CurrentScheme();
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kSchemeCount - 2)(m_rng)) % kSchemeCount;
    m_to = m_hasArt && (m_scheme % 2 == 1) ? FromArt(m_art, kSchemes[m_scheme].s) : kSchemes[m_scheme].s;
    m_blend = 0.0f;
    m_blendSeconds = blendSeconds;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
}

void AuroraVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_mids = Smooth(m_mids, 0.5f * (audio.lowMid + audio.highMid), dt, 0.3f, 1.0f);
    m_bass = Smooth(m_bass, audio.bass, dt, 0.08f, 0.5f);
    m_treble = Smooth(m_treble, std::max(audio.treble * 0.6f, audio.hat), dt, 0.05f, 0.4f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.6f, 0.4f);

    // Beats send a flare racing along the curtains (predicted grid beats when locked).
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    bool beat = false;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; beat |= !gridLocked; }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; beat |= gridLocked; }
    if (beat && !audio.silent) {
        m_flare = std::max(m_flare, 0.6f + 0.6f * audio.bass);
        std::uniform_real_distribution<float> where(-14.0f, 14.0f);
        m_flareX = where(m_rng);
        m_flareSpeed = (m_flareX < 0.0f ? 1.0f : -1.0f) * (6.0f + 6.0f * audio.intensity);
        ++m_beatsSinceChange;
    }
    m_flare *= std::exp(-dt / 0.6f);
    m_flareX += m_flareSpeed * dt;

    // Drops: a surge of light and a new colour scheme.
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        m_surge = 1.0f;
        if (!m_locked && m_timeSinceChange > 4.0f) NextScheme(2.0f);
    }
    m_surge *= std::exp(-dt / 1.2f);
    if (m_beatsSinceChange >= 64 && m_timeSinceChange > 40.0f) m_changePending = true;   // slower than the others: calm
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 90.0f)) NextScheme(8.0f);
    m_blend = std::min(1.0f, m_blend + dt / m_blendSeconds);

    // Slow, smooth drivers: auroras move gracefully even when the music doesn't.
    const float speed = audio.silent ? 0.35f : 0.5f + 1.2f * m_mids + 0.8f * m_buildUp;
    m_anim += dt * speed * audio.tempoFactor;   // faster songs animate faster
    const float targetBright = audio.silent ? 0.45f : 0.55f + 0.5f * audio.intensity + 0.35f * m_bass + 0.4f * m_buildUp + 0.8f * m_surge + 0.35f * audio.vocal;
    m_brightness = Smooth(m_brightness, targetBright, dt, 0.25f, 1.2f);
    m_sway = Smooth(m_sway, 0.8f + 0.7f * m_mids + 0.3f * audio.pad, dt, 1.0f, 2.0f);
    m_height = Smooth(m_height, 1.0f + 0.5f * m_buildUp + 0.25f * m_surge, dt, 0.8f, 1.5f);
    m_ripple = Smooth(m_ripple, 0.25f + 0.9f * m_bass, dt, 0.1f, 0.8f);

    const Scheme s = CurrentScheme();
    AuroraConstants c{};
    c.animTime = m_anim;
    c.brightness = m_brightness;
    c.sway = m_sway;
    c.heightScale = m_height;
    c.flare = m_flare;
    c.flareX = m_flareX;
    c.stars = m_treble;
    c.ripple = m_ripple;
    c.yaw = 0.09f * std::sin(m_time * 0.021f);   // a slow look around
    c.pitch = 0.32f;
    Copy3(c.low, s.low);
    Copy3(c.high, s.high);
    Copy3(c.skyTop, s.skyTop);
    Copy3(c.skyHorizon, s.skyHorizon);
    Copy3(c.land, s.land);
    memcpy(m_constants, &c, sizeof(c));
}

void AuroraVisualizer::Render(Renderer& renderer, float, float) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(AuroraConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    renderer.DrawFullscreen(m_ps.Get());
}

bool AuroraVisualizer::OnKey(UINT vk) {
    if (vk == 'Q') {   // the drop's moment, by hand: the next update sees a "new" drop
        m_lastDrop = ~0u;
        return true;
    }
    if (vk == 'N') {
        NextScheme(2.5f);
        m_message = m_hasArt && (m_scheme % 2 == 1) ? L"Album colours" : kSchemes[m_scheme].name;
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Colours locked" : L"Colours unlocked";
        return true;
    }
    return false;
}

std::wstring AuroraVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring AuroraVisualizer::Status() const {
    return std::wstring(m_hasArt && (m_scheme % 2 == 1) ? L"Album colours" : kSchemes[m_scheme].name) +
           (m_locked ? L" (locked)" : L"");
}

void AuroraVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Scheme s = CurrentScheme();
    Copy3(lo, s.low);
    Copy3(hi, s.high);
}

void AuroraVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked && m_timeSinceChange > 10.0f) NextScheme(6.0f);
}
