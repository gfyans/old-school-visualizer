#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// Flying through a glowing gas nebula.
//
// Music response (all smoothed here on the CPU, then passed to nebula_ps.hlsl):
//   bass      -> denser, brighter gas; the light core swells
//   mids      -> more turbulence and faster churning of the gas
//   treble    -> stars twinkle brighter, sparkles in the gas
//   beat      -> shockwave shell from the core, brightness kick, slight zoom punch
//   intensity -> flight speed and overall brightness (calm sections drift, loud ones rush)
//   sections  -> every ~32 beats, or on a sudden "drop", blends to a new look
//                (palette + gas shape) over a few seconds
class NebulaVisualizer : public IVisualizer {
public:
    NebulaVisualizer();

    const wchar_t* Name() const override { return L"Nebula"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.6f; }   // soft gas tolerates upscaling well
    void Activate() override;
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Look {
        float deep[3], mid[3], hot[3];                       // palette (linear RGB)
        float threshold, gain, warp, filament, absorb, detail;   // gas shape
    };

private:
    void NextLook(float blendSeconds = 6.0f);
    Look CurrentLook() const;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_noise;

    std::mt19937 m_rng;

    // Look (palette + shape) blending
    Look m_from{}, m_to{};
    int m_paletteIndex = 0, m_shapeIndex = 0;
    float m_lookBlend = 1.0f;
    bool m_locked = false;
    float m_timeSinceChange = 0.0f;
    int m_beatsSinceChange = 0;
    float m_lookBlendSeconds = 6.0f;

    // Beat grid and song structure
    uint32_t m_lastGridBeat = 0, m_lastDropCount = 0;
    int m_gridBeatsSinceChange = 0;
    bool m_changePending = false;
    float m_lastBarPhase = 0.0f;
    float m_gridPulse = 0.0f;   // 1 exactly on each predicted beat, decays through the beat
    float m_dropFlash = 0.0f;
    float m_buildUp = 0.0f;

    // Album-art palette (replaces the built-in palettes while set)
    bool m_hasArt = false;
    TrackPalette m_art{};
    int m_artVariant = 0;

    // Motion
    double m_pos[3] = { 10.0, 20.0, 30.0 };
    float m_time = 0.0f;
    float m_speed = 0.4f;
    float m_rollDrift = 0.0f;
    float m_swirl = 0.0f;
    float m_basis[3][3] = {};   // right, up, forward

    // Audio-driven, smoothed
    uint32_t m_lastBeatCount = 0;
    float m_beatPulse = 0.0f;
    float m_zoom = 0.0f;   // smoothed beat pulse driving the field of view
    float m_shellAge = 10.0f;
    float m_shellPower = 0.0f;
    float m_exposure = 0.6f;
    float m_lightStrength = 0.6f;
    float m_bass = 0, m_mids = 0, m_treble = 0;
    bool m_silent = true;

    std::wstring m_message;
};
