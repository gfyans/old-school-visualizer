#pragma once
#include <wrl/client.h>
#include <random>
#include "render/RenderTarget.h"
#include "viz/Visualizer.h"

// "Liquid": a Milkdrop-style feedback visualiser. Every frame the previous frame
// is warped (zoom / rotate / ripple / kaleidoscope), faded and hue-shifted, and
// fresh ink (a waveform ring or line, sparks) is drawn on top. The accumulated
// trails become tunnels and liquid light.
//
//   bass / beat  -> faster zoom (flying into the tunnel), brighter, thicker ink
//   mids         -> spin and ripple speed
//   treble       -> sparks and ripple strength
//   build-up     -> everything accelerates and spins harder
//   drop         -> flash, spin reverses, new preset
//   phrases      -> new preset every 8 bars (on the downbeat when the beat grid is locked)
class FeedbackVisualizer : public IVisualizer {
public:
    FeedbackVisualizer();

    const wchar_t* Name() const override { return L"Liquid"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.75f; }
    void Activate() override { m_clear = true; }
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Params {   // every field blends linearly between presets
        float zoom, rotation, warpAmp, warpFreq, decay, hueShift, mirror, inkShape, ringRadius, waveScale;
    };

private:
    void NextPreset(float blendSeconds);
    Params Current() const;
    void InkColours(float a[3], float b[3]) const;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_warpPS, m_outPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_border, m_linear;
    RenderTarget m_buffers[2];   // ping-pong feedback buffers
    int m_current = 0;
    bool m_clear = true;

    std::mt19937 m_rng;
    int m_preset = 0;
    Params m_from{}, m_to{};
    float m_blend = 1.0f, m_blendSeconds = 3.0f;
    bool m_locked = false;
    float m_timeSinceChange = 0.0f;
    int m_beatsSinceChange = 0;
    bool m_changePending = false;
    float m_lastBarPhase = 0.0f;
    uint32_t m_lastBeat = 0, m_lastGridBeat = 0, m_lastDrop = 0;

    float m_time = 0.0f;
    float m_hue = 0.0f;
    float m_rotationDir = 1.0f;
    float m_warpPhase = 0.0f;
    float m_beatPulse = 0.0f, m_pulse = 0.0f, m_flash = 0.0f, m_buildUp = 0.0f;
    float m_bass = 0.0f, m_mids = 0.0f, m_treble = 0.0f;
    bool m_silent = true;

    bool m_hasArt = false;
    TrackPalette m_art{};

    float m_constants[24] = {};   // FeedbackConstants for this frame, built in Update
    std::wstring m_message;
};
