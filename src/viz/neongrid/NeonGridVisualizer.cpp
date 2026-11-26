#include "viz/neongrid/NeonGridVisualizer.h"
#include "render/Renderer.h"
#include "g_neongrid_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr float kRowSpacing = 1.0f;       // one terrain row per world unit (= one across-line)
constexpr float kFarDist = 60.0f;         // rows enter this far ahead
constexpr double kTravelWrap = 1024.0;    // multiple of kRows * spacing: wrapping is seamless

struct NeonConstants {   // matches cbuffer Neon in neongrid_ps.hlsl
    float travel, camHeight, pitch, roll;
    float rowSpacing, farDist, maxHeight, tanHalfFov;
    float pulse, buildUp, glitch, glitchSeed;
    float sunRadius, roadLights, treble, horizonGlow;
    float skyTop[3];     float flash;
    float skyHorizon[3]; float sunStripes;
    float sunTop[3];     float pulseZ;
    float sunBottom[3];  float gateGlow;
    float gridA[3];      float bass;
    float gridB[3];      float gateSpacing;
    float ground[3];     float pad;
    float shockR, shockZ, surge, hit;   // sun shockwave radius; the light wall's distance; envelopes
};
static_assert(sizeof(NeonConstants) % 16 == 0);
static_assert(sizeof(NeonConstants) <= sizeof(float) * 48);

struct NamedScheme { const wchar_t* name; NeonGridVisualizer::Scheme s; };
const NamedScheme kSchemes[] = {
    // sky top, sky horizon, sun top, sun bottom, left neon, right neon, ground
    { L"Outrun",     { { 0.03f, 0.00f, 0.08f }, { 0.42f, 0.06f, 0.45f }, { 1.00f, 0.92f, 0.40f }, { 1.00f, 0.12f, 0.60f },
                       { 1.00f, 0.20f, 0.85f }, { 0.10f, 0.85f, 1.00f }, { 0.035f, 0.01f, 0.06f } } },
    { L"Cyber Teal", { { 0.00f, 0.02f, 0.06f }, { 0.02f, 0.25f, 0.40f }, { 0.70f, 1.00f, 0.95f }, { 0.10f, 0.50f, 1.00f },
                       { 0.00f, 1.00f, 0.85f }, { 1.00f, 0.25f, 0.70f }, { 0.00f, 0.03f, 0.05f } } },
    { L"Blade",      { { 0.04f, 0.01f, 0.00f }, { 0.50f, 0.15f, 0.04f }, { 1.00f, 0.85f, 0.40f }, { 1.00f, 0.25f, 0.10f },
                       { 1.00f, 0.45f, 0.10f }, { 0.15f, 0.80f, 1.00f }, { 0.05f, 0.02f, 0.01f } } },
    { L"Matrix",     { { 0.00f, 0.02f, 0.00f }, { 0.02f, 0.20f, 0.07f }, { 0.80f, 1.00f, 0.60f }, { 0.10f, 0.70f, 0.25f },
                       { 0.15f, 1.00f, 0.35f }, { 0.60f, 1.00f, 0.85f }, { 0.00f, 0.035f, 0.01f } } },
    { L"Midnight",   { { 0.00f, 0.00f, 0.06f }, { 0.12f, 0.15f, 0.55f }, { 1.00f, 0.95f, 0.90f }, { 0.55f, 0.40f, 1.00f },
                       { 0.60f, 0.40f, 1.00f }, { 0.30f, 0.85f, 1.00f }, { 0.01f, 0.01f, 0.06f } } },
};
constexpr int kSchemeCount = static_cast<int>(std::size(kSchemes));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

NeonGridVisualizer::Scheme FromArt(const TrackPalette& a) {
    NeonGridVisualizer::Scheme s{};
    for (int c = 0; c < 3; ++c) {
        s.skyTop[c] = a.deep[c] * 0.25f;
        s.skyHorizon[c] = std::min(1.0f, a.deep[c] * 2.5f + a.mid[c] * 0.25f);
        s.sunTop[c] = std::min(1.0f, a.hot[c] * 0.6f + 0.4f);
        s.sunBottom[c] = a.mid[c];
        s.gridA[c] = a.mid[c];
        s.gridB[c] = a.hot[c];
        s.ground[c] = a.deep[c] * 0.35f;
    }
    return s;
}

void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

} // namespace

NeonGridVisualizer::NeonGridVisualizer() : m_rng(std::random_device{}()) {
    m_scheme = std::uniform_int_distribution<int>(0, kSchemeCount - 1)(m_rng);
    m_from = m_to = kSchemes[m_scheme].s;
}

