#pragma once
#include <wrl/client.h>
#include <random>
#include "viz/Visualizer.h"

// "Detonation": a vast procedural city at night around a central spire, and the blast that swallows it.
//
//   kick      -> a ring of light runs out from the spire across the rooftops
//   build-up  -> an orb charges on the spire, a light column rises, clouds spiral faster, windows stutter
//   drop      -> a white dome erupts, tearing at buildings on its rim, a shockwave sweeps the streets and
//                punches a ring through the clouds; the dome inverts to a black sphere with a burning rim,
//                holds while the song stays big, then collapses, leaving a crater the city rebuilds
//   phrases   -> the camera cuts between an orbit, a flight down an avenue, a rooftop skim, an overhead
//                view and a distant witness (where it cuts to on a drop)
//   keys      -> N next look (Night, Ink, Dusk), M next shot, Q detonate, Space lock
//
// An original take on the classic anime "city-swallowing dome", not a recreation of any film.
class DetonationVisualizer : public IVisualizer {
public:
    DetonationVisualizer();

    const wchar_t* Name() const override { return L"Detonation"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override;
    float RenderScale() const override { return 0.8f; }
    void Update(const AudioFeatures& audio, float dt) override;
    void Render(Renderer& renderer, float time, float dt) override;
    bool OnKey(UINT vk) override;
    std::vector<VisualizerAction> Actions() const override {
        return { { 'N', L"Next look (Night, Ink, Dusk)" }, { 'M', L"Next camera shot" }, { 'Q', L"Detonate" }, { VK_SPACE, L"Lock / unlock the look" } };
    }
    bool Busy() const override { return m_phase == Phase::Blast || m_phase == Phase::Collapse || (m_phase == Phase::Void && m_phaseTime < 6.0f); }
    std::wstring TakeMessage() override;
    std::wstring Status() const override;
    void AccentColours(float lo[3], float hi[3]) const override;
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) override;

private:
    enum class Phase { Calm, Blast, Void, Collapse };
    enum Shot { Orbit, Avenue, Rooftops, Overhead, Witness, kShotCount };

    void Detonate();
    void Cut(int shot);
    void PickShot();
    void Camera(float out[12]) const;   // position, right, up, forward

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    std::mt19937 m_rng;

    int m_look = 0;
    bool m_locked = false;
    bool m_hasArt = false;
    TrackPalette m_art{};

    // The blast.
    Phase m_phase = Phase::Calm;
    float m_phaseTime = 0.0f;
    float m_domeR = 0.0f, m_domeStart = 0.0f, m_domeMax = 30.0f, m_domeWhite = 1.0f;
    float m_shockR = 0.0f, m_cloudRing = 0.0f;
    float m_ruinR = 0.0f, m_regrow = 1.0f;
    float m_charge = 0.0f, m_flash = 0.0f, m_shake = 0.0f;
    float m_swirl = 0.0f, m_intensity = 0.0f, m_pulse = 0.0f;
    float m_ringAge[4] = { 99, 99, 99, 99 }, m_ringHit[4] = {};
    float m_kick = 0.0f;          // the last kick's strength, decaying
    float m_kickSmooth = 0.0f;    // the dome's kick envelope: rises fast (not a jump), eases off
    float m_energy = 0.0f;        // smoothed bass energy
    float m_sinceBlast = 0.0f;    // seconds since the last blast started
    float m_peakCharge = 0.0f;    // the highest charge of the current build-up
    int m_phraseBeats = 0;        // grid beats since the last blast (for phrase-timed blasts)

    // The camera director.
    int m_shot = Orbit;
    float m_shotTime = 0.0f;
    float m_seedA = 0.0f, m_seedB = 0.0f;   // per-shot randomness (angles, which avenue)
    float m_camTime = 0.0f;                 // shot time scaled by tempo
    float m_pullBack = 0.0f;                // the orbit widens while the dome is up

    float m_time = 0.0f;
    float m_freeze = -1.0f;   // developer switch: seconds into a blast to hold
    uint32_t m_lastBeat = 0, m_lastGrid = 0, m_lastDrop = 0;
    int m_beatsInShot = 0;
    float m_lastBarPhase = 0.0f;
    float m_constants[44] = {};
    std::wstring m_message;
};
