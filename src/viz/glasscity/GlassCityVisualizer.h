#pragma once
#include <wrl/client.h>
#include <random>
#include "render/RenderTarget.h"
#include "viz/Visualizer.h"

// "Glass City": flying through an endless cyberspace city of translucent glass towers, with glowing
// edges, walls of glass tiles, currents of light running up circuit traces and a mirror-black floor.
//
//   bands       -> each tower listens to its own band: its tiles light up and its edges glow with it
//   beat (grid) -> a ring of light spreads from ahead across the floor and up the towers
//   snare       -> a share of the tiles refresh (new patterns)
//   hi-hat      -> glints along the edges
//   intensity   -> faster flight, more lit tiles, busier currents, more light streaks
//   bars        -> a gentle sway; the Weave shot slaloms in time
//   build-up    -> the flight accelerates, tiles stutter, the vanishing glow swells
//   drop        -> Overdrive: a flash, hyperspeed with a tunnel of streaks, a shockwave that sinks the
//                  city and rezzes it back up behind it, new colours and a dive down an avenue; the city
//                  stays supercharged while the song stays big, rolling 45, 90 or 180 degrees every two
//                  bars (landing on the downbeat), then levels out
//   phrases     -> the camera eases between shots (avenue, skim, rooftops, crown, weave), hopping
//                  avenues from up high; in big sections it sometimes tilts 45 degrees
//   keys        -> N next look, M next shot, Q Overdrive, Space lock
class GlassCityVisualizer : public IVisualizer {
public:
    GlassCityVisualizer();

    const wchar_t* Name() const override { return L"Glass City"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    // Traced at kTraceScale of this and upscaled temporally, so full resolution is affordable.
    float RenderScale() const override { return 1.0f; }
    void Activate() override { m_historyValid = false; }   // don't reproject a stale image
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next look" }, { 'M', L"Next camera shot" }, { 'Q', L"Overdrive" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    bool Busy() const override { return m_hyper > 0.15f; }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Palette { float edge[3], tile[3], spark[3], floor[3], fog[3], glass[3]; };

private:
    enum Shot { Avenue, Skim, Rooftops, Crown, Weave, kShotCount };

    void Overdrive();
    void Turn(float radians, float seconds);
    void SetShot(int shot);
    void PickShot();
    void NextLook(bool instant);
    Palette MakePalette(int look) const;
    Palette CurrentPalette() const;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps, m_postPs, m_taaPs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb, m_postCb, m_taaCb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_clamp;
    // Temporal upscaling: jittered trace (smaller) -> resolve against reprojected history (full size).
    RenderTarget m_trace;        // HDR + distance to the first glass
    RenderTarget m_resolved;     // full size, with mips for the bloom
    RenderTarget m_history[2];   // ping-pong accumulated image
    int m_historyIndex = 0;
    bool m_historyValid = false;
    bool m_taaEnabled = true;    // VISUALIZER_TAA=0: trace at full size, no accumulation (comparisons)
    uint32_t m_frame = 0;
    float m_camPos[3] = {}, m_prevPos[3] = {};
    float m_prevBasis[3][3] = {}, m_prevFocal = 1.25f;
    std::mt19937 m_rng;

    int m_look = 0;
    Palette m_from{}, m_to{};
    float m_blend = 1.0f;
    float m_ceiling = 0.0f, m_ceilingTarget = 0.0f;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // The flight.
    float m_time = 0.0f, m_z = 0.0f, m_speed = 2.0f;
    float m_x = 1.0f, m_h = 0.6f, m_pitch = 0.0f, m_roll = 0.0f, m_yaw = 0.0f;
    int m_lane = 0, m_pendingLane = 0;
    int m_shot = Avenue, m_side = 1;
    float m_shotTime = 0.0f, m_snap = 0.0f;
    float m_streakPhase = 0.0f;
    // Eased camera rolls (45, 90, 180 degrees), landing on the beat.
    float m_turn = 0.0f, m_turnFrom = 0.0f, m_turnTo = 0.0f, m_turnT = 1.0f, m_turnDur = 1.0f;
    int m_halfBars = 0;

    // The music.
    float m_intensity = 0.0f, m_bass = 0.0f, m_buildUp = 0.0f, m_hat = 0.0f, m_vocal = 0.0f;
    float m_hyper = 0.0f, m_flash = 0.0f, m_aberr = 0.0f, m_over = 0.0f;
    float m_shockR = 0.0f, m_shockX = 0.0f, m_shockZ = 0.0f;
    float m_ringAge[4] = { 99, 99, 99, 99 }, m_ringHit[4] = {}, m_ringZ[4] = {};
    float m_sinceOver = 0.0f, m_peakBuild = 0.0f, m_lastBarPhase = 0.0f;
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0, m_lastSnare = 0, m_tileSeed = 0;
    int m_beatsInShot = 0;
    bool m_fire = false, m_silent = true;

    float m_constants[64] = {};
    float m_post[12] = {};
    float m_camFwd[3] = { 0, 0, 1 }, m_camRight[3] = { 1, 0, 0 }, m_camUp[3] = { 0, 1, 0 }, m_focal = 1.25f;
    std::wstring m_message;
};