bool NeonGridVisualizer::CreateResources(ID3D11Device* device) {
    device->GetImmediateContext(&m_context);
    if (FAILED(device->CreatePixelShader(g_neongrid_ps, sizeof(g_neongrid_ps), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(NeonConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;   // bands
    s.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;    // rows form a ring
    s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_sampler))) return false;

    // Terrain history: 64 bands x 128 rows; one row is written per unit of distance flown.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kBands;
    td.Height = kRows;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<float> zeros(kBands * kRows, 0.0f);
    D3D11_SUBRESOURCE_DATA init{ zeros.data(), kBands * sizeof(float), 0 };
    return SUCCEEDED(device->CreateTexture2D(&td, &init, &m_historyTex)) &&
           SUCCEEDED(device->CreateShaderResourceView(m_historyTex.Get(), nullptr, &m_history));
}

void NeonGridVisualizer::ReleaseResources() {
    m_history.Reset();
    m_historyTex.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_ps.Reset();
    m_context.Reset();
}

NeonGridVisualizer::Scheme NeonGridVisualizer::CurrentScheme() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Scheme r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Scheme) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void NeonGridVisualizer::NextScheme(float blendSeconds) {
    m_from = CurrentScheme();
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kSchemeCount - 2)(m_rng)) % kSchemeCount;
    m_to = m_hasArt && (m_scheme & 1) == 0 ? FromArt(m_art) : kSchemes[m_scheme].s;   // album art in the rotation
    m_blend = 0.0f;
    m_blendSeconds = blendSeconds;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
}

void NeonGridVisualizer::PushRow() {
    // Light smoothing across bands so ridges read as mountains rather than spikes.
    float row[kBands];
    for (int b = 0; b < kBands; ++b) {
        const float l = m_rowPeak[std::max(b - 1, 0)], r = m_rowPeak[std::min(b + 1, kBands - 1)];
        row[b] = 0.25f * l + 0.5f * m_rowPeak[b] + 0.25f * r;
        row[b] = 0.6f * row[b] + 0.4f * m_lastRow[b];   // and a little along time
        m_lastRow[b] = row[b];
        m_rowPeak[b] = 0.0f;
    }
    if (m_context && m_historyTex) {
        D3D11_BOX box{ 0, static_cast<UINT>(m_rowIndex % kRows), 0, kBands, static_cast<UINT>(m_rowIndex % kRows) + 1, 1 };
        m_context->UpdateSubresource(m_historyTex.Get(), 0, &box, row, kBands * sizeof(float), 0);
    }
    m_rowIndex = (m_rowIndex + 1) % static_cast<int>(kTravelWrap);
}

void NeonGridVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    for (int b = 0; b < kBands; ++b) m_rowPeak[b] = std::max(m_rowPeak[b], audio.spectrum[b]);

    // --- Beats: predicted grid when locked, heard onsets otherwise ---
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    float target = 0.0f;
    if (gridLocked) target = std::pow(1.0f - audio.beatPhase, 4.0f);
    if (audio.beatCount != m_lastBeat) {
        m_lastBeat = audio.beatCount;
        if (!gridLocked) { m_pulse = 1.0f; m_pulseZ = 12.0f; ++m_beatsSinceChange; }
        // Kicks in big sections flare the neon (light only: no camera move).
        const float big = std::clamp((audio.intensity - 0.45f) / 0.3f, 0.0f, 1.0f);
        m_hit = std::max(m_hit, big * std::clamp(0.3f + 0.9f * audio.bass, 0.0f, 1.0f));
    }
    m_hit *= std::exp(-dt / 0.18f);
    m_hitEnv = Smooth(m_hitEnv, m_hit, dt, 0.03f, 0.08f);
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; if (gridLocked) { m_pulseZ = 12.0f; ++m_beatsSinceChange; } }
    m_pulse = gridLocked ? target * audio.tempoConfidence : m_pulse * std::exp(-dt / 0.25f);
    m_bob = Smooth(m_bob, m_pulse, dt, 0.12f, 0.5f);   // eased: never a jolt

    // --- Structure ---
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        m_glitch = 1.0f;
        m_flash = 1.0f;
        m_surge = 1.0f;
        m_sinceSurge = 0.0f;
        m_glitchSeed = std::fmod(m_glitchSeed + 0.618034f, 1.0f);
        if (!m_locked && m_timeSinceChange > 3.0f) NextScheme(0.8f);
    }
    m_glitch *= std::exp(-dt / 0.3f);
    // The surge holds for a moment, then eases off over a couple of seconds; its envelope rises fast
    // but not in a single frame, so the view swells open rather than jumping.
    m_sinceSurge += dt;
    if (m_sinceSurge > 0.6f) m_surge *= std::exp(-dt / 1.4f);
    m_surgeEnv = Smooth(m_surgeEnv, m_surge, dt, 0.12f, 0.25f);
    m_flash *= std::exp(-dt / 0.4f);
    if (m_beatsSinceChange >= 32 && m_timeSinceChange > 20.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 60.0f)) NextScheme(4.0f);
    m_blend = std::min(1.0f, m_blend + dt / m_blendSeconds);

    m_bass = Smooth(m_bass, audio.bass, dt, 0.05f, 0.3f);
    m_treble = Smooth(m_treble, std::max(audio.treble * 0.6f, audio.hat), dt, 0.02f, 0.2f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.5f, 0.3f);
    m_drums = Smooth(m_drums, std::clamp((audio.percussive - 0.3f) / 0.12f, 0.0f, 1.0f), dt, 0.5f, 2.0f);

    // --- Flight: speed from intensity and build-ups; calm drift in silence ---
    const float targetSpeed = audio.silent ? 2.5f : 4.0f + 9.0f * audio.intensity + 8.0f * m_buildUp;
    const float tf = audio.tempoFactor;   // faster songs animate faster
    m_speed = Smooth(m_speed, targetSpeed * tf, dt, 1.0f, 2.0f);
    const float speed = m_speed * (1.0f + 2.2f * m_surgeEnv);   // the surge's speed burst
    m_travel += double(speed) * dt;
    m_pulseZ -= speed * dt * 1.5f;   // the beat ring rushes towards the camera
    while (m_travel >= m_nextRowAt) {
        PushRow();
        m_nextRowAt += kRowSpacing;
    }
    if (m_travel >= kTravelWrap) { m_travel -= kTravelWrap; m_nextRowAt -= kTravelWrap; }

    // --- Constants for this frame ---
    const Scheme s = CurrentScheme();
    NeonConstants c{};
    c.travel = static_cast<float>(m_travel);
    c.camHeight = 1.15f + 0.06f * m_bob;
    c.pitch = -0.07f;
    c.roll = 0.025f * std::sin(m_time * 0.27f);
    c.rowSpacing = kRowSpacing;
    c.farDist = kFarDist;
    c.maxHeight = 5.0f * (1.0f + 0.35f * m_surgeEnv);
    c.tanHalfFov = std::tan((32.0f + 12.0f * m_surgeEnv) * 3.14159265f / 180.0f);   // the view widens into the burst
    c.pulse = m_pulse;
    c.buildUp = m_buildUp;
    c.glitch = m_glitch;
    c.glitchSeed = m_glitchSeed;
    c.sunRadius = 0.34f + 0.03f * m_bass + 0.05f * m_surgeEnv;
    c.gateGlow = 0.85f + 0.8f * m_buildUp + 1.4f * m_surgeEnv + 0.7f * m_hitEnv;
    c.bass = m_bass;
    c.gateSpacing = 10.0f;
    c.roadLights = m_drums * (0.4f + 0.6f * m_pulse);
    c.treble = m_treble;
    c.horizonGlow = 0.5f + 0.9f * m_buildUp + 0.3f * m_bass + 1.6f * m_surgeEnv + 0.3f * m_hitEnv;
    c.flash = m_flash;
    c.sunStripes = 9.0f;
    c.pulseZ = m_pulseZ;
    c.shockR = m_sinceSurge * 1.6f;                            // screen heights per second
    c.shockZ = kFarDist * 1.4f - m_sinceSurge * 75.0f;          // from beyond the horizon to past the camera
    c.surge = m_surgeEnv;
    c.hit = m_hitEnv;
    Copy3(c.skyTop, s.skyTop);
    Copy3(c.skyHorizon, s.skyHorizon);
    Copy3(c.sunTop, s.sunTop);
    Copy3(c.sunBottom, s.sunBottom);
    Copy3(c.gridA, s.gridA);
    Copy3(c.gridB, s.gridB);
    Copy3(c.ground, s.ground);
    memcpy(m_constants, &c, sizeof(c));
}

void NeonGridVisualizer::Render(Renderer& renderer, float, float) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(NeonConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ID3D11ShaderResourceView* srv = m_history.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_ps.Get());
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

bool NeonGridVisualizer::OnKey(UINT vk) {
    if (vk == 'Q') {   // the drop's moment, by hand: the next update sees a "new" drop
        m_lastDrop = ~0u;
        return true;
    }
    if (vk == 'N') {
        NextScheme(1.2f);
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Colours locked" : L"Colours unlocked";
        return true;
    }
    return false;
}

std::wstring NeonGridVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring NeonGridVisualizer::Status() const {
    return std::wstring(m_hasArt && (m_scheme & 1) == 0 ? L"Album colours" : kSchemes[m_scheme].name) +
           (m_locked ? L" (locked)" : L"");
}

void NeonGridVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Scheme s = CurrentScheme();
    Copy3(lo, s.gridB);
    Copy3(hi, s.gridA);
}

void NeonGridVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) NextScheme(2.0f);
}
