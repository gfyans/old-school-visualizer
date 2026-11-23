#pragma once
#include <wrl/client.h>
#include <random>
#include <vector>
#include "viz/Visualizer.h"

// "Dot Field": a plane covered in dots, seen from an isometric angle, that bounce to the music.
// Every dot is a quad worked out in the vertex shader from its index (no per-dot buffers); the
// heights come from a short history of the spectrum, so waves can travel across the plane.
//
//   layouts     -> grid, scatter, rows, rings, spiral (M); dots glide from one to the next
//   motions     -> terrain (a spectrogram rolling away), equaliser (bass in the middle, treble
//                  at the rim); swaps by itself every other phrase, on splashes and new tracks
//   bar         -> a scan line sweeps across the plane
//   peaks       -> the highest dots fire data beams straight up (more in big sections)
//   build-up    -> the camera sinks to skim low across the dots, and springs back up on release
//   snares      -> in big sections, slices of the plane glitch sideways (RGB split)
//   hi-hats     -> a sprinkle of dots flickers
//   build-up    -> the backdrop and glow swell
//   drop        -> alternately splash (a shockwave throws every dot into the air; they bounce down
//                  into a new layout) and breach (red alert: the plane shatters into tiles that
//                  drift apart, then snap back together on a downbeat) (Q)
//   camera      -> slow isometric turn, eased 45-degree turns on phrases
//   keys        -> N colours, M layout, Q big hit, Space lock
class DotsVisualizer : public IVisualizer {
public:
    DotsVisualizer();

    const wchar_t* Name() const override { return L"Dot Field"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next colours" }, { 'M', L"Next layout" },
                 { 'Q', L"Big hit (splash / breach)" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    bool Busy() const override { return m_splashT >= 0.0f || m_breachT >= 0.0f; }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

    struct Palette { float base[3], mid[3], peak[3], bg[3]; };

    static constexpr int kHistCols = 66;    // 64 bands, bass, level
    static constexpr int kHistRows = 512;   // at 60 Hz: about 8.5 s
    static constexpr float kHistRate = 60.0f;

private:
    void NextColours(float seconds);
    void NextLayout(float seconds);
    void NextMotion(float seconds);
    void Splash(bool morph);
    void Breach();
    Palette CurrentPalette() const;
    bool CreateTarget(int w, int h);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_dotsVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_dotsPS, m_outPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_addBlend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linear, m_histSampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_histTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_histSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_tex;   // RGBA16F with mips (glow)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    int m_width = 0, m_height = 0;

    std::mt19937 m_rng;
    std::vector<float> m_hist;   // CPU copy of the history ring
    bool m_locked = false, m_hasArt = false;
    TrackPalette m_art{};
    int m_scheme = 0;
    Palette m_from{}, m_to{};
    float m_colourBlend = 1.0f, m_colourSeconds = 4.0f;
    int m_layoutA = 0, m_layoutB = 0;
    float m_morph = 1.0f, m_morphSeconds = 4.0f;
    int m_motionA = 0, m_motionB = 0;
    float m_motionBlend = 1.0f, m_motionSeconds = 3.0f;
    int m_phrases = 0;

    // Audio history (written at a fixed 60 Hz into a ring of rows).
    float m_smooth[kHistCols] = {};
    float m_histClock = 0.0f;
    int m_histRow = 0, m_rowsPending = 0;

    float m_splashT = -1.0f, m_flash = 0.0f;
    bool m_splashMorph = false, m_fireSplash = false;
    float m_yaw = 0.785398f, m_yawTarget = 0.785398f, m_yawDrift = 0.0f;
    float m_waveTime = 0.0f, m_hat = 0.0f, m_buildUp = 0.0f, m_glow = 1.0f;
    uint32_t m_hatSeed = 1, m_lastHat = 0, m_lastDrop = 0, m_lastGrid = 0, m_lastBeat = 0;
    int m_beatsSinceChange = 0, m_beatsSinceTurn = 0;
    float m_timeSinceChange = 0.0f, m_timeSinceTurn = 0.0f, m_lastBarPhase = 0.0f;
    bool m_silent = true;
    float m_intensity = 0.0f, m_scanPhase = 0.0f, m_scan = 0.0f, m_glitch = 0.0f, m_beams = 0.0f, m_skim = 0.0f;
    float m_breach = 0.0f, m_breachT = -1.0f, m_snapT = -1.0f, m_snapFrom = 0.0f;
    uint32_t m_breachSeed = 7;
    bool m_nextBreach = false;
    uint32_t m_glitchSeed = 1, m_lastSnare = 0;

    std::wstring m_message;
};
