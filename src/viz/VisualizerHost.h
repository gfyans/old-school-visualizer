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

    void Select(int index, bool crossfade = true);
    void Next() { Select((m_current + 1) % Count()); }
    void Previous() { Select((m_current + Count() - 1) % Count()); }
    int Count() const { return static_cast<int>(m_slots.size()); }
    int Current() const { return m_current; }
    int FindByName(const std::wstring& name) const;
    IVisualizer& Active() { return *m_slots[m_current].viz; }
    const IVisualizer& Active() const { return *m_slots[m_current].viz; }

    void Update(const AudioFeatures& audio, float dt);
    void Render(Renderer& renderer, float time, float dt);

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
    static constexpr float kFadeSeconds = 1.2f;

private:
    struct Slot {
        std::unique_ptr<IVisualizer> viz;
        RenderTarget target;   // allocated at the visualiser's preferred scale
        GpuTimer timer;
        float scale = 1.0f;    // current dynamic scale (<= preferred)
        float adjustTimer = 0.0f;
    };

    void EnsureTarget(Slot& slot);
    void RenderSlot(Slot& slot, Renderer& renderer, float time, float dt);
    void AdaptScale(Slot& slot, float dt);
    CompositeLayer LayerFor(const Slot& slot) const;
    void ViewportSize(const Slot& slot, int& w, int& h) const;

    std::vector<Slot> m_slots;
    float m_gpuBudgetMs = kDefaultGpuBudgetMs;
    ID3D11Device* m_device = nullptr;
    int m_width = 1, m_height = 1;
    int m_current = 0;
    int m_previous = -1;   // fading out, or -1
    float m_fade = 1.0f;   // 0 -> 1 during a crossfade
};
