#include "viz/nebula/NebulaVisualizer.h"
#include "render/Renderer.h"
#include "g_nebula_ps.h"
#include "g_nebula_taa_ps.h"
#include "g_nebula_post_ps.h"
#include "g_noise3d_cs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kNoiseSize = 64;
constexpr double kWorldPeriod = 64.0;   // noise tiles every 64 world units
constexpr float kLookBlendSeconds = 6.0f;

struct NebulaConstants {   // matches cbuffer Nebula in nebula_ps.hlsl
    float camPos[3];     float tanHalfFov;
    float camRight[3];   float exposure;
    float camUp[3];      float starBoost;
    float camFwd[3];     float sparkle;
    float lightPos[3];   float lightStrength;
    float lightColor[3]; float shellRadius;
    float colDeep[3];    float shellStrength;
    float colMid[3];     float absorb;
    float colHot[3];     float ambient;
    float threshold, gain, warp, filament;
    float warpOffset[3]; float detail;
    float jitter[2];     float frameIndex; float scatter;
    float pulsarPos[3];  float pulsarStrength;
    float pulsarDir[3];  float pulsarWidth;
    float pulsarCol[3];  float snFlash;
    float snPos[3];      float snShellRadius;
    float snCol[3];      float snShellStrength;
};
static_assert(sizeof(NebulaConstants) % 16 == 0);

struct TaaConstants {   // matches cbuffer Taa in nebula_taa_ps.hlsl
    float prevRight[3]; float prevTanHalfFov;
    float prevUp[3];    float blend;
    float prevFwd[3];   float historyValid;
    float camDelta[3];  float pad;
};
static_assert(sizeof(TaaConstants) % 16 == 0);

struct PostConstants {   // matches cbuffer Post in nebula_post_ps.hlsl
    float snScreen[2]; float snFlare; float bloom;
    float flareCol[3]; float flash;
};
static_assert(sizeof(PostConstants) % 16 == 0);

float Rand(std::mt19937& rng, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
void Normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Weight of each new frame in the accumulated image: lower = smoother but slower to react
// (the neighbourhood clamp keeps beats and look changes responsive either way).
constexpr float kTaaBlend = 0.15f;

struct Palette {
    const wchar_t* name;
    float deep[3], mid[3], hot[3];   // sRGB, converted to linear when used
};
const Palette kPalettes[] = {
    { L"Orion",       { 0.05f, 0.03f, 0.12f }, { 0.60f, 0.18f, 0.55f }, { 1.00f, 0.60f, 0.45f } },
    { L"Teal Drift",  { 0.01f, 0.05f, 0.10f }, { 0.04f, 0.50f, 0.62f }, { 0.60f, 0.95f, 1.00f } },
    { L"Ember",       { 0.08f, 0.02f, 0.02f }, { 0.85f, 0.28f, 0.08f }, { 1.00f, 0.85f, 0.50f } },
    { L"Ice & Gold",  { 0.02f, 0.04f, 0.12f }, { 0.20f, 0.40f, 0.95f }, { 1.00f, 0.78f, 0.35f } },
    { L"Emerald",     { 0.00f, 0.05f, 0.05f }, { 0.06f, 0.58f, 0.38f }, { 0.70f, 1.00f, 0.40f } },
    { L"Hubble",      { 0.04f, 0.03f, 0.09f }, { 0.90f, 0.45f, 0.10f }, { 0.25f, 0.70f, 1.00f } },
    { L"Violet Rose", { 0.06f, 0.01f, 0.10f }, { 0.55f, 0.15f, 0.85f }, { 1.00f, 0.55f, 0.75f } },
};
constexpr int kPaletteCount = static_cast<int>(std::size(kPalettes));

struct Shape {
    const wchar_t* name;
    float threshold, gain, warp, filament, absorb, detail;
};
const Shape kShapes[] = {
    { L"Clouds",    0.53f, 6.0f, 2.5f, 0.10f, 1.4f, 0.35f },
    { L"Filaments", 0.56f, 7.0f, 4.0f, 0.65f, 1.2f, 0.40f },
    { L"Wisps",     0.57f, 8.0f, 6.0f, 0.30f, 1.0f, 0.50f },
    { L"Pillars",   0.50f, 5.0f, 1.8f, 0.25f, 1.8f, 0.30f },
    { L"Bubbles",   0.60f, 8.0f, 3.0f, 0.90f, 1.2f, 0.35f },
};
constexpr int kShapeCount = static_cast<int>(std::size(kShapes));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

float SrgbToLinear(float c) { return std::pow(c, 2.2f); }

NebulaVisualizer::Look MakeLook(int palette, int shape) {
    NebulaVisualizer::Look l{};
    const Palette& p = kPalettes[palette];
    for (int c = 0; c < 3; ++c) {
        l.deep[c] = SrgbToLinear(p.deep[c]);
        l.mid[c] = SrgbToLinear(p.mid[c]);
        l.hot[c] = SrgbToLinear(p.hot[c]);
    }
    const Shape& s = kShapes[shape];
    l.threshold = s.threshold;
    l.gain = s.gain;
    l.warp = s.warp;
    l.filament = s.filament;
    l.absorb = s.absorb;
    l.detail = s.detail;
    return l;
}

NebulaVisualizer::Look Blend(const NebulaVisualizer::Look& a, const NebulaVisualizer::Look& b, float t) {
    // Treat the struct as an array of floats: every field blends linearly.
    static_assert(sizeof(NebulaVisualizer::Look) % sizeof(float) == 0);
    NebulaVisualizer::Look r;
    const float* pa = reinterpret_cast<const float*>(&a);
    const float* pb = reinterpret_cast<const float*>(&b);
    float* pr = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(r) / sizeof(float); ++i) pr[i] = pa[i] + (pb[i] - pa[i]) * t;
    return r;
}

