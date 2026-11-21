#pragma once
#include <memory>
#include <string>
#include <vector>

#include "render/GpuTimer.h"
#include "render/RenderTarget.h"
#include "render/Renderer.h"
#include "viz/Visualizer.h"

// Owns the registered visualisers and decides what is on screen: renders the
// active one (plus the outgoing one during a crossfade) into offscreen targets,
// adapts their resolution to a GPU-time budget, and composites to the window.
class VisualizerHost {
public:
    void Add(std::unique_ptr<IVisualizer> viz);

    bool CreateResources(Renderer& renderer);
    void ReleaseResources();
    void Resize(int width, int height);

    // Switch visualiser; with `animate`, plays a randomly chosen transition.
    void Select(int index, bool animate = true);
    void Next() { Select((m_current + 1) % Count()); }
    void Previous() { Select((m_current + Count() - 1) % Count()); }
    void SelectRandom();   // any visualiser other than the current one (auto-cycle)
    int Count() const { return static_cast<int>(m_slots.size()); }
    int Current() const { return m_current; }
    int FindByName(const std::wstring& name) const;
    IVisualizer& Active() { return *m_slots[m_current].viz; }
    IVisualizer& At(int index) { return *m_slots[index].viz; }
    const IVisualizer& Active() const { return *m_slots[m_current].viz; }

    void Update(const AudioFeatures& audio, float dt);
    // Album colours for every visualiser (nullptr = none). newTrack: the song changed.
    void SetTrackPalette(const TrackPalette* palette, bool newTrack) {
        for (Slot& s : m_slots) s.viz->SetTrackPalette(palette, newTrack);
    }
    void Render(Renderer& renderer, float time, float dt);

    // Multi-monitor: one region of the back buffer per monitor (client pixels). Region 0 shows the
    // active visualiser (with transitions); each other region gets a different one. Empty = the
    // whole window shows the active visualiser.
    void SetRegions(const std::vector<RECT>& regions);
    bool HasRegions() const { return !m_regions.empty(); }
    RECT PrimaryRegion() const;   // where the active visualiser is (the whole window without regions)
    void ShuffleExtras();         // new visualisers on the other monitors (e.g. on a new song)

    // Stats for the active visualiser.
    float ActiveScale() const { return m_slots[m_current].scale; }
    float ActiveGpuMs() const { return m_slots[m_current].timer.Milliseconds(); }
    int ActiveRenderWidth() const;
    int ActiveRenderHeight() const;

    VisualizerHost();

    // Never spend more than this much GPU time per frame on one visualiser
    // (override with the VISUALIZER_GPU_BUDGET_MS environment variable, for testing).
    static constexpr float kDefaultGpuBudgetMs = 3.0f;
    static constexpr float kMinScale = 0.3f;
    static constexpr int kTransitionStyles = 5;   // see Renderer::Composite

private:
    struct Slot {
        std::unique_ptr<IVisualizer> viz;
        RenderTarget target;   // allocated at the visualiser's preferred scale
        GpuTimer timer;
        float scale = 1.0f;    // current dynamic scale (<= preferred)
        float adjustTimer = 0.0f;
        int baseW = 0, baseH = 0;  // size of the area it's shown in (0 = the whole window)
    };

    void EnsureTarget(Slot& slot);
    void RenderSlot(Slot& slot, Renderer& renderer, float time, float dt);
    void AdaptScale(Slot& slot, float dt);
    CompositeLayer LayerFor(const Slot& slot) const;
    void ViewportSize(const Slot& slot, int& w, int& h) const;

    std::vector<Slot> m_slots;
    std::vector<RECT> m_regions;
    std::vector<int> m_extra;   // visualiser shown in region i + 1
    bool IsShown(int index) const;
    void SetBase(Slot& slot, const RECT* region);
    float m_gpuBudgetMs = kDefaultGpuBudgetMs;
    ID3D11Device* m_device = nullptr;
    int m_width = 1, m_height = 1;
    int m_current = 0;
    int m_previous = -1;   // fading out, or -1
    float m_fade = 1.0f;   // 0 -> 1 during a transition
    float m_fadeSeconds = 1.2f;
    int m_style = 0;
    float m_seed = 0.0f;
    uint32_t m_rng = 0x9E3779B9u;
    uint32_t Random();
};
