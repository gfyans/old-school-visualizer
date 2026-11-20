#include "ui/EqBars.h"
#include "render/Renderer.h"
#include "g_eqbars_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
struct EqConstants {   // matches cbuffer Eq in eqbars_ps.hlsl
    float values[EqBars::kBars];
    float peaks[EqBars::kBars];
    float lo[3];  float opacity;
    float hi[3];  float beat;
};
static_assert(sizeof(EqConstants) % 16 == 0);

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}
}

bool EqBars::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_eqbars_ps, sizeof(g_eqbars_ps), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(EqConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;

    // Premultiplied alpha: the shader can mix solid glass (alpha) with pure glow (alpha 0, additive).
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&bd, &m_blend));
}

void EqBars::ReleaseResources() {
    m_blend.Reset();
    m_cb.Reset();
    m_ps.Reset();
}

void EqBars::Update(const AudioFeatures& audio, const float lo[3], const float hi[3], float dt) {
    m_opacity = Smooth(m_opacity, m_visible ? 1.0f : 0.0f, dt, 0.25f, 0.35f);
    if (m_opacity < 0.002f && !m_visible) return;

    // 64 log bands -> 48 bars, interpolated so neighbours blend smoothly.
    constexpr int bands = AudioFeatures::kSpectrumBands;
    for (int b = 0; b < kBars; ++b) {
        const float pos = (b + 0.5f) / kBars * (bands - 1);
        const int i = static_cast<int>(pos);
        const float f = pos - i;
        const float v = audio.spectrum[i] * (1.0f - f) + audio.spectrum[std::min(i + 1, bands - 1)] * f;
        m_values[b] = Smooth(m_values[b], v, dt, 0.03f, 0.18f);

        // Peak caps: jump up, hang briefly, then fall with gravity.
        if (m_values[b] >= m_peaks[b]) {
            m_peaks[b] = m_values[b];
            m_peakHold[b] = 0.35f;
            m_peakVelocity[b] = 0.0f;
        } else if ((m_peakHold[b] -= dt) <= 0.0f) {
            m_peakVelocity[b] += 2.2f * dt;
            m_peaks[b] = std::max(m_values[b], m_peaks[b] - m_peakVelocity[b] * dt);
        }
    }
    for (int c = 0; c < 3; ++c) {   // follow the visualiser's palette gently
        m_lo[c] = Smooth(m_lo[c], lo[c], dt, 1.5f, 1.5f);
        m_hi[c] = Smooth(m_hi[c], hi[c], dt, 1.5f, 1.5f);
    }
    m_beat = std::max(audio.beat, audio.tempoConfidence > 0.4f ? std::pow(1.0f - audio.beatPhase, 6.0f) : 0.0f);
}

void EqBars::Render(Renderer& renderer) {
    if (m_opacity < 0.002f || !m_ps) return;

    EqConstants c{};
    memcpy(c.values, m_values, sizeof(c.values));
    memcpy(c.peaks, m_peaks, sizeof(c.peaks));
    memcpy(c.lo, m_lo, sizeof(c.lo));
    memcpy(c.hi, m_hi, sizeof(c.hi));
    c.opacity = m_opacity;
    c.beat = m_beat;

    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &c, sizeof(c));
    ctx->Unmap(m_cb.Get(), 0);

    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->OMSetBlendState(m_blend.Get(), nullptr, 0xFFFFFFFF);
    renderer.DrawFullscreen(m_ps.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}