void Copy3(float* dst, const float* src) { memcpy(dst, src, 3 * sizeof(float)); }

} // namespace

NebulaVisualizer::NebulaVisualizer() : m_rng(std::random_device{}()) {
    // Developer switch for before/after comparisons: VISUALIZER_TAA=0 disables accumulation.
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"VISUALIZER_TAA", env, ARRAYSIZE(env)) > 0 && env[0] == L'0') m_taaEnabled = false;
    // ...and VISUALIZER_SEED=<n> makes the look and flight path repeatable between runs.
    if (GetEnvironmentVariableW(L"VISUALIZER_SEED", env, ARRAYSIZE(env)) > 0) m_rng.seed(_wtoi(env));
    m_paletteIndex = std::uniform_int_distribution<int>(0, kPaletteCount - 1)(m_rng);
    m_shapeIndex = std::uniform_int_distribution<int>(0, kShapeCount - 1)(m_rng);
    m_from = m_to = MakeLook(m_paletteIndex, m_shapeIndex);
    m_time = std::uniform_real_distribution<float>(0.0f, 500.0f)(m_rng);   // different flight path each run
}

bool NebulaVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_nebula_ps, sizeof(g_nebula_ps), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(NebulaConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_sampler))) return false;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&s, &m_clamp))) return false;

    // Temporal accumulation resolve.
    m_device = device;
    if (FAILED(device->CreatePixelShader(g_nebula_taa_ps, sizeof(g_nebula_taa_ps), nullptr, &m_taaPS))) return false;
    cb.ByteWidth = sizeof(TaaConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_taaCB))) return false;
    if (FAILED(device->CreatePixelShader(g_nebula_post_ps, sizeof(g_nebula_post_ps), nullptr, &m_postPS))) return false;
    cb.ByteWidth = sizeof(PostConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_postCB))) return false;
    m_historyValid = false;

    // Noise volume: generated once by a compute shader (~1 MB, a few ms of GPU time).
    D3D11_TEXTURE3D_DESC td{};
    td.Width = td.Height = td.Depth = kNoiseSize;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(device->CreateTexture3D(&td, nullptr, &tex)) ||
        FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &m_noise)) ||
        FAILED(device->CreateUnorderedAccessView(tex.Get(), nullptr, &uav)) ||
        FAILED(device->CreateComputeShader(g_noise3d_cs, sizeof(g_noise3d_cs), nullptr, &cs)))
        return false;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ID3D11UnorderedAccessView* uavs[] = { uav.Get() };
    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(kNoiseSize / 4, kNoiseSize / 4, kNoiseSize / 4);
    ID3D11UnorderedAccessView* none[] = { nullptr };
    ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    return true;
}

