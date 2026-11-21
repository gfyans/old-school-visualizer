#include "viz/liquid/LiquidVisualizer.h"
#include "render/Renderer.h"
#include "g_liquid_curl_ps.h"
#include "g_liquid_vorticity_ps.h"
#include "g_liquid_splat_vel_ps.h"
#include "g_liquid_splat_dye_ps.h"
#include "g_liquid_divergence_ps.h"
#include "g_liquid_pressure_ps.h"
#include "g_liquid_gradient_ps.h"
#include "g_liquid_advect_vel_ps.h"
#include "g_liquid_advect_dye_ps.h"
#include "g_liquid_display_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct LiquidConstants {   // matches cbuffer Liquid in liquid.hlsli
    float simTexel[2];  float dyeTexel[2];
    float dt, curlStrength, velKeep, dyeKeep;
    float swirl, look, aspect, time;
    float flash, kick, splatCount, energy;
    float tint[3];      float buildUp;
    float splatPos[LiquidVisualizer::kMaxSplats][4];
    float splatForce[LiquidVisualizer::kMaxSplats][4];
    float splatColour[LiquidVisualizer::kMaxSplats][4];
};

const wchar_t* kLooks[] = { L"Ink", L"Neon", L"Chrome" };
constexpr float kPi = 3.14159265f;
constexpr int kSimHeight = 180;        // the flow's grid (the dye is finer)
constexpr int kDyeMaxHeight = 720;
constexpr int kPressureSteps = 22;

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

void HsvToRgb(float h, float s, float v, float out[3]) {
    h = h - std::floor(h);
    for (int c = 0; c < 3; ++c) {
        const float k = std::fmod(5.0f - 2.0f * c + h * 6.0f, 6.0f);
        out[c] = v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    }
}

} // namespace

LiquidVisualizer::LiquidVisualizer() : m_rng(std::random_device{}()) {
    m_look = std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng);
    m_hue = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
    for (int i = 0; i < kEmitters; ++i) m_emitAngle[i] = 2.0f * kPi * i / kEmitters;
    // Developer switch: VISUALIZER_LIQUID=<look 0-2> fixes the look for comparisons.
    wchar_t env[16];
    if (GetEnvironmentVariableW(L"VISUALIZER_LIQUID", env, ARRAYSIZE(env)) > 0) {
        const int look = _wtoi(env);
        if (look >= 0 && look < kLookCount) { m_look = look; m_locked = true; }
    }
}

bool LiquidVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    struct { const BYTE* code; size_t size; ID3D11PixelShader** out; } shaders[] = {
        { g_liquid_curl_ps, sizeof(g_liquid_curl_ps), &m_curl },
        { g_liquid_vorticity_ps, sizeof(g_liquid_vorticity_ps), &m_vorticity },
        { g_liquid_splat_vel_ps, sizeof(g_liquid_splat_vel_ps), &m_splatVel },
        { g_liquid_splat_dye_ps, sizeof(g_liquid_splat_dye_ps), &m_splatDye },
        { g_liquid_divergence_ps, sizeof(g_liquid_divergence_ps), &m_divergence },
        { g_liquid_pressure_ps, sizeof(g_liquid_pressure_ps), &m_pressure },
        { g_liquid_gradient_ps, sizeof(g_liquid_gradient_ps), &m_gradient },
        { g_liquid_advect_vel_ps, sizeof(g_liquid_advect_vel_ps), &m_advectVel },
        { g_liquid_advect_dye_ps, sizeof(g_liquid_advect_dye_ps), &m_advectDye },
        { g_liquid_display_ps, sizeof(g_liquid_display_ps), &m_display },
    };
    for (auto& s : shaders)
        if (FAILED(device->CreatePixelShader(s.code, s.size, nullptr, s.out))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(LiquidConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = 0.0f;
    if (FAILED(device->CreateSamplerState(&s, &m_linear))) return false;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_mips))) return false;
    m_clear = true;
    return true;
}

void LiquidVisualizer::ReleaseResources() {
    for (RenderTarget* t : { &m_vel[0], &m_vel[1], &m_pressureRT[0], &m_pressureRT[1], &m_div, &m_curlRT, &m_dye[0], &m_dye[1] })
        t->Reset();
    m_mips.Reset();
    m_linear.Reset();
    m_cb.Reset();
    Microsoft::WRL::ComPtr<ID3D11PixelShader>* shaders[] = { &m_curl, &m_vorticity, &m_splatVel, &m_splatDye, &m_divergence,
                                                             &m_pressure, &m_gradient, &m_advectVel, &m_advectDye, &m_display };
    for (auto* ps : shaders) ps->Reset();
    m_device.Reset();
}

