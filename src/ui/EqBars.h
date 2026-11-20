#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "audio/AudioFeatures.h"

class Renderer;

// Optional cyberpunk equaliser along the bottom of the screen, drawn over any
// visualiser: slanted neon LED columns with falling peak segments, chromatic
// aberration, scanlines, glitch tears on hard hits and a HUD frame. Lightly
// tinted by the active visualiser's palette. Fades in/out; free while hidden.
class EqBars {
public:
    static constexpr int kBars = 48;

    bool CreateResources(ID3D11Device* device);
    void ReleaseResources();

    void SetVisible(bool visible) { m_visible = visible; }
    bool Visible() const { return m_visible; }

    // lo/hi: palette for the bars' bottom and top (display RGB 0..1).
    void Update(const AudioFeatures& audio, const float lo[3], const float hi[3], float dt);
    // Draws onto whatever target is bound (the back buffer), alpha-blended.
    void Render(Renderer& renderer);

private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;

    bool m_visible = false;
    float m_opacity = 0.0f;
    float m_values[kBars] = {};
    float m_peaks[kBars] = {};
    float m_peakHold[kBars] = {};
    float m_peakVelocity[kBars] = {};
    float m_lo[3] = { 0.15f, 0.45f, 1.0f };
    float m_hi[3] = { 1.0f, 0.3f, 0.7f };
    float m_beat = 0.0f;
    float m_glitch = 0.0f, m_seed = 0.0f, m_time = 0.0f;
    uint32_t m_lastBeat = 0, m_lastDrop = 0;
};