void NebulaVisualizer::ReleaseResources() {
    m_current.Reset();
    m_resolved.Reset();
    m_postCB.Reset();
    m_postPS.Reset();
    m_history[0].Reset();
    m_history[1].Reset();
    m_taaCB.Reset();
    m_taaPS.Reset();
    m_clamp.Reset();
    m_noise.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_ps.Reset();
    m_device.Reset();
}

void NebulaVisualizer::Activate() {
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_historyValid = false;   // don't reproject a stale image from when it was last visible
}

NebulaVisualizer::Look NebulaVisualizer::CurrentLook() const {
    const float t = m_lookBlend * m_lookBlend * (3.0f - 2.0f * m_lookBlend);   // smoothstep
    return Blend(m_from, m_to, t);
}

void NebulaVisualizer::NextLook(float blendSeconds) {
    m_from = CurrentLook();   // start from exactly what's on screen, even mid-blend
    const int newPalette = (m_paletteIndex + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    const int newShape = (m_shapeIndex + 1 + std::uniform_int_distribution<int>(0, kShapeCount - 2)(m_rng)) % kShapeCount;
    m_paletteIndex = newPalette;
    m_shapeIndex = newShape;
    m_to = MakeLook(m_paletteIndex, m_shapeIndex);
    if (m_hasArt) {
        // Album colours: keep the song's palette, vary which colour leads so looks still change.
        const float* mid = (m_artVariant & 1) ? m_art.hot : m_art.mid;
        const float* hot = (m_artVariant & 1) ? m_art.mid : m_art.hot;
        for (int c = 0; c < 3; ++c) {
            m_to.deep[c] = SrgbToLinear(m_art.deep[c]);
            m_to.mid[c] = SrgbToLinear(mid[c]);
            m_to.hot[c] = SrgbToLinear(hot[c]);
        }
        ++m_artVariant;
    }
    m_lookBlend = 0.0f;
    m_lookBlendSeconds = blendSeconds;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_gridBeatsSinceChange = 0;
    m_changePending = false;
}

void NebulaVisualizer::Relative(const double world[3], float out[3]) const {
    for (int i = 0; i < 3; ++i) {
        double d = world[i] - m_pos[i];
        d -= kWorldPeriod * std::floor(d / kWorldPeriod + 0.5);
        out[i] = static_cast<float>(d);
    }
}

// A pulsar appears well ahead and off to one side, so the flight sweeps past it.
void NebulaVisualizer::SpawnPulsar() {
    const float* right = m_basis[0];
    const float* up = m_basis[1];
    const float* fwd = m_basis[2];
    const float side = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f;
    const float dist = Rand(m_rng, 20.0f, 26.0f), lateral = side * Rand(m_rng, 3.5f, 6.5f), lift = Rand(m_rng, -2.5f, 2.5f);
    for (int i = 0; i < 3; ++i) m_pulsar.pos[i] = m_pos[i] + fwd[i] * dist + right[i] * lateral + up[i] * lift;
    // Spin axis: tilted, so the beams sweep across the view rather than straight at the camera.
    float axis[3] = { Rand(m_rng, -1, 1), Rand(m_rng, -1, 1), Rand(m_rng, -1, 1) };
    Normalize3(axis);
    float u[3] = { axis[1] * fwd[2] - axis[2] * fwd[1], axis[2] * fwd[0] - axis[0] * fwd[2], axis[0] * fwd[1] - axis[1] * fwd[0] };
    Normalize3(u);
    const float v[3] = { axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0] };
    memcpy(m_pulsar.axisU, u, sizeof(u));
    memcpy(m_pulsar.axisV, v, sizeof(v));
    m_pulsar.active = true;
    m_pulsar.age = 0.0f;
}

void NebulaVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_silent = audio.silent;

    // --- Beats -------------------------------------------------------------
    if (audio.beatCount != m_lastBeatCount) {
        m_lastBeatCount = audio.beatCount;
        m_beatPulse = 1.0f;
        m_shellAge = 0.0f;
        m_shellPower = 0.5f + audio.bass;   // harder kicks, stronger shockwave
        ++m_beatsSinceChange;
    }
    m_beatPulse *= std::exp(-dt / 0.35f);
    m_shellAge += dt;

    // --- Song structure: drops and phrases ------------------------------------
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    m_gridPulse = gridLocked ? std::pow(1.0f - audio.beatPhase, 4.0f) * audio.tempoConfidence : 0.0f;
    if (gridLocked && audio.gridBeat != m_lastGridBeat) { ++m_gridBeatsSinceChange; ++m_gridBeatsSinceTurn; }
    m_lastGridBeat = audio.gridBeat;

    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDropCount) {
        m_lastDropCount = audio.dropCount;
        // The drop: a supernova ahead, a rush through its blast, and a fast switch to a new look.
        m_shellAge = 0.0f;
        m_shellPower = 1.0f;
        m_dropFlash = 1.0f;
        m_hyper = 1.0f;
        const float* fwd = m_basis[2];
        const float* right = m_basis[0];
        const float* up = m_basis[1];
        const float lat = Rand(m_rng, -2.0f, 2.0f), lift = Rand(m_rng, -1.2f, 1.2f);
        for (int i = 0; i < 3; ++i) m_nova.pos[i] = m_pos[i] + fwd[i] * 13.0f + right[i] * lat + up[i] * lift;
        m_nova.active = true;
        m_nova.age = 0.0f;
        if (!m_locked && m_timeSinceChange > 4.0f) NextLook(1.5f);
    }
    m_dropFlash *= std::exp(-dt / 0.8f);
    m_hyper *= std::exp(-dt / 1.6f);
    if (m_nova.active && (m_nova.age += dt) > 14.0f) m_nova.active = false;

    // Phrases: every 8 bars when the grid is locked (changing on the downbeat),
    // otherwise every 32 heard beats; and at least once a minute.
    const bool phraseDone = gridLocked ? m_gridBeatsSinceChange >= 32 : m_beatsSinceChange >= 32;
    if (phraseDone && m_timeSinceChange > 20.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;   // bar phase wrapped
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 60.0f)) {
        NextLook(kLookBlendSeconds);
        if (!m_pulsar.active && !audio.silent && std::uniform_int_distribution<int>(0, 1)(m_rng) == 0) SpawnPulsar();
    }

    // --- Events -------------------------------------------------------------------
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);
    // Pulsar: fades in, spins (two turns per bar when locked, so a beam crosses each beat),
    // and fades out once the flight has passed it.
    if (m_pulsar.active) {
        m_pulsar.age += dt;
        float rel[3];
        Relative(m_pulsar.pos, rel);
        const float ahead = rel[0] * m_basis[2][0] + rel[1] * m_basis[2][1] + rel[2] * m_basis[2][2];
        const bool leaving = ahead < 3.0f || m_pulsar.age > 50.0f;
        m_pulsar.fade = std::clamp(m_pulsar.fade + dt * (leaving ? -0.6f : 0.5f), 0.0f, 1.0f);
        if (leaving && m_pulsar.fade <= 0.0f) m_pulsar.active = false;
        if (gridLocked) m_pulsar.spin = 6.2831853f * 2.0f * audio.barPhase;
        else m_pulsar.spin += dt * 6.2831853f * 0.7f;
    }
    m_lookBlend = std::min(1.0f, m_lookBlend + dt / m_lookBlendSeconds);

    // --- Smoothed audio drivers ----------------------------------------------
    m_bass = Smooth(m_bass, audio.bass, dt, 0.03f, 0.25f);
    m_mids = Smooth(m_mids, 0.5f * (audio.lowMid + audio.highMid), dt, 0.1f, 0.6f);
    m_treble = Smooth(m_treble, audio.treble, dt, 0.02f, 0.2f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.5f, 0.3f);

    // Build-ups accelerate the flight; drops hit hard then settle.
    const float targetSpeed = audio.silent ? 0.25f
        : 0.35f + 1.1f * audio.intensity + 0.5f * audio.bass + 0.25f * m_beatPulse + 1.2f * m_buildUp;
    m_speed = Smooth(m_speed, targetSpeed, dt, 0.5f, 2.0f);
    const float flySpeed = m_speed + 3.5f * m_hyper;

    // Beat zoom: small and eased in (never a one-frame jump), which is what avoids motion sickness.
    m_zoom = Smooth(m_zoom, std::max(m_beatPulse, m_gridPulse), dt, 0.15f, 0.6f);

    const float targetExposure = audio.silent ? 0.8f
        : 0.7f + 0.3f * audio.level + 0.2f * m_beatPulse + 0.15f * m_gridPulse + 0.25f * m_buildUp + 0.5f * m_dropFlash;
    m_exposure = Smooth(m_exposure, targetExposure, dt, 0.08f, 0.8f);

    // With a locked grid, the core's heartbeat follows the predicted beat (no lag).
    const float pulse = gridLocked ? std::max(m_gridPulse, 0.5f * m_beatPulse) : m_beatPulse;
    const float targetLight = audio.silent ? 0.6f : 0.6f + 0.7f * m_bass + 0.9f * pulse + 0.6f * m_buildUp;
    m_lightStrength = Smooth(m_lightStrength, targetLight, dt, 0.03f, 0.35f);

    m_swirl += dt * (0.003f + 0.02f * m_mids + 0.03f * m_buildUp);   // tension: the gas churns faster
    m_rollDrift += dt * (0.04f * m_mids + 0.05f * m_buildUp);

    // --- Camera: directed flight. Every 8 bars (or ~16 s) the flight swoops to a new heading,
    // easing into the turn and banking with it; a gentle wander stops it feeling on rails.
    m_timeSinceTurn += dt;
    const bool turnDue = gridLocked ? (onDownbeat && m_gridBeatsSinceTurn >= 32) : m_timeSinceTurn > 16.0f;
    if (turnDue && !audio.silent) {
        m_yawTarget += Rand(m_rng, 0.5f, 1.3f) * (std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f);
        m_pitchTarget = Rand(m_rng, -0.35f, 0.35f);
        m_gridBeatsSinceTurn = 0;
        m_timeSinceTurn = 0.0f;
    }
    const float prevYaw = m_yaw;
    m_yaw = Smooth(m_yaw, m_yawTarget, dt, 2.2f, 2.2f);
    m_pitch = Smooth(m_pitch, m_pitchTarget, dt, 2.5f, 2.5f);
    const float yawRate = (m_yaw - prevYaw) / std::max(dt, 1e-4f);
    m_bank = Smooth(m_bank, std::clamp(-yawRate * 0.9f, -0.5f, 0.5f), dt, 0.5f, 0.8f);
    const float t = m_time;
    const float yaw = m_yaw + 0.25f * std::sin(t * 0.047f + 1.3f);
    const float pitch = m_pitch + 0.12f * std::sin(t * 0.031f + 2.1f);
    const float roll = 0.15f * std::sin(t * 0.017f) + m_rollDrift + m_bank;
    const float fwd[3] = { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
    // right = normalize(cross(worldUp, fwd)); up = cross(fwd, right)
    float right[3] = { fwd[2], 0.0f, -fwd[0] };
    const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    right[0] /= rl; right[2] /= rl;
    const float up[3] = { fwd[1] * right[2] - fwd[2] * right[1],
                          fwd[2] * right[0] - fwd[0] * right[2],
                          fwd[0] * right[1] - fwd[1] * right[0] };
    const float cr = std::cos(roll), sr = std::sin(roll);
    for (int c = 0; c < 3; ++c) {
        m_basis[0][c] = right[c] * cr + up[c] * sr;
        m_basis[1][c] = up[c] * cr - right[c] * sr;
        m_basis[2][c] = fwd[c];
    }
    for (int c = 0; c < 3; ++c) {
        m_pos[c] += double(fwd[c]) * flySpeed * dt;
        m_pos[c] = std::fmod(m_pos[c], kWorldPeriod);
        if (m_pos[c] < 0) m_pos[c] += kWorldPeriod;
    }
}

