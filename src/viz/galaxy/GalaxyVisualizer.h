#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Galaxy": about a million stars swirling around a black hole. Each particle works out
// its own orbit in the vertex shader (no particle buffers, no simulation), drawn as additive
// points over the galaxy's smooth light; a final pass bends it all around the black hole
// (lensing, Einstein ring) and adds bloom.
//
//   camera      -> a director picks shots (wide orbit, flyby through the arms, dive to the
//                  black hole, top-down spin, edge-on skim) and changes them on downbeats,
//                  more often when the music is intense; kicks punch the zoom and shake it
//   bass        -> gravity: faster spin, the galaxy pulls in, the accretion disc burns hotter
//   beat (grid) -> a shockwave races out through the arms, flinging stars out and lighting them
//   treble      -> bright stars twinkle
//   build-up    -> the camera pushes in and spins faster, the jets start to stir
//   drop        -> hard cut, the black hole fires a quasar: twin beams, flare, flash and a huge
//                  shockwave; really big beats in intense sections fire smaller bursts
//   keys        -> N next look, Space lock, M next shot, Q fire the quasar
//   phrases     -> new look: arm count, winding, bulge and colours morph smoothly
class GalaxyVisualizer : public IVisualizer {
public:
    GalaxyVisualizer();

    const wchar_t* Name() const override { return L"Galaxy"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 1.0f; }   // pin-sharp stars
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Palette { float core[3], arm[3], knot[3], disc[3], sky[3]; };
    struct Shape { float arms, twist, width, bulge, thickness, elevation; };
    // A camera shot: distance from its target, elevation above the disc, orbit speed,
    // focal length, roll, and how far the target sits to the side of the black hole.
    struct Shot { float dist, elev, yawSpeed, focal, roll, side; };
    static constexpr unsigned kParticles = 1u << 20;   // stars (kStarCount in galaxy.hlsli)
    static constexpr unsigned kDust = 1u << 18;        // dust lane particles

private:
    void NextLook(float blendSeconds);
    void NextShot(int shot, bool cut);
    Palette CurrentPalette() const;
    bool CreateTarget(int w, int h);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_starsVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_starsPS, m_outPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_addBlend, m_subBlend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_tex;              // RGBA16F with mips (bloom)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    int m_width = 0, m_height = 0;

    std::mt19937 m_rng;
    int m_scheme = 0, m_shape = 0;
    Palette m_from{}, m_to{};
    Shape m_shapeA{}, m_shapeB{};
    float m_blend = 1.0f, m_blendSeconds = 6.0f;
    float m_shapeBlend = 1.0f, m_shapeSeconds = 8.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // Camera director
    int m_shot = 0, m_shotBeats = 0;
    float m_shotTime = 0.0f, m_camTau = 2.5f, m_elevSign = 1.0f;
    Shot m_cam{ 1.55f, 0.5f, 0.07f, 1.6f, 0.0f, 0.0f };        // current (smoothed towards the shot)
    float m_kick = 0.0f;
    bool m_fireQuasar = false;
    uint32_t m_lastSnare = 0;
    float m_vocal = 0.0f;

    float m_time = 0.0f, m_anim = 0.0f, m_pattern = 0.0f, m_accSpin = 0.0f, m_yaw = 0.0f;
    float m_gravity = 0.0f, m_treble = 0.0f, m_buildUp = 0.0f, m_beatGlow = 0.0f, m_intensity = 0.0f;
    float m_quasar = 0.0f, m_quasarCooldown = 0.0f, m_flash = 0.0f;
    float m_waves[4][2] = {};   // front radius, strength
    int m_nextWave = 0;
    bool m_silent = true;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsSinceChange = 0;
    float m_timeSinceChange = 0.0f, m_lastBarPhase = 0.0f;
    bool m_changePending = false;

    std::wstring m_message;
};
