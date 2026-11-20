#include "viz/nebula/NebulaVisualizer.h"
#include "render/Renderer.h"
#include "g_nebula_ps.h"
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
};
static_assert(sizeof(NebulaConstants) % 16 == 0);

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
    m_noise.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_ps.Reset();
}

void NebulaVisualizer::Activate() {
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
}

NebulaVisualizer::Look NebulaVisualizer::CurrentLook() const {
    const float t = m_lookBlend * m_lookBlend * (3.0f - 2.0f * m_lookBlend);   // smoothstep
    return Blend(m_from, m_to, t);
}

void NebulaVisualizer::NextLook() {
    m_from = CurrentLook();   // start from exactly what's on screen, even mid-blend
    const int newPalette = (m_paletteIndex + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    const int newShape = (m_shapeIndex + 1 + std::uniform_int_distribution<int>(0, kShapeCount - 2)(m_rng)) % kShapeCount;
    m_paletteIndex = newPalette;
    m_shapeIndex = newShape;
    m_to = MakeLook(m_paletteIndex, m_shapeIndex);
    m_lookBlend = 0.0f;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
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

    // --- Look changes (song sections) ----------------------------------------
    m_timeSinceChange += dt;
    m_intensityBaseline = Smooth(m_intensityBaseline, audio.intensity, dt, 20.0f, 20.0f);
    const bool drop = audio.intensity > m_intensityBaseline + 0.12f && m_timeSinceChange > 10.0f;
    const bool phraseDone = m_beatsSinceChange >= 32 && m_timeSinceChange > 20.0f;
    if (!m_locked && (phraseDone || drop || m_timeSinceChange > 60.0f)) {
        NextLook();
        if (drop) m_intensityBaseline = audio.intensity;   // one change per drop
    }
    m_lookBlend = std::min(1.0f, m_lookBlend + dt / kLookBlendSeconds);

    // --- Smoothed audio drivers ----------------------------------------------
    m_bass = Smooth(m_bass, audio.bass, dt, 0.03f, 0.25f);
    m_mids = Smooth(m_mids, 0.5f * (audio.lowMid + audio.highMid), dt, 0.1f, 0.6f);
    m_treble = Smooth(m_treble, audio.treble, dt, 0.02f, 0.2f);

    const float targetSpeed = audio.silent ? 0.25f
        : 0.35f + 1.1f * audio.intensity + 0.5f * audio.bass + 0.25f * m_beatPulse;
    m_speed = Smooth(m_speed, targetSpeed, dt, 0.5f, 2.0f);

    // Beat zoom: small and eased in (never a one-frame jump), which is what avoids motion sickness.
    m_zoom = Smooth(m_zoom, m_beatPulse, dt, 0.15f, 0.6f);

    const float targetExposure = audio.silent ? 0.8f : 0.7f + 0.3f * audio.level + 0.3f * m_beatPulse;
    m_exposure = Smooth(m_exposure, targetExposure, dt, 0.08f, 0.8f);

    const float targetLight = audio.silent ? 0.6f : 0.6f + 0.7f * m_bass + 0.9f * m_beatPulse;
    m_lightStrength = Smooth(m_lightStrength, targetLight, dt, 0.03f, 0.35f);

    m_swirl += dt * (0.003f + 0.02f * m_mids);
    m_rollDrift += dt * 0.04f * m_mids;

    // --- Camera: slow wandering heading, flying forward ------------------------
    const float t = m_time;
    const float yaw = 0.8f * std::sin(t * 0.021f) + 0.4f * std::sin(t * 0.047f + 1.3f);
    const float pitch = 0.35f * std::sin(t * 0.031f + 2.1f);
    const float roll = 0.3f * std::sin(t * 0.017f) + m_rollDrift;
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
        m_pos[c] += double(fwd[c]) * m_speed * dt;
        m_pos[c] = std::fmod(m_pos[c], kWorldPeriod);
        if (m_pos[c] < 0) m_pos[c] += kWorldPeriod;
    }
}

void NebulaVisualizer::Render(Renderer& renderer, float, float) {
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
        c.lightPos[i] = c.camPos[i] + fwd[i] * 9.0f
                      + right[i] * 2.5f * std::sin(m_time * 0.13f)
                      + up[i] * 1.5f * std::sin(m_time * 0.17f + 1.0f);
        c.lightColor[i] = 0.6f * look.hot[i] + 0.4f * look.mid[i];
    }
    c.tanHalfFov = std::tan((38.0f - 0.8f * m_zoom) * kPi / 180.0f);   // beat: gentle zoom swell
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
    c.threshold = look.threshold - 0.035f * m_bass;
    c.gain = look.gain * 1.15f * (1.0f + 0.25f * m_bass);
    c.warp = look.warp * (1.0f + 0.7f * m_mids);
    c.filament = look.filament;
    c.warpOffset[0] = m_swirl;
    c.warpOffset[1] = m_swirl * 0.7f;
    c.warpOffset[2] = m_swirl * 0.4f;
    c.detail = look.detail;

    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ID3D11ShaderResourceView* srv = m_noise.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_ps.Get());
}

bool NebulaVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextLook();
        m_message = std::wstring(kPalettes[m_paletteIndex].name) + L"  ·  " + kShapes[m_shapeIndex].name;
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

std::wstring NebulaVisualizer::Status() const {
    return std::wstring(kPalettes[m_paletteIndex].name) + L" · " + kShapes[m_shapeIndex].name +
           (m_locked ? L" (locked)" : L"");
}