bool LiquidVisualizer::EnsureTargets(int w, int h) {
    const float aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
    const int simH = kSimHeight, simW = std::max(16, static_cast<int>(std::lround(simH * aspect)));
    const int dyeH = std::min(h, kDyeMaxHeight), dyeW = std::max(16, static_cast<int>(std::lround(dyeH * aspect)));
    if (m_vel[0].width == simW && m_vel[0].height == simH && m_dye[0].width == dyeW && m_dye[0].height == dyeH) return true;
    ID3D11Device* d = m_device.Get();
    const bool ok = m_vel[0].Create(d, simW, simH, DXGI_FORMAT_R16G16_FLOAT) && m_vel[1].Create(d, simW, simH, DXGI_FORMAT_R16G16_FLOAT) &&
                    m_pressureRT[0].Create(d, simW, simH, DXGI_FORMAT_R16_FLOAT) && m_pressureRT[1].Create(d, simW, simH, DXGI_FORMAT_R16_FLOAT) &&
                    m_div.Create(d, simW, simH, DXGI_FORMAT_R16_FLOAT) && m_curlRT.Create(d, simW, simH, DXGI_FORMAT_R16_FLOAT) &&
                    m_dye[0].Create(d, dyeW, dyeH, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
                    m_dye[1].Create(d, dyeW, dyeH, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    m_aspect = aspect;
    m_clear = true;
    return ok;
}

void LiquidVisualizer::Colour(int which, float out[3]) const {
    if (m_hasArt && which < 2) {
        memcpy(out, which == 0 ? m_art.mid : m_art.hot, sizeof(float) * 3);
        return;
    }
    const float hues[3] = { m_hue, m_hue + 0.13f, m_hue + 0.5f };
    HsvToRgb(hues[which], which == 2 ? 0.7f : 0.85f, 1.0f, out);
}

void LiquidVisualizer::Splat(float x, float y, float radius, float fx, float fy, const float colour[3], float amount) {
    if (m_splatCount >= kMaxSplats) return;
    float* s = m_splats[m_splatCount++];
    s[0] = x; s[1] = y; s[2] = radius; s[3] = amount;
    s[4] = fx; s[5] = fy;
    s[6] = colour[0]; s[7] = colour[1]; s[8] = colour[2];
}

// A drop: a ring of bursts from the centre, in a new colour, flooding outward.
void LiquidVisualizer::Burst() {
    m_hue += 0.3f + 0.15f * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
    m_flash = 1.0f;
    m_swirlDir = -m_swirlDir;
    float c[3];
    const float spin = std::uniform_real_distribution<float>(0.0f, 2.0f * kPi)(m_rng);
    constexpr int kRing = 12;
    for (int k = 0; k < kRing; ++k) {
        const float a = spin + 2.0f * kPi * k / kRing;
        const float dx = std::cos(a), dy = std::sin(a);
        Colour(k % 2, c);
        Splat(0.5f + 0.07f * dx / m_aspect, 0.5f + 0.07f * dy, 0.05f, dx * 1300.0f, dy * 1300.0f, c, 0.9f);
    }
}

void LiquidVisualizer::Update(const AudioFeatures& audio, float dt) {
    const float tf = audio.tempoFactor;
    m_time += dt;
    m_dt = std::min(dt, 1.0f / 30.0f);
    // (The splat list is used up by Render, so a burst from a key press before this frame isn't lost.)
    m_silent = audio.silent;
    m_level = Smooth(m_level, audio.silent ? 0.0f : audio.level, dt, 0.05f, 0.3f);
    m_energy = Smooth(m_energy, audio.silent ? 0.0f : audio.bass, dt, 0.05f, 0.4f);
    m_hat = Smooth(m_hat, audio.hat, dt, 0.02f, 0.2f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.4f, 0.6f);
    m_kick *= std::exp(-dt / 0.2f);
    m_flash *= std::exp(-dt / 0.35f);
    m_hue += dt * tf * 0.006f;

    // The emitters wander on looping paths round the centre, faster in a build-up.
    float vel[kEmitters][2] = {};
    for (int i = 0; i < kEmitters; ++i) {
        m_emitAngle[i] += dt * tf * (0.22f + 0.15f * m_level + 1.3f * m_buildUp);
        const float a = m_emitAngle[i];
        const float x = 0.5f + (0.30f * std::cos(a) + 0.06f * std::sin(a * 2.7f + i)) / m_aspect;
        const float y = 0.5f + 0.24f * std::sin(a * 1.3f + i * 1.7f);
        if (m_emitStarted && dt > 0.0f) {
            vel[i][0] = (x - m_emitPos[i][0]) / dt;
            vel[i][1] = (y - m_emitPos[i][1]) / dt;
        }
        m_emitPos[i][0] = x;
        m_emitPos[i][1] = y;
    }
    m_emitStarted = true;
    const float simH = static_cast<float>(kSimHeight), simW = simH * m_aspect;
    float c[3];

    if (!audio.silent) {
        // A steady trail of ink from each emitter, as loud as the music.
        for (int i = 0; i < kEmitters && m_level > 0.03f; ++i) {
            Colour(i % 2, c);
            Splat(m_emitPos[i][0], m_emitPos[i][1], 0.016f, vel[i][0] * simW * 0.5f, vel[i][1] * simH * 0.5f, c, m_level * dt * 1.4f);
        }
        // Kicks: every emitter splashes outward from the centre.
        if (audio.beatCount != m_lastBeat) {
            m_lastBeat = audio.beatCount;
            m_kick = std::min(1.0f, 0.4f + 0.8f * audio.bass);
            for (int i = 0; i < kEmitters; ++i) {
                float dx = (m_emitPos[i][0] - 0.5f) * m_aspect, dy = m_emitPos[i][1] - 0.5f;
                const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);
                dx /= len; dy /= len;
                const float push = 300.0f + 650.0f * audio.bass;
                Colour(i % 2, c);
                Splat(m_emitPos[i][0], m_emitPos[i][1], 0.028f + 0.02f * audio.bass, dx * push, dy * push, c, 0.22f + 0.3f * audio.bass);
            }
        }
        // Snares: a drop of the accent colour somewhere.
        if (audio.snareCount != m_lastSnare) {
            m_lastSnare = audio.snareCount;
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            const float a = u(m_rng) * kPi;
            Colour(2, c);
            Splat(0.5f + 0.3f * u(m_rng), 0.5f + 0.3f * u(m_rng), 0.026f,
                  std::cos(a) * 260.0f, std::sin(a) * 260.0f, c, 0.5f);
        }
    } else {
        m_lastBeat = audio.beatCount;
        m_lastSnare = audio.snareCount;
    }
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        Burst();
    }
}