void NebulaVisualizer::Render(Renderer& renderer, float time, float dt) {
    const Look look = CurrentLook();
    const float* right = m_basis[0];
    const float* up = m_basis[1];
    const float* fwd = m_basis[2];

    NebulaConstants c{};
    for (int i = 0; i < 3; ++i) {
        c.camPos[i] = static_cast<float>(m_pos[i]);
        c.camRight[i] = right[i];
        c.camUp[i] = up[i];
        c.camFwd[i] = fwd[i];
        // The light core floats ahead of the camera, drifting around the centre of view.
        // Build-ups pull it closer and into the centre: the flight dives towards it.
        const float centre = 1.0f - 0.7f * m_buildUp;
        c.lightPos[i] = c.camPos[i] + fwd[i] * (9.0f - 3.5f * m_buildUp)
                      + right[i] * 2.5f * centre * std::sin(m_time * 0.13f)
                      + up[i] * 1.5f * centre * std::sin(m_time * 0.17f + 1.0f);
        c.lightColor[i] = 0.6f * look.hot[i] + 0.4f * look.mid[i];
    }
    c.tanHalfFov = std::tan((38.0f - 0.8f * m_zoom + 6.0f * m_hyper + 3.0f * m_buildUp) * kPi / 180.0f);   // beat: gentle zoom swell; rushes widen the view
    c.exposure = m_exposure * 2.4f;
    c.starBoost = (m_silent ? 0.9f : 0.9f + 1.6f * m_treble);
    c.sparkle = m_treble * 1.5f;
    c.lightStrength = m_lightStrength;
    c.shellRadius = m_shellAge * 10.0f;
    c.shellStrength = std::max(0.0f, 1.0f - m_shellAge / 1.4f) * 0.6f * m_shellPower;
    Copy3(c.colDeep, look.deep);
    Copy3(c.colMid, look.mid);
    Copy3(c.colHot, look.hot);
    c.absorb = look.absorb;
    c.ambient = 0.3f;
    c.threshold = look.threshold - 0.035f * m_bass + 0.04f * m_buildUp;   // build-up thins the gas: tension
    c.gain = look.gain * 1.15f * (1.0f + 0.25f * m_bass);
    c.warp = look.warp * (1.0f + 0.7f * m_mids);
    c.filament = look.filament;
    c.warpOffset[0] = m_swirl;
    c.warpOffset[1] = m_swirl * 0.7f;
    c.warpOffset[2] = m_swirl * 0.4f;
    c.detail = look.detail;
    c.scatter = 1.0f + 1.5f * m_buildUp + 0.8f * m_bass;

    // Events
    const float hotWhite[3] = { 0.85f, 0.92f, 1.0f };
    if (m_pulsar.active && m_pulsar.fade > 0.0f) {
        float rel[3];
        Relative(m_pulsar.pos, rel);
        const float cs = std::cos(m_pulsar.spin), sn = std::sin(m_pulsar.spin);
        for (int i = 0; i < 3; ++i) {
            c.pulsarPos[i] = c.camPos[i] + rel[i];
            c.pulsarDir[i] = m_pulsar.axisU[i] * cs + m_pulsar.axisV[i] * sn;
            c.pulsarCol[i] = 0.6f * hotWhite[i] + 0.4f * look.hot[i];
        }
        const float f = m_pulsar.fade * m_pulsar.fade * (3.0f - 2.0f * m_pulsar.fade);
        c.pulsarStrength = f * (0.8f + 0.5f * m_gridPulse);
        c.pulsarWidth = 0.22f;
    } else {
        c.pulsarPos[0] = c.camPos[0] + 1e4f;   // far away: contributes nothing
    }
    float novaFlare = 0.0f;
    float novaScreen[2] = { -10.0f, -10.0f };
    if (m_nova.active) {
        float rel[3];
        Relative(m_nova.pos, rel);
        const float a = m_nova.age;
        const float flash = std::exp(-a / 0.45f);
        const float remnant = 0.18f * std::exp(-a / 5.0f) * std::min(1.0f, a * 4.0f);
        c.snFlash = flash * 1.2f + remnant;
        c.snShellRadius = a * 9.0f;
        c.snShellStrength = 1.6f * std::max(0.0f, 1.0f - a / 3.0f);
        const float shift = std::min(1.0f, a / 2.5f);   // white-hot, then the look's colours
        for (int i = 0; i < 3; ++i) {
            c.snPos[i] = c.camPos[i] + rel[i];
            c.snCol[i] = hotWhite[i] * (1.0f - shift) + (0.5f * look.hot[i] + 0.5f * look.mid[i]) * shift;
        }
        // Where it is on screen, for the lens flare.
        const float z = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2];
        if (z > 0.5f) {
            const int bw = renderer.BoundWidth(), bh = renderer.BoundHeight();
            const float aspect = static_cast<float>(bw) / std::max(bh, 1);
            const float sx = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / (z * c.tanHalfFov);
            const float sy = (rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) / (z * c.tanHalfFov);
            novaScreen[0] = (sx / aspect + 1.0f) * 0.5f;
            novaScreen[1] = (1.0f - sy) * 0.5f;
            novaFlare = flash * 1.0f + remnant * 1.5f;
        }
    } else {
        c.snPos[0] = c.camPos[0] + 1e4f;
    }

    // --- Temporal accumulation setup ---
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    if (m_current.width != w || m_current.height != h) {
        // 16-bit float: colour plus the gas distance (alpha) needed for reprojection.
        m_current.Create(m_device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_history[0].Create(m_device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_history[1].Create(m_device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_resolved.Create(m_device.Get(), w, h, DXGI_FORMAT_R11G11B10_FLOAT, true);
        m_historyValid = false;
    }
    if (!m_current.rtv || !m_history[0].rtv || !m_history[1].rtv || !m_resolved.rtv) return;

    // Halton(2,3) sub-pixel jitter: 8 well-spread positions per cycle.
    static constexpr float kHalton[8][2] = { { 0.500f, 0.333f }, { 0.250f, 0.667f }, { 0.750f, 0.111f }, { 0.125f, 0.444f },
                                             { 0.625f, 0.778f }, { 0.375f, 0.222f }, { 0.875f, 0.556f }, { 0.0625f, 0.889f } };
    ++m_frame;
    c.jitter[0] = kHalton[m_frame % 8][0] - 0.5f;
    c.jitter[1] = kHalton[m_frame % 8][1] - 0.5f;
    c.frameIndex = static_cast<float>(m_frame % 1024);

    TaaConstants taa{};
    for (int i = 0; i < 3; ++i) {
        taa.prevRight[i] = m_prevBasis[0][i];
        taa.prevUp[i] = m_prevBasis[1][i];
        taa.prevFwd[i] = m_prevBasis[2][i];
        // The camera position wraps every 64 units; take the short way round.
        double d = m_pos[i] - m_prevPos[i];
        if (d > 32.0) d -= 64.0;
        if (d < -32.0) d += 64.0;
        taa.camDelta[i] = static_cast<float>(d);
    }
    taa.prevTanHalfFov = m_prevTanHalfFov;
    taa.blend = m_taaEnabled ? kTaaBlend : 1.0f;
    taa.historyValid = m_historyValid ? 1.0f : 0.0f;

    ID3D11DeviceContext* ctx = renderer.Context();
    auto upload = [&](ID3D11Buffer* buffer, const void* data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, data, size);
            ctx->Unmap(buffer, 0);
        }
    };
    upload(m_cb.Get(), &c, sizeof(c));
    upload(m_taaCB.Get(), &taa, sizeof(taa));
    PostConstants post{};
    post.snScreen[0] = novaScreen[0];
    post.snScreen[1] = novaScreen[1];
    post.snFlare = novaFlare;
    post.bloom = 1.0f + 0.6f * m_buildUp + 0.5f * m_bass + 0.8f * m_dropFlash;
    for (int i = 0; i < 3; ++i) post.flareCol[i] = c.snCol[i];
    post.flash = 0.08f * std::exp(-m_nova.age / 0.12f) * (m_nova.active ? 1.0f : 0.0f);
    upload(m_postCB.Get(), &post, sizeof(post));
    ID3D11Buffer* cbs[2] = { m_cb.Get(), m_taaCB.Get() };
    ID3D11ShaderResourceView* none[2] = {};

    // Pass 1: raymarch the gas (jittered) into the current-frame buffer.
    renderer.BindTarget(m_current.rtv.Get(), w, h, time, dt);
    ID3D11ShaderResourceView* noise = m_noise.Get();
    ID3D11SamplerState* wrap = m_sampler.Get();
    ctx->PSSetConstantBuffers(2, 2, cbs);
    ctx->PSSetShaderResources(0, 1, &noise);
    ctx->PSSetSamplers(0, 1, &wrap);
    renderer.DrawFullscreen(m_ps.Get());
    ctx->PSSetShaderResources(0, 1, none);

    // Pass 2: resolve against history -> resolved image + new history (two outputs).
    RenderTarget& prevHistory = m_history[m_historyIndex];
    RenderTarget& nextHistory = m_history[1 - m_historyIndex];
    renderer.BindTarget(m_resolved.rtv.Get(), w, h, time, dt);
    ID3D11RenderTargetView* targets[2] = { m_resolved.rtv.Get(), nextHistory.rtv.Get() };
    ctx->OMSetRenderTargets(2, targets, nullptr);
    ID3D11ShaderResourceView* inputs[2] = { m_current.srv.Get(), prevHistory.srv.Get() };
    ID3D11SamplerState* clamp = m_clamp.Get();
    ctx->PSSetConstantBuffers(2, 2, cbs);
    ctx->PSSetShaderResources(0, 2, inputs);
    ctx->PSSetSamplers(0, 1, &clamp);
    renderer.DrawFullscreen(m_taaPS.Get());
    ctx->PSSetShaderResources(0, 2, none);

    // Pass 3: bloom, lens flare and vignette into the host's target.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(m_resolved.srv.Get());
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11Buffer* postCB = m_postCB.Get();
    ID3D11ShaderResourceView* resolved = m_resolved.srv.Get();
    ctx->PSSetConstantBuffers(3, 1, &postCB);
    ctx->PSSetShaderResources(0, 1, &resolved);
    ctx->PSSetSamplers(0, 1, &clamp);
    renderer.DrawFullscreen(m_postPS.Get());
    ctx->PSSetShaderResources(0, 1, none);
    m_historyIndex = 1 - m_historyIndex;
    m_historyValid = true;

    // Remember this frame's camera for next frame's reprojection.
    memcpy(m_prevBasis, m_basis, sizeof(m_basis));
    memcpy(m_prevPos, m_pos, sizeof(m_pos));
    m_prevTanHalfFov = c.tanHalfFov;
}

