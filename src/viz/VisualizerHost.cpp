#include "viz/VisualizerHost.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {
// Float target: smooth gradients and headroom; the composite pass dithers down to 8-bit.
constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R11G11B10_FLOAT;
}

VisualizerHost::VisualizerHost() {
    wchar_t buf[32];
    if (GetEnvironmentVariableW(L"VISUALIZER_GPU_BUDGET_MS", buf, ARRAYSIZE(buf)) > 0) {
        const float v = static_cast<float>(_wtof(buf));
        if (v > 0.0f) m_gpuBudgetMs = v;
    }
}

void VisualizerHost::Add(std::unique_ptr<IVisualizer> viz) {
    Slot slot;
    slot.scale = viz->RenderScale();
    slot.viz = std::move(viz);
    m_slots.push_back(std::move(slot));
}

bool VisualizerHost::CreateResources(Renderer& renderer) {
    m_device = renderer.Device();
    m_width = std::max(1, renderer.Width());
    m_height = std::max(1, renderer.Height());
    bool ok = true;
    for (Slot& s : m_slots) {
        ok &= s.viz->CreateResources(m_device);
        s.timer.Create(m_device);   // optional: no timing just means no adaptive scaling
        s.target.Reset();
    }
    return ok;
}

void VisualizerHost::ReleaseResources() {
    for (Slot& s : m_slots) {
        s.viz->ReleaseResources();
        s.timer.Reset();
        s.target.Reset();
    }
    m_device = nullptr;
}

void VisualizerHost::Resize(int width, int height) {
    m_width = std::max(1, width);
    m_height = std::max(1, height);
    // Targets are reallocated lazily in EnsureTarget; drop the invisible ones now to free memory.
    for (int i = 0; i < Count(); ++i)
        if (i != m_current && i != m_previous) m_slots[i].target.Reset();
}

int VisualizerHost::FindByName(const std::wstring& name) const {
    for (int i = 0; i < Count(); ++i)
        if (name == m_slots[i].viz->Name()) return i;
    return -1;
}

void VisualizerHost::Select(int index, bool crossfade) {
    if (index < 0 || index >= Count() || index == m_current) return;
    m_previous = crossfade ? m_current : -1;
    m_current = index;
    m_fade = crossfade ? 0.0f : 1.0f;
    m_slots[m_current].viz->Activate();
}

void VisualizerHost::Update(const AudioFeatures& audio, float dt) {
    if (m_slots.empty()) return;
    if (m_previous >= 0) {
        m_fade += dt / kFadeSeconds;
        if (m_fade >= 1.0f) {
            m_fade = 1.0f;
            m_slots[m_previous].target.Reset();   // free its memory until it's shown again
            m_previous = -1;
        }
    }
    m_slots[m_current].viz->Update(audio, dt);
    if (m_previous >= 0) m_slots[m_previous].viz->Update(audio, dt);
}

void VisualizerHost::ViewportSize(const Slot& slot, int& w, int& h) const {
    w = std::max(1, static_cast<int>(std::lround(m_width * slot.scale)));
    h = std::max(1, static_cast<int>(std::lround(m_height * slot.scale)));
}

int VisualizerHost::ActiveRenderWidth() const { int w, h; ViewportSize(m_slots[m_current], w, h); return w; }
int VisualizerHost::ActiveRenderHeight() const { int w, h; ViewportSize(m_slots[m_current], w, h); return h; }

void VisualizerHost::EnsureTarget(Slot& slot) {
    // Allocate at the preferred scale; dynamic scaling renders into a sub-rectangle of it.
    const float pref = slot.viz->RenderScale();
    const int w = std::max(1, static_cast<int>(std::lround(m_width * pref)));
    const int h = std::max(1, static_cast<int>(std::lround(m_height * pref)));
    if (slot.target.width != w || slot.target.height != h)
        slot.target.Create(m_device, w, h, kTargetFormat);
}

void VisualizerHost::AdaptScale(Slot& slot, float dt) {
    slot.adjustTimer += dt;
    if (slot.adjustTimer < 0.5f || !slot.timer.HasValue()) return;
    slot.adjustTimer = 0.0f;

    // GPU cost scales with pixel count (scale^2): aim for ~70% of budget.
    const float pref = slot.viz->RenderScale();
    const float ms = slot.timer.Milliseconds();
    float next = slot.scale;
    if (ms > m_gpuBudgetMs) next = slot.scale * std::sqrt(0.7f * m_gpuBudgetMs / ms);
    else if (ms < 0.4f * m_gpuBudgetMs) next = slot.scale * 1.05f;
    slot.scale = std::clamp(next, std::min(kMinScale, pref), pref);
}

void VisualizerHost::RenderSlot(Slot& slot, Renderer& renderer, float time, float dt) {
    EnsureTarget(slot);
    if (!slot.target.rtv) return;
    int w, h;
    ViewportSize(slot, w, h);
    w = std::min(w, slot.target.width);
    h = std::min(h, slot.target.height);

    ID3D11DeviceContext* ctx = renderer.Context();
    slot.timer.Collect(ctx);
    AdaptScale(slot, dt);
    slot.timer.Begin(ctx);
    renderer.BindTarget(slot.target.rtv.Get(), w, h, time, dt);
    slot.viz->Render(renderer, time, dt);
    slot.timer.End(ctx);
}

CompositeLayer VisualizerHost::LayerFor(const Slot& slot) const {
    int w, h;
    ViewportSize(slot, w, h);
    const float tw = static_cast<float>(std::max(1, slot.target.width));
    const float th = static_cast<float>(std::max(1, slot.target.height));
    w = std::min(w, slot.target.width);
    h = std::min(h, slot.target.height);
    CompositeLayer layer;
    layer.srv = slot.target.srv.Get();
    layer.uvScale[0] = w / tw;
    layer.uvScale[1] = h / th;
    layer.uvMax[0] = (w - 0.5f) / tw;
    layer.uvMax[1] = (h - 0.5f) / th;
    return layer;
}

void VisualizerHost::Render(Renderer& renderer, float time, float dt) {
    if (m_slots.empty() || !m_device) return;
    Slot& cur = m_slots[m_current];
    RenderSlot(cur, renderer, time, dt);
    const CompositeLayer current = LayerFor(cur);

    if (m_previous >= 0) {
        Slot& prev = m_slots[m_previous];
        RenderSlot(prev, renderer, time, dt);
        const CompositeLayer previous = LayerFor(prev);
        renderer.Composite(previous, &current, m_fade);
    } else {
        renderer.Composite(current, nullptr, 0.0f);
    }
}
