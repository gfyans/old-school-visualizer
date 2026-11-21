#pragma once
#include <wrl/client.h>
#include <random>
#include <vector>
#include "viz/Visualizer.h"

// "Oscilloscope": a vector display on an old CRT. The actual audio waveform drives an
// electron beam drawn as real line segments: brighter where it moves slowly, leaving a
// white-hot core that fades to a coloured phosphor afterglow. Curved glass, graticule,
// scanlines and bloom.
//
//   modes (N)    -> triggered waveform, X/Y vectorscope (stereo field), dual trace (L/R), polar ring
//   bass / beat  -> bigger, brighter trace
//   build-up     -> longer phosphor persistence (trails smear out)
//   drop         -> beam jitter and flash, new mode
//   phrases      -> new phosphor colour (P1 green, P3 amber, P4 blue-white, P7, synth pink, album)
class ScopeVisualizer : public IVisualizer {
public:
    ScopeVisualizer();

    const wchar_t* Name() const override { return L"Oscilloscope"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 1.0f; }   // thin beams want full resolution
    void Activate() override { m_clear = true; }
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next mode" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Phosphor { float core[3], trail[3], grid[3]; };
    static constexpr int kMaxPoints = 2 * 1024 + 2;

private:
    void BuildPoints(float aspect);
    void NextScheme();
    Phosphor CurrentPhosphor() const;
    bool CreatePhosphorTarget(int w, int h);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_beamVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_beamPS, m_decayPS, m_crtPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_beamCB, m_crtCB, m_points;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pointsSRV;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_decayBlend, m_addBlend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_phosphorTex;      // RG16F with mips (bloom)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_phosphorRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_phosphorSRV;
    int m_width = 0, m_height = 0;
    bool m_clear = true;

    std::mt19937 m_rng;
    int m_mode = 0;
    int m_scheme = 0;
    Phosphor m_from{}, m_to{};
    float m_blend = 1.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // Latest audio (copied in Update, turned into points in Render where the aspect is known)
    float m_mid[1024] = {}, m_side[1024] = {};
    std::vector<float> m_pointData;   // float4 per point
    int m_pointCount = 0;

    float m_beat = 0.0f, m_bass = 0.0f, m_buildUp = 0.0f, m_jitter = 0.0f, m_flash = 0.0f;
    bool m_silent = true;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0;
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;
    bool m_changePending = false;
    float m_time = 0.0f;
    std::wstring m_message;
};
