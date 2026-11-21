#include "viz/scope/ScopeVisualizer.h"
#include "render/Renderer.h"
#include "g_scope_beam_ps.h"
#include "g_scope_beam_vs.h"
#include "g_scope_crt_ps.h"
#include "g_scope_decay_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265f;

struct BeamConstants { float width, energy, pad0, pad1; };   // matches cbuffer Beam
struct CrtConstants {                                          // matches cbuffer Crt
    float core[3];  float bloom;
    float trail[3]; float curvature;
    float grid[3];  float flicker;
    float glare, gridPulse, pad1, pad2;
};

const wchar_t* const kModeNames[] = { L"Waveform", L"X/Y vectorscope", L"Dual trace", L"Polar" };
constexpr int kModeCount = 4;

struct NamedPhosphor { const wchar_t* name; ScopeVisualizer::Phosphor p; };
const NamedPhosphor kPhosphors[] = {
    { L"P1 green",      { { 0.80f, 1.00f, 0.80f }, { 0.15f, 1.00f, 0.30f }, { 0.30f, 0.70f, 0.35f } } },
    { L"P3 amber",      { { 1.00f, 0.95f, 0.75f }, { 1.00f, 0.55f, 0.08f }, { 0.70f, 0.45f, 0.15f } } },
    { L"P4 blue-white", { { 1.00f, 1.00f, 1.00f }, { 0.50f, 0.65f, 1.00f }, { 0.40f, 0.50f, 0.75f } } },
    { L"P7",            { { 0.75f, 0.85f, 1.00f }, { 0.65f, 1.00f, 0.20f }, { 0.45f, 0.65f, 0.30f } } },
    { L"Synth pink",    { { 1.00f, 0.85f, 1.00f }, { 1.00f, 0.20f, 0.80f }, { 0.60f, 0.30f, 0.70f } } },
};
constexpr int kPhosphorCount = static_cast<int>(std::size(kPhosphors));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

ScopeVisualizer::Phosphor FromArt(const TrackPalette& a) {
    ScopeVisualizer::Phosphor p{};
    for (int c = 0; c < 3; ++c) {
        p.trail[c] = a.mid[c];
        p.core[c] = std::min(1.0f, a.hot[c] * 0.5f + 0.5f);
        p.grid[c] = a.mid[c] * 0.6f;
    }
    return p;
}

} // namespace

ScopeVisualizer::ScopeVisualizer() : m_rng(std::random_device{}()) {
    m_scheme = 0;   // start on classic P1 green
    m_from = m_to = kPhosphors[m_scheme].p;
    m_pointData.resize(kMaxPoints * 4, 0.0f);
}

bool ScopeVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    if (FAILED(device->CreateVertexShader(g_scope_beam_vs, sizeof(g_scope_beam_vs), nullptr, &m_beamVS)) ||
        FAILED(device->CreatePixelShader(g_scope_beam_ps, sizeof(g_scope_beam_ps), nullptr, &m_beamPS)) ||
        FAILED(device->CreatePixelShader(g_scope_decay_ps, sizeof(g_scope_decay_ps), nullptr, &m_decayPS)) ||
        FAILED(device->CreatePixelShader(g_scope_crt_ps, sizeof(g_scope_crt_ps), nullptr, &m_crtPS)))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(BeamConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_beamCB))) return false;
    cb.ByteWidth = sizeof(CrtConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_crtCB))) return false;

    // Beam points: a dynamic structured buffer of float4, rewritten every frame.
    D3D11_BUFFER_DESC pb{};
    pb.ByteWidth = kMaxPoints * 4 * sizeof(float);
    pb.Usage = D3D11_USAGE_DYNAMIC;
    pb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    pb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    pb.StructureByteStride = 4 * sizeof(float);
    if (FAILED(device->CreateBuffer(&pb, nullptr, &m_points)) ||
        FAILED(device->CreateShaderResourceView(m_points.Get(), nullptr, &m_pointsSRV)))
        return false;

    // Decay: buffer *= blend factor (per channel: slow afterglow, fast core). Beams: additive.
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &m_decayBlend))) return false;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
    if (FAILED(device->CreateBlendState(&bd, &m_addBlend))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_sampler))) return false;
    m_width = m_height = 0;
    m_clear = true;
    return true;
}

void ScopeVisualizer::ReleaseResources() {
    m_phosphorSRV.Reset();
    m_phosphorRTV.Reset();
    m_phosphorTex.Reset();
    m_sampler.Reset();
    m_addBlend.Reset();
    m_decayBlend.Reset();
    m_pointsSRV.Reset();
    m_points.Reset();
    m_crtCB.Reset();
    m_beamCB.Reset();
    m_crtPS.Reset();
    m_decayPS.Reset();
    m_beamPS.Reset();
    m_beamVS.Reset();
    m_device.Reset();
    m_width = m_height = 0;
}

