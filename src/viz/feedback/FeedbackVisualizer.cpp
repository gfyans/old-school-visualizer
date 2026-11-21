#include "viz/feedback/FeedbackVisualizer.h"
#include "render/Renderer.h"
#include "g_feedback_out_ps.h"
#include "g_feedback_warp_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct FeedbackConstants {   // matches cbuffer Feedback in feedback_warp_ps.hlsl
    float zoom, rotation, warpAmp, warpFreq;
    float decay, hueShift, mirror, inkShape;
    float inkA[3]; float inkGlow;
    float inkB[3]; float ringRadius;
    float waveScale, warpPhase, sparks, flash;
};
static_assert(sizeof(FeedbackConstants) % 16 == 0);
static_assert(sizeof(FeedbackConstants) <= sizeof(float) * 24);

struct Preset {
    const wchar_t* name;
    FeedbackVisualizer::Params p;   // per-frame values at 60 fps
};
// zoom < 1 flies into the image, > 1 pulls out; mirror = kaleidoscope segments (0 = off);
// inkShape 0 = waveform ring, 1 = waveform lines.
const Preset kPresets[] = {
    { L"Tunnel",       { 0.975f,  0.004f, 0.003f,  9.0f, 0.975f, 0.004f, 0.0f, 0.0f, 0.22f, 0.06f } },
    { L"Vortex",       { 0.988f,  0.022f, 0.002f,  7.0f, 0.970f, 0.006f, 0.0f, 0.0f, 0.18f, 0.05f } },
    { L"Ripples",      { 0.995f,  0.000f, 0.010f, 14.0f, 0.962f, 0.003f, 0.0f, 1.0f, 0.00f, 0.10f } },
    { L"Supernova",    { 1.018f, -0.006f, 0.002f,  8.0f, 0.968f, 0.005f, 0.0f, 0.0f, 0.12f, 0.05f } },
    { L"Kaleidoscope", { 0.982f,  0.010f, 0.004f, 10.0f, 0.972f, 0.005f, 6.0f, 0.0f, 0.20f, 0.06f } },
    { L"Mandala",      { 0.990f,  0.006f, 0.006f, 12.0f, 0.975f, 0.004f, 8.0f, 1.0f, 0.00f, 0.08f } },
};
constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

void HsvToRgb(float h, float s, float v, float out[3]) {
    h = h - std::floor(h);
    for (int c = 0; c < 3; ++c) {
        const float k = std::fmod(5.0f - 2.0f * c + h * 6.0f, 6.0f);   // r: n=5, g: n=3, b: n=1
        out[c] = v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    }
}

} // namespace

FeedbackVisualizer::FeedbackVisualizer() : m_rng(std::random_device{}()) {
    m_preset = std::uniform_int_distribution<int>(0, kPresetCount - 1)(m_rng);
    m_from = m_to = kPresets[m_preset].p;
    m_hue = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
}

bool FeedbackVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    if (FAILED(device->CreatePixelShader(g_feedback_warp_ps, sizeof(g_feedback_warp_ps), nullptr, &m_warpPS)) ||
        FAILED(device->CreatePixelShader(g_feedback_out_ps, sizeof(g_feedback_out_ps), nullptr, &m_outPS)))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FeedbackConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;   // black beyond the edges
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_border))) return false;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&s, &m_linear))) return false;
    m_clear = true;
    return true;
}

void FeedbackVisualizer::ReleaseResources() {
    m_buffers[0].Reset();
    m_buffers[1].Reset();
    m_linear.Reset();
    m_border.Reset();
    m_cb.Reset();
    m_outPS.Reset();
    m_warpPS.Reset();
    m_device.Reset();
}

FeedbackVisualizer::Params FeedbackVisualizer::Current() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Params r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* out = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Params) / sizeof(float); ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    // Discrete choices switch half-way through the blend.
    r.mirror = t < 0.5f ? m_from.mirror : m_to.mirror;
    r.inkShape = t < 0.5f ? m_from.inkShape : m_to.inkShape;
    return r;
}

void FeedbackVisualizer::NextPreset(float blendSeconds) {
    m_from = Current();
    m_preset = (m_preset + 1 + std::uniform_int_distribution<int>(0, kPresetCount - 2)(m_rng)) % kPresetCount;
    m_to = kPresets[m_preset].p;
    m_blend = 0.0f;
    m_blendSeconds = blendSeconds;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
    m_hue += 0.25f + 0.2f * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
}

void FeedbackVisualizer::InkColours(float a[3], float b[3]) const {
    if (m_hasArt) {
        memcpy(a, m_art.mid, sizeof(float) * 3);
        memcpy(b, m_art.hot, sizeof(float) * 3);
    } else {
        HsvToRgb(m_hue, 0.75f, 1.0f, a);
        HsvToRgb(m_hue + 0.33f, 0.65f, 1.0f, b);
    }
}

void FeedbackVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_silent = audio.silent;

    // --- Pulses: heard beats, and the predicted grid when it's locked ---
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; m_beatPulse = 1.0f; }
    m_beatPulse *= std::exp(-dt / 0.2f);
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    const float gridPulse = gridLocked ? std::pow(1.0f - audio.beatPhase, 5.0f) : 0.0f;
    m_pulse = gridLocked ? std::max(gridPulse, 0.5f * m_beatPulse) : m_beatPulse;
    if (audio.gridBeat != m_lastGridBeat) { m_lastGridBeat = audio.gridBeat; if (gridLocked) ++m_beatsSinceChange; }
    if (!gridLocked && m_beatPulse > 0.99f) ++m_beatsSinceChange;

    // --- Structure: drops and phrases ---
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        m_flash = 1.0f;
        m_rotationDir = -m_rotationDir;
        if (!m_locked && m_timeSinceChange > 3.0f) NextPreset(0.6f);
    }
    m_flash *= std::exp(-dt / 0.4f);
    if (m_beatsSinceChange >= 32 && m_timeSinceChange > 16.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 45.0f))
        NextPreset(3.0f);
    m_blend = std::min(1.0f, m_blend + dt / m_blendSeconds);

    m_bass = Smooth(m_bass, audio.bass, dt, 0.03f, 0.25f);
    m_mids = Smooth(m_mids, 0.5f * (audio.lowMid + audio.highMid), dt, 0.1f, 0.5f);
    m_treble = Smooth(m_treble, std::max(audio.treble * 0.6f, audio.hat), dt, 0.02f, 0.2f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.5f, 0.3f);
    const float tf = audio.tempoFactor;   // faster songs animate faster
    m_hue += dt * tf * (0.01f + 0.03f * m_mids);
    m_warpPhase += dt * tf * (1.0f + 2.0f * m_mids + 2.0f * m_buildUp);

    // --- This frame's warp, scaled so motion is independent of frame rate ---
    const Params p = Current();
    const float frames = dt * 60.0f;
    FeedbackConstants c{};
    c.zoom = std::pow(p.zoom, frames * (1.0f + 1.2f * m_bass + 1.5f * m_pulse + 1.5f * m_buildUp));
    c.rotation = p.rotation * m_rotationDir * frames * (1.0f + 1.5f * m_mids + 2.0f * m_buildUp);
    c.warpAmp = p.warpAmp * (1.0f + 2.0f * m_treble);
    c.warpFreq = p.warpFreq;
    c.decay = std::pow(m_silent ? 0.93f : p.decay, frames);   // silence: trails fade away
    c.hueShift = p.hueShift * frames;
    c.mirror = p.mirror;
    c.inkShape = p.inkShape;
    InkColours(c.inkA, c.inkB);
    c.inkGlow = m_pulse + 0.3f * m_buildUp;
    c.ringRadius = p.ringRadius * (1.0f + 0.15f * m_bass);
    c.waveScale = m_silent ? 0.0f : p.waveScale;
    c.warpPhase = m_warpPhase;
    c.sparks = m_treble * 1.5f + m_buildUp;
    c.flash = m_flash;
    memcpy(m_constants, &c, sizeof(c));
}

void FeedbackVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    ID3D11DeviceContext* ctx = renderer.Context();

    if (m_buffers[0].width != w || m_buffers[0].height != h) {
        // 16-bit float: long, smooth trails without 8-bit banding as they fade.
        m_buffers[0].Create(m_device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_buffers[1].Create(m_device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_clear = true;
    }
    if (!m_buffers[0].rtv || !m_buffers[1].rtv) return;
    if (m_clear) {
        const float black[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(m_buffers[0].rtv.Get(), black);
        ctx->ClearRenderTargetView(m_buffers[1].rtv.Get(), black);
        m_clear = false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(FeedbackConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();
    ID3D11ShaderResourceView* none = nullptr;

    // Pass 1: previous buffer -> warp + ink -> other buffer.
    RenderTarget& src = m_buffers[m_current];
    RenderTarget& dst = m_buffers[1 - m_current];
    renderer.BindTarget(dst.rtv.Get(), w, h, time, dt);
    ID3D11ShaderResourceView* srv = src.srv.Get();
    ID3D11SamplerState* border = m_border.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &border);
    renderer.DrawFullscreen(m_warpPS.Get());
    ctx->PSSetShaderResources(0, 1, &none);
    m_current = 1 - m_current;

    // Pass 2: present into the host's target.
    renderer.BindTarget(hostTarget, w, h, time, dt);
    srv = dst.srv.Get();
    ID3D11SamplerState* linear = m_linear.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &linear);
    renderer.DrawFullscreen(m_outPS.Get());
    ctx->PSSetShaderResources(0, 1, &none);
}

bool FeedbackVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextPreset(1.5f);
        m_message = kPresets[m_preset].name;
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Preset locked" : L"Preset unlocked";
        return true;
    }
    return false;
}

std::wstring FeedbackVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring FeedbackVisualizer::Status() const {
    return std::wstring(kPresets[m_preset].name) + (m_hasArt ? L" · album colours" : L"") +
           (m_locked ? L" (locked)" : L"");
}

void FeedbackVisualizer::AccentColours(float lo[3], float hi[3]) const {
    InkColours(lo, hi);
}

void FeedbackVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) NextPreset(1.0f);
}
