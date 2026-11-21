#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Aurora": soft ribbons of northern lights over a frozen forest lake at night. The calm one.
//
//   mids        -> curtains sway and fold
//   beat (grid) -> a flare travels along the curtains
//   bass        -> brighter curtains, ripples on the lake
//   treble      -> stars twinkle
//   build-up    -> curtains climb higher and brighten
//   drop        -> a surge of light and a new colour scheme
//   silence     -> a slow, dim drift
class AuroraVisualizer : public IVisualizer {
public:
    AuroraVisualizer();

    const wchar_t* Name() const override { return L"Aurora"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.6f; }   // soft light: upscales cleanly
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next colours" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Scheme { float low[3], high[3], skyTop[3], skyHorizon[3], land[3]; };

private:
    void NextScheme(float blendSeconds);
    Scheme CurrentScheme() const;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;

    std::mt19937 m_rng;
    int m_scheme = 0;
    Scheme m_from{}, m_to{};
    float m_blend = 1.0f, m_blendSeconds = 6.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    float m_time = 0.0f, m_anim = 0.0f;
    float m_brightness = 0.6f, m_sway = 1.0f, m_height = 1.0f, m_ripple = 0.3f;
    float m_flare = 0.0f, m_flareX = 0.0f, m_flareSpeed = 8.0f, m_surge = 0.0f;
    float m_mids = 0.0f, m_bass = 0.0f, m_treble = 0.0f, m_buildUp = 0.0f;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0;
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;
    bool m_changePending = false;

    float m_constants[28] = {};
    std::wstring m_message;
};