bool ScopeVisualizer::CreatePhosphorTarget(int w, int h) {
    m_phosphorSRV.Reset();
    m_phosphorRTV.Reset();
    m_phosphorTex.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 0;   // full chain: the CRT pass samples small mips for bloom
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = td.Format;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_phosphorTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_phosphorTex.Get(), &rv, &m_phosphorRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_phosphorTex.Get(), nullptr, &m_phosphorSRV)))
        return false;
    m_width = w;
    m_height = h;
    m_clear = true;
    return true;
}

ScopeVisualizer::Phosphor ScopeVisualizer::CurrentPhosphor() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Phosphor r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Phosphor) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void ScopeVisualizer::NextScheme() {
    m_from = CurrentPhosphor();
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kPhosphorCount - 2)(m_rng)) % kPhosphorCount;
    m_to = m_hasArt && (m_scheme % 2 == 0) ? FromArt(m_art) : kPhosphors[m_scheme].p;   // album colours in the rotation
    m_blend = 0.0f;
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
}

void ScopeVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_silent = audio.silent;
    memcpy(m_mid, audio.scopeMid, sizeof(m_mid));
    memcpy(m_side, audio.scopeSide, sizeof(m_side));

    const bool gridLocked = audio.tempoConfidence > 0.4f;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; if (!gridLocked) { m_beat = 1.0f; ++m_beatsSinceChange; } }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; if (gridLocked) { m_beat = 1.0f; ++m_beatsSinceChange; } }
    m_beat *= std::exp(-dt / 0.18f);
    m_bass = Smooth(m_bass, audio.bass, dt, 0.03f, 0.25f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.5f, 0.3f);

    // Drops: the beam shudders and flares, and the display switches mode.
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        m_jitter = 1.0f;
        m_flash = 1.0f;
        if (!m_locked) m_mode = (m_mode + 1 + std::uniform_int_distribution<int>(0, kModeCount - 2)(m_rng)) % kModeCount;
    }
    m_jitter *= std::exp(-dt / 0.3f);
    m_flash *= std::exp(-dt / 0.25f);

    // Phrases: a new phosphor colour every 8 bars, on the downbeat.
    if (m_beatsSinceChange >= 32 && m_timeSinceChange > 20.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 60.0f)) NextScheme();
    m_blend = std::min(1.0f, m_blend + dt / 3.0f);
}

void ScopeVisualizer::BuildPoints(float aspect) {
    constexpr int n = AudioFeatures::kScopePoints;
    const float gain = 0.85f + 0.3f * m_bass + 0.25f * m_beat;
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const float jitter = 0.02f * m_jitter + (m_silent ? 0.0015f : 0.0f);   // idle beam has a little noise
    // Horizontal sweep (height units): a little past both edges, so the beam runs off the glass
    // like a real scope's timebase rather than stopping short of it.
    const float width = aspect * 1.04f;
    int count = 0;
    auto add = [&](float x, float y, float intensity, bool on) {
        float* p = &m_pointData[count * 4];
        p[0] = x + jitter * noise(m_rng);
        p[1] = y + jitter * noise(m_rng);
        p[2] = intensity;
        p[3] = on ? 1.0f : 0.0f;
        ++count;
    };

    switch (m_mode) {
    case 0:   // Triggered waveform, sweeping left to right.
        for (int i = 0; i < n; ++i) add(-width + 2.0f * width * i / (n - 1), m_mid[i] * 0.75f * gain, 1.0f, true);
        break;
    case 1:   // X/Y vectorscope: mono is vertical, stereo width spreads it sideways.
        for (int i = 0; i < n; ++i) add(m_side[i] * 1.2f * gain, m_mid[i] * 0.85f * gain, 1.0f, true);
        break;
    case 2:   // Dual trace: left channel above, right below.
        for (int i = 0; i < n; i += 2) add(-width + 2.0f * width * i / (n - 1), 0.42f + (m_mid[i] + m_side[i]) * 0.32f * gain, 1.0f, true);
        add(width, 0.42f, 0.0f, false);   // pen up between traces
        for (int i = 0; i < n; i += 2) add(-width + 2.0f * width * i / (n - 1), -0.42f + (m_mid[i] - m_side[i]) * 0.32f * gain, 1.0f, true);
        break;
    default:  // Polar: the waveform wrapped around a ring.
        for (int i = 0; i <= n; ++i) {
            const int k = i % n;
            const float a = 2.0f * kPi * i / n;
            // Blend the last stretch into the start so the ring closes without a seam.
            const float seam = std::clamp((i - n * 0.88f) / (n * 0.12f), 0.0f, 1.0f);
            const float v = m_mid[k] + (m_mid[0] - m_mid[k]) * seam * seam * (3.0f - 2.0f * seam);
            const float r = 0.5f + 0.28f * v * gain;
            add(r * std::cos(a), r * std::sin(a), 1.0f, true);
        }
        break;
    }
    m_pointCount = count;
}

void ScopeVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    ID3D11DeviceContext* ctx = renderer.Context();
    if ((w != m_width || h != m_height) && !CreatePhosphorTarget(w, h)) return;
    if (m_clear) {
        const float black[4] = {};
        ctx->ClearRenderTargetView(m_phosphorRTV.Get(), black);
        m_clear = false;
    }

    BuildPoints(static_cast<float>(w) / h);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_points.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_pointData.data(), m_pointCount * 4 * sizeof(float));
        ctx->Unmap(m_points.Get(), 0);
    }
    const float scale = h / 1080.0f;
    BeamConstants beam{ 1.35f * scale, 1.0f + 0.8f * m_beat + 0.5f * m_buildUp + 1.5f * m_flash, 0, 0 };
    if (SUCCEEDED(ctx->Map(m_beamCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &beam, sizeof(beam));
        ctx->Unmap(m_beamCB.Get(), 0);
    }
    const Phosphor ph = CurrentPhosphor();
    CrtConstants crt{};
    memcpy(crt.core, ph.core, sizeof(crt.core));
    memcpy(crt.trail, ph.trail, sizeof(crt.trail));
    memcpy(crt.grid, ph.grid, sizeof(crt.grid));
    crt.bloom = 0.9f + 0.5f * m_beat;
    crt.curvature = 0.045f;   // the grid's bulge
    crt.flicker = 0.015f * std::sin(m_time * 377.0f);   // faint mains-frequency shimmer
    crt.glare = 1.0f;
    crt.gridPulse = m_beat;   // the neon grid swells a little on the beat
    if (SUCCEEDED(ctx->Map(m_crtCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &crt, sizeof(crt));
        ctx->Unmap(m_crtCB.Get(), 0);
    }

    // 1. Phosphor decay: slow afterglow (longer during build-ups) and a fast white-hot core.
    const float slowHalfLife = 0.07f + 0.25f * m_buildUp;
    const float factor[4] = { std::exp(-dt * 0.693f / slowHalfLife), std::exp(-dt * 0.693f / 0.022f), 1.0f, 1.0f };
    renderer.BindTarget(m_phosphorRTV.Get(), w, h, time, dt);
    ctx->OMSetBlendState(m_decayBlend.Get(), factor, 0xFFFFFFFF);
    renderer.DrawFullscreen(m_decayPS.Get());

    // 2. The beam, additively.
    if (m_pointCount > 1) {
        ID3D11Buffer* cb = m_beamCB.Get();
        ID3D11ShaderResourceView* pts = m_pointsSRV.Get();
        ctx->OMSetBlendState(m_addBlend.Get(), nullptr, 0xFFFFFFFF);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(m_beamVS.Get(), nullptr, 0);
        ctx->VSSetShaderResources(0, 1, &pts);
        ctx->VSSetConstantBuffers(2, 1, &cb);
        ctx->PSSetConstantBuffers(2, 1, &cb);
        ctx->PSSetShader(m_beamPS.Get(), nullptr, 0);
        ctx->Draw(6 * (m_pointCount - 1), 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->VSSetShaderResources(0, 1, &none);
    }
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // 3. Bloom mips, then the CRT into the host's target.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(m_phosphorSRV.Get());
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11Buffer* crtCB = m_crtCB.Get();
    ID3D11ShaderResourceView* srv = m_phosphorSRV.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ctx->PSSetConstantBuffers(2, 1, &crtCB);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_crtPS.Get());
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

bool ScopeVisualizer::OnKey(UINT vk) {
    if (vk == 'Q') {   // the drop's moment, by hand: the next update sees a "new" drop
        m_lastDrop = ~0u;
        return true;
    }
    if (vk == 'N') {
        m_mode = (m_mode + 1) % kModeCount;
        m_message = kModeNames[m_mode];
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Mode and colour locked" : L"Mode and colour unlocked";
        return true;
    }
    return false;
}

std::wstring ScopeVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring ScopeVisualizer::Status() const {
    const bool art = m_hasArt && (m_scheme % 2 == 0);
    return std::wstring(kModeNames[m_mode]) + L" · " + (art ? L"album colours" : kPhosphors[m_scheme].name) +
           (m_locked ? L" (locked)" : L"");
}

void ScopeVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Phosphor p = CurrentPhosphor();
    memcpy(lo, p.trail, 3 * sizeof(float));
    memcpy(hi, p.core, 3 * sizeof(float));
}

void ScopeVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) NextScheme();
}
