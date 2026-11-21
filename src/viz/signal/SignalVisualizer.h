#pragma once
#include <wrl/client.h>
#include <random>
#include "render/RenderTarget.h"
#include "viz/Visualizer.h"

// "Signal": 3D subjects (a cyber skull model, a lattice brain, a monolith) turning in
// the dark, drawn as cyberpunk glitch art: ASCII glyphs, hard-palette dithering, LED cells or torn
// scan lines. Shown one in the centre, or as a scrolling grid of them filling the screen.
//
//   vocals/bass -> the skull nods (its jaw works)   kick       -> the subject pulses; ripples across a grid
//   snare       -> rows tear, eye sockets flare     hi-hats    -> glyphs flicker to 0s and 1s
//   build-up    -> the picture dissolves cell by cell and tears harder
//   drop        -> the palette flashes inside out, the screen floods with a grid, the subject changes
//   phrases     -> new style and palette, centre <-> grid
//   keys        -> N next look, M centre / grid, Space lock
//
// The skull model is embedded from assets/cyberskull.mesh when the exe is built with it (it's a
// third-party asset, kept out of git); otherwise the skull is a distance field.
class SignalVisualizer : public IVisualizer {
public:
    SignalVisualizer();

    const wchar_t* Name() const override { return L"Signal"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 1.0f; }   // crisp cells; one sample per cell keeps it cheap
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

private:
    void NextLook(bool newSubject, int rows = -1);   // rows: force a layout
    void SetLayout(int rows);
    bool LoadModel(ID3D11Device* device);
    bool EnsureSceneTargets(ID3D11Device* device, int w, int h);
    void DrawModel(Renderer& renderer, int slot, int rows, int w, int h, float time, float dt);

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;

    // The skull model (absent when the exe was built without it).
    bool m_hasModel = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_meshVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_meshPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_meshLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_meshVB, m_meshIB, m_instanceCB;
    UINT m_indexCount = 0;
    DXGI_FORMAT m_indexFormat = DXGI_FORMAT_R16_UINT;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    RenderTarget m_scene[2];   // the model in the current layout, and the previous one during a wipe
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;

    std::mt19937 m_rng;
    int m_style = 2, m_palette = 0, m_subject = 0, m_nextSubject = 0;
    float m_morph = 0.0f;          // 0..1 while morphing to m_nextSubject
    int m_rows = 0, m_prevRows = 0;   // layout: 0 one subject in the centre, else grid rows to the screen
    float m_layoutMix = 1.0f;      // 0..1 while wiping from m_prevRows to m_rows
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    float m_time = 0.0f, m_spin = 0.0f, m_scroll = 0.0f;
    float m_jaw = 0.0f, m_pulse = 0.0f, m_glitch = 0.0f, m_eyes = 0.0f, m_flash = 0.0f, m_dissolve = 0.0f, m_rain = 0.5f;
    float m_beatAge = 10.0f, m_beatHit = 0.0f;
    float m_yaw = 0.0f, m_pitch = 0.0f;
    uint32_t m_lastGrid = 0, m_lastBeat = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0;
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;

    float m_constants[40] = {};
    std::wstring m_message;
};
