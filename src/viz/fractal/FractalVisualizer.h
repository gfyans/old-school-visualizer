#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Fractal flights": flying forward through infinite raymarched fractals, down a winding
// tunnel carved along the flight path. Five worlds: Menger halls, Mandelbox, Apollonian foam,
// glowing coral and a vaulted cathedral.
//
//   bass        -> the fractal breathes and its veins pulse
//   beat (grid) -> a ring of light appears ahead and the flight passes through it, and bands
//                  of light chase down the walls, one per beat; kicks swell the speed gently
//   bars        -> the camera sways in time; every 4 bars (when intense) the world turns 45 degrees
//   mids        -> the world turns around the flight path
//   treble      -> glints sparkle on the surfaces
//   build-up    -> the flight accelerates and the glow rises
//   drop        -> hyperspeed, a barrel roll and a flash cut to a new world
//   phrases     -> a new world (hidden behind a flash), new colours
//   keys        -> N next world, Space lock, Q hyperspeed
class FractalVisualizer : public IVisualizer {
public:
    FractalVisualizer();

    const wchar_t* Name() const override { return L"Fractal flights"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.5f; }   // raymarching: the expensive one
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Palette { float albedo[3], emit[3], fog[3]; };

private:
    void NextWorld(float flash, bool barrelRoll);
    Palette CurrentPalette() const;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;

    std::mt19937 m_rng;
    int m_world = 0, m_scheme = 0;
    Palette m_from{}, m_to{};
    float m_blend = 1.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    float m_time = 0.0f, m_z = 0.0f, m_speed = 0.4f, m_spin = 0.0f, m_spinDir = 1.0f;
    float m_bass = 0.0f, m_mids = 0.0f, m_treble = 0.0f, m_buildUp = 0.0f, m_intensity = 0.0f;
    float m_kick = 0.0f, m_flash = 0.0f, m_hyper = 0.0f, m_roll = 0.0f, m_barrel = 1.0f, m_barrelDir = 1.0f;
    float m_pulses[4][2] = {};   // world z, strength
    int m_nextPulse = 0;
    bool m_silent = true, m_fireDrop = false;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0, m_bars = 0;
    float m_turn = 0.0f, m_turnTarget = 0.0f;   // eased 45-degree world turns on phrases
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;
    bool m_changePending = false;

    float m_constants[52] = {};
    std::wstring m_message;
};
