#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Spectrum": the hi-fi scene. A classic 64-band analyser, an honest readout of the music, shown two ways:
//
//   Studio    -> glossy 3D bars on a mirror-black floor in a dark room, light spilling from each bar,
//                with peak caps that fall under gravity and bounce; the camera moves between shots
//   Radial    -> the spectrum bent into a ring round a glowing core (bass at the bottom, treble at the
//                top, mirrored); every kick sends an echo of the ring rippling outward
//
//   kick      -> the bass bars thump and the light swells
//   build-up  -> the bars brighten and tighten, the peak caps hover and jitter
//   drop      -> every peak cap launches like a firework and bursts into sparks (the ring bursts outward)
//   keys      -> N next look, M next shot (Studio) or colours, Q fireworks, Space lock
//
// Bars, caps and sparks are simulated once here in "bar space" (x = band 0..64, y = height, z = depth or
// side) and each look maps that space onto its own picture.
class SpectrumVisualizer : public IVisualizer {
public:
    SpectrumVisualizer();

    const wchar_t* Name() const override { return L"Spectrum"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 1.0f; }
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next look (Studio, Radial)" }, { 'M', L"Next shot / colours" }, { 'Q', L"Fireworks" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    bool Busy() const override { return m_fireworks > 0.0f; }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    static constexpr int kBars = 64;
    static constexpr int kEchoes = 8;
    static constexpr int kSparks = 128;

private:
    enum Look { Studio, Radial, kLookCount };
    enum Shot { Wide, Low, Hero, High, Orbit, kShotCount };

    void SetLook(int look);
    void Fireworks();
    void Spark(float x, float y, float z, float vx, float vy, float vz, float life);
    void Camera(float out[12]) const;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    std::mt19937 m_rng;

    int m_look = Studio;
    int m_shot = Wide;
    int m_palette = 0;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // The bars and their peak caps.
    float m_bar[kBars] = {};
    float m_cap[kBars] = {}, m_capV[kBars] = {}, m_capHold[kBars] = {};
    bool m_rocket[kBars] = {};   // launched by a drop: bursts at the top of its flight

    // Echoes of the ring (Radial): a snapshot of the bars on each kick, and how long ago.
    float m_echo[kEchoes][kBars] = {};
    float m_echoAge[kEchoes] = {};
    int m_nextEcho = 0;

    struct Particle { float x, y, z, vx, vy, vz, life, maxLife; };
    Particle m_sparks[kSparks] = {};
    int m_sparkCount = 0;

    float m_time = 0.0f, m_shotTime = 0.0f;
    float m_kick = 0.0f, m_kickSmooth = 0.0f, m_flash = 0.0f, m_fireworks = 0.0f;
    float m_buildUp = 0.0f, m_intensity = 0.0f, m_rotation = 0.0f;
    float m_seed = 0.0f;
    uint32_t m_lastBeat = 0, m_lastDrop = 0, m_lastGrid = 0;
    int m_beatsInShot = 0;
    float m_lastBarPhase = 0.0f;
    std::wstring m_message;

    struct Constants;
    std::vector<float> m_constants;
};
