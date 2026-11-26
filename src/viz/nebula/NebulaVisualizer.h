#pragma once
#include <wrl/client.h>
#include <random>
#include "render/RenderTarget.h"
#include "viz/Visualizer.h"

// Flying through a glowing gas nebula.
//
// Music response (all smoothed here on the CPU, then passed to nebula_ps.hlsl):
//   bass      -> denser, brighter gas; the light core swells
//   mids      -> more turbulence and faster churning of the gas
//   treble    -> stars twinkle brighter, sparkles in the gas
//   beat      -> shockwave shell from the core, brightness kick, slight zoom swell
//   intensity -> flight speed and overall brightness (calm sections drift, loud ones rush)
//   sections  -> every ~32 beats, or on a sudden "drop", blends to a new look
//                (palette + gas shape) over a few seconds
//   phrases   -> the flight swoops to a new heading; sometimes a pulsar appears, its
//                lighthouse beams sweeping through the gas in time with the bar
//   build-up  -> the flight dives towards the core, light beams intensify
//   drop      -> a supernova ahead: blinding flash, lens flare, and a shockwave that lights
//                the gas as the flight rushes through it
// Light scattering (lit rims, dark undersides, god rays) and bloom make it all glow.
class NebulaVisualizer : public IVisualizer {
public:
    NebulaVisualizer();

    const wchar_t* Name() const override { return L"Nebula"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.75f; }   // TAA jitter recovers sub-pixel detail
    void Activate() override;
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next look" }, { 'Q', L"Supernova" }, { 'P', L"Summon a pulsar" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    bool Busy() const override { return m_nova.active && m_nova.age < 5.0f; }
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
    float m_noiseRedRange = 0.5f;   // how far the noise volume's red channel strays from 0.5

    // Temporal accumulation (TAA): jittered raymarch -> resolve with reprojected history.
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_taaPS, m_postPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_taaCB, m_postCB;
    RenderTarget m_resolved;     // TAA output with mips, for bloom
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_clamp;
    RenderTarget m_current;      // this frame's raymarch: colour + gas distance
    RenderTarget m_history[2];   // ping-pong accumulated image
    int m_historyIndex = 0;
    bool m_historyValid = false;
    bool m_taaEnabled = true;
    uint32_t m_frame = 0;
    float m_prevBasis[3][3] = {};
    double m_prevPos[3] = {};
    float m_prevTanHalfFov = 1.0f;

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

    // Directed flight: swoops to a new heading every phrase
    float m_yaw = 0.0f, m_pitch = 0.0f, m_yawTarget = 0.0f, m_pitchTarget = 0.0f, m_bank = 0.0f;
    int m_gridBeatsSinceTurn = 0;
    float m_timeSinceTurn = 0.0f;
    float m_hyper = 0.0f;   // drop: rushing through the blast

    // Events (positions in the wrapped world; used relative to the camera)
    struct Pulsar { bool active = false; double pos[3] = {}; float axisU[3] = {}, axisV[3] = {}; float spin = 0.0f, fade = 0.0f, age = 0.0f; };
    struct Supernova { bool active = false; double pos[3] = {}; float age = 0.0f; };
    Pulsar m_pulsar;
    Supernova m_nova;
    float m_intensity = 0.0f;
    void SpawnPulsar();
    void Relative(const double world[3], float out[3]) const;   // world point minus the camera, the short way round

    std::wstring m_message;
};