bool NebulaVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextLook();
        m_message = std::wstring(kPalettes[m_paletteIndex].name) + L"  ·  " + kShapes[m_shapeIndex].name;
        return true;
    }
    if (vk == 'Q') {   // supernova, as on a drop
        m_lastDropCount = ~0u;
        return true;
    }
    if (vk == 'P') {   // summon a pulsar
        SpawnPulsar();
        m_pulsar.fade = 0.0f;
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    }
    return false;
}

std::wstring NebulaVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

void NebulaVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Look look = CurrentLook();
    for (int c = 0; c < 3; ++c) {   // look colours are linear; overlays want display values
        lo[c] = std::pow(look.mid[c], 1.0f / 2.2f);
        hi[c] = std::pow(look.hot[c], 1.0f / 2.2f);
    }
}

std::wstring NebulaVisualizer::Status() const {
    return std::wstring(m_hasArt ? L"Album colours" : kPalettes[m_paletteIndex].name) + L" · " +
           kShapes[m_shapeIndex].name + (m_locked ? L" (locked)" : L"");
}

void NebulaVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    const bool changed = (palette != nullptr) != m_hasArt ||
                         (palette && memcmp(palette, &m_art, sizeof(TrackPalette)) != 0);
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    m_artVariant = 0;
    // New song, or its colours just arrived / were toggled: blend to a matching look.
    if (!m_locked && (newTrack || changed)) NextLook(newTrack ? 2.0f : 3.0f);
}