void LiquidVisualizer::Pass(Renderer& renderer, RenderTarget& dst, ID3D11PixelShader* ps, ID3D11ShaderResourceView* a,
                            ID3D11ShaderResourceView* b, float time, float dt) {
    ID3D11DeviceContext* ctx = renderer.Context();
    renderer.BindTarget(dst.rtv.Get(), dst.width, dst.height, time, dt);
    ID3D11ShaderResourceView* srvs[2] = { a, b };
    ctx->PSSetShaderResources(0, 2, srvs);
    renderer.DrawFullscreen(ps);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
}

void LiquidVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    ID3D11DeviceContext* ctx = renderer.Context();
    if (!EnsureTargets(w, h)) return;
    if (m_clear) {
        const float zero[4] = {};
        for (RenderTarget* t : { &m_vel[0], &m_vel[1], &m_pressureRT[0], &m_pressureRT[1], &m_div, &m_curlRT, &m_dye[0], &m_dye[1] })
            ctx->ClearRenderTargetView(t->rtv.Get(), zero);
        m_clear = false;
    }

    // This frame's constants.
    LiquidConstants c{};
    c.simTexel[0] = 1.0f / m_vel[0].width;  c.simTexel[1] = 1.0f / m_vel[0].height;
    c.dyeTexel[0] = 1.0f / m_dye[0].width;  c.dyeTexel[1] = 1.0f / m_dye[0].height;
    const float sdt = m_dt;
    c.dt = sdt;
    c.curlStrength = 7.0f + 16.0f * m_hat + 8.0f * m_energy;
    c.velKeep = std::exp(-sdt * 0.35f);
    c.dyeKeep = std::exp(-sdt * (m_silent ? 1.2f : 0.55f));
    c.swirl = m_swirlDir * m_buildUp * m_buildUp * 1.2f;
    c.look = static_cast<float>(m_look);
    c.aspect = m_aspect;
    c.time = std::fmod(m_time, 1000.0f);
    c.flash = m_flash;
    c.kick = m_kick;
    c.splatCount = static_cast<float>(m_splatCount);
    c.energy = m_energy;
    Colour(0, c.tint);
    c.buildUp = m_buildUp;
    for (int i = 0; i < m_splatCount; ++i) {
        const float* s = m_splats[i];
        c.splatPos[i][0] = s[0]; c.splatPos[i][1] = s[1]; c.splatPos[i][2] = s[2]; c.splatPos[i][3] = s[3];
        c.splatForce[i][0] = s[4]; c.splatForce[i][1] = s[5];
        c.splatColour[i][0] = s[6]; c.splatColour[i][1] = s[7]; c.splatColour[i][2] = s[8];
    }
    m_splatCount = 0;   // used up
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ID3D11SamplerState* samplers[2] = { m_linear.Get(), m_mips.Get() };
    ctx->PSSetSamplers(0, 2, samplers);

    // The step: curl, vorticity (+ swirl), the music's pushes and ink, projection, then advection.
    auto flipV = [&] { m_v = 1 - m_v; };
    Pass(renderer, m_curlRT, m_curl.Get(), m_vel[m_v].srv.Get(), nullptr, time, dt);
    Pass(renderer, m_vel[1 - m_v], m_vorticity.Get(), m_vel[m_v].srv.Get(), m_curlRT.srv.Get(), time, dt); flipV();
    if (c.splatCount > 0.0f) {
        Pass(renderer, m_vel[1 - m_v], m_splatVel.Get(), m_vel[m_v].srv.Get(), nullptr, time, dt); flipV();
        Pass(renderer, m_dye[1 - m_d], m_splatDye.Get(), m_dye[m_d].srv.Get(), nullptr, time, dt); m_d = 1 - m_d;
    }
    Pass(renderer, m_div, m_divergence.Get(), m_vel[m_v].srv.Get(), nullptr, time, dt);
    for (int k = 0; k < kPressureSteps; ++k) {
        Pass(renderer, m_pressureRT[1 - m_p], m_pressure.Get(), m_pressureRT[m_p].srv.Get(), m_div.srv.Get(), time, dt);
        m_p = 1 - m_p;
    }
    Pass(renderer, m_vel[1 - m_v], m_gradient.Get(), m_pressureRT[m_p].srv.Get(), m_vel[m_v].srv.Get(), time, dt); flipV();
    Pass(renderer, m_vel[1 - m_v], m_advectVel.Get(), m_vel[m_v].srv.Get(), m_vel[m_v].srv.Get(), time, dt); flipV();
    Pass(renderer, m_dye[1 - m_d], m_advectDye.Get(), m_dye[m_d].srv.Get(), m_vel[m_v].srv.Get(), time, dt); m_d = 1 - m_d;
    ctx->GenerateMips(m_dye[m_d].srv.Get());   // the looks blur and bloom from the dye's mips

    // Show it.
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11ShaderResourceView* srvs[2] = { m_dye[m_d].srv.Get(), m_vel[m_v].srv.Get() };
    ctx->PSSetShaderResources(0, 2, srvs);
    renderer.DrawFullscreen(m_display.Get());
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
}

bool LiquidVisualizer::OnKey(UINT vk) {
    switch (vk) {
    case 'N':
        m_look = (m_look + 1) % kLookCount;
        m_message = kLooks[m_look];
        return true;
    case 'M':
        m_hue += 0.3f;
        m_message = L"New colours";
        return true;
    case 'Q':
        Burst();
        return true;
    case VK_SPACE:
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    default:
        return false;
    }
}

std::wstring LiquidVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring LiquidVisualizer::Status() const {
    return std::wstring(kLooks[m_look]) + (m_hasArt ? L" · album colours" : L"") + (m_locked ? L" (locked)" : L"");
}

void LiquidVisualizer::AccentColours(float lo[3], float hi[3]) const {
    Colour(0, lo);
    Colour(1, hi);
}

void LiquidVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack) {
        m_hue += 0.3f;
        if (!m_locked) m_look = std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng);
    }
}
