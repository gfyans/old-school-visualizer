#pragma once
#include <wrl/client.h>
#include <random>
#include "render/RenderTarget.h"
#include "viz/Visualizer.h"

// "Liquid": coloured ink in moving water, a real fluid simulation on the GPU (stable fluids: the flow is
// carried along itself, kept swirling by vorticity confinement and made incompressible by a pressure
// solve; the ink rides on it). The music pushes the water:
//
//   level     -> three emitters drifting round the screen trail a steady stream of ink
//   kick      -> each emitter splashes: a burst of ink thrown outward, curling into vortex pairs
//   snare     -> a drop of a contrasting colour lands and blooms
//   hi-hats   -> finer turbulence (stronger vorticity), so the edges feather
//   build-up  -> the emitters circle faster and the whole flow winds into one whirlpool
//   drop      -> a huge burst from the centre floods the screen with a new colour
//
//   looks     -> Ink (pigment in backlit water), Neon (glowing dye with bloom), Chrome (liquid metal)
//   keys      -> N next look, M new colours, Q burst, Space lock
class LiquidVisualizer : public IVisualizer {
public:
    LiquidVisualizer();

    const wchar_t* Name() const override { return L"Liquid"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.75f; }
    void Activate() override { m_clear = true; }
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next look (Ink, Neon, Chrome)" }, { 'M', L"New colours" }, { 'Q', L"Burst" },
                 { VK_SPACE, L"Lock / unlock the look" } };
    }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    static constexpr int kMaxSplats = 24;
    static constexpr int kEmitters = 3;

private:
    enum Look { Ink, Neon, Chrome, kLookCount };

    bool EnsureTargets(int w, int h);
    void Splat(float x, float y, float radius, float fx, float fy, const float colour[3], float amount);
    void Burst();
    void Colour(int which, float out[3]) const;   // 0, 1 the main colours, 2 the accent
    void Pass(Renderer& renderer, RenderTarget& dst, ID3D11PixelShader* ps, ID3D11ShaderResourceView* a,
              ID3D11ShaderResourceView* b, float time, float dt);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_curl, m_vorticity, m_splatVel, m_splatDye, m_divergence,
        m_pressure, m_gradient, m_advectVel, m_advectDye, m_display;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linear, m_mips;
    RenderTarget m_vel[2], m_pressureRT[2], m_div, m_curlRT, m_dye[2];
    int m_v = 0, m_p = 0, m_d = 0;   // which of each pair is current
    bool m_clear = true;
    std::mt19937 m_rng;

    int m_look = Ink;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};
    float m_hue = 0.0f;

    // The music's pushes this frame (cleared each Update).
    float m_splats[kMaxSplats][12] = {};   // pos xy, radius, amount, force xy, colour rgb
    int m_splatCount = 0;

    float m_emitAngle[kEmitters] = {};
    float m_emitPos[kEmitters][2] = {};
    bool m_emitStarted = false;
    float m_time = 0.0f, m_dt = 0.0f;
    float m_kick = 0.0f, m_flash = 0.0f, m_energy = 0.0f, m_hat = 0.0f, m_buildUp = 0.0f, m_level = 0.0f;
    float m_swirlDir = 1.0f;
    bool m_silent = true;
    float m_aspect = 16.0f / 9.0f;
    uint32_t m_lastBeat = 0, m_lastSnare = 0, m_lastDrop = 0;
    std::wstring m_message;
};
