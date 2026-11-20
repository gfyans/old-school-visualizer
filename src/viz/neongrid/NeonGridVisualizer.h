#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Neon Grid": a cyberpunk flight over a wireframe landscape made from the music.
//
//   spectrum     -> the terrain: each row is one moment's spectrum, entering at the horizon
//                   (bass beside the road, treble further out), so the song forms the mountains
//   beat (grid)  -> across-lines pulse, a bright ring rushes towards you, gentle camera bob
//   intensity    -> flight speed
//   build-up     -> faster, brighter horizon
//   drop         -> glitch tear + flash, new colour scheme
//   drums        -> strobing road lights (calmer drift for drumless music)
//   treble       -> twinkling stars; bass -> the sun swells
class NeonGridVisualizer : public IVisualizer {
public:
    NeonGridVisualizer();

    const wchar_t* Name() const override { return L"Neon Grid"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.75f; }   // thin lines want resolution
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Scheme { float skyTop[3], skyHorizon[3], sunTop[3], sunBottom[3], gridA[3], gridB[3], ground[3]; };

    static constexpr int kBands = 64;
    static constexpr int kRows = 128;

private:
    void NextScheme(float blendSeconds);
    Scheme CurrentScheme() const;
    void PushRow();

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_historyTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_history;

    std::mt19937 m_rng;
    int m_scheme = 0;
    Scheme m_from{}, m_to{};
    float m_blend = 1.0f, m_blendSeconds = 3.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // Terrain history
    float m_rowPeak[kBands] = {};   // loudest spectrum since the last row was pushed
    float m_lastRow[kBands] = {};
    double m_travel = 0.0;          // distance flown (wrapped)
    double m_nextRowAt = 0.0;
    int m_rowIndex = 0;

    // Motion and response
    float m_time = 0.0f, m_speed = 6.0f, m_bob = 0.0f, m_pulse = 0.0f, m_pulseZ = 100.0f;
    float m_buildUp = 0.0f, m_glitch = 0.0f, m_glitchSeed = 0.0f, m_flash = 0.0f;
    float m_bass = 0.0f, m_treble = 0.0f, m_drums = 0.0f;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0;
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;
    bool m_changePending = false;

    float m_constants[48] = {};
    std::wstring m_message;
};
