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
    m_rng ^= static_cast<uint32_t>(GetTickCount64());
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
        if (!IsShown(i)) m_slots[i].target.Reset();
}

bool VisualizerHost::IsShown(int index) const {
    return index == m_current || index == m_previous ||
           std::find(m_extra.begin(), m_extra.end(), index) != m_extra.end();
}

void VisualizerHost::SetRegions(const std::vector<RECT>& regions) {
    m_regions = regions;
    std::vector<int> old;
    old.swap(m_extra);
    for (size_t i = 1; i < m_regions.size(); ++i) {
        // Keep what this monitor showed before if possible, otherwise the next unused visualiser.
        int pick = i - 1 < old.size() ? old[i - 1] : -1;
        if (pick < 0 || pick == m_current || pick == m_previous ||
            std::find(m_extra.begin(), m_extra.end(), pick) != m_extra.end()) {
            pick = -1;
            for (int k = 1; k <= Count() && pick < 0; ++k) {
                const int c = (m_current + static_cast<int>(i) * 3 + k) % Count();
                if (c != m_current && c != m_previous && std::find(m_extra.begin(), m_extra.end(), c) == m_extra.end()) pick = c;
            }
        }
        if (pick < 0) break;   // more monitors than visualisers: the rest stay black
        const bool wasShown = IsShown(pick);
        m_extra.push_back(pick);
        if (!wasShown) m_slots[pick].viz->Activate();
    }
    for (int i = 0; i < Count(); ++i)
        if (!IsShown(i)) m_slots[i].target.Reset();
}

void VisualizerHost::ShuffleExtras() {
    if (m_extra.empty()) return;
    std::vector<int> pool;
    for (int i = 0; i < Count(); ++i) if (i != m_current && i != m_previous) pool.push_back(i);
    for (size_t i = pool.size(); i > 1; --i) std::swap(pool[i - 1], pool[Random() % i]);
    for (size_t i = 0; i < m_extra.size() && i < pool.size(); ++i) {
        if (std::find(m_extra.begin(), m_extra.end(), pool[i]) == m_extra.end()) m_slots[pool[i]].viz->Activate();
        m_extra[i] = pool[i];
    }
    for (int i = 0; i < Count(); ++i)
        if (!IsShown(i)) m_slots[i].target.Reset();
}

RECT VisualizerHost::PrimaryRegion() const {
    if (!m_regions.empty()) return m_regions[0];
    return RECT{ 0, 0, m_width, m_height };
}

void VisualizerHost::SetBase(Slot& slot, const RECT* region) {
    slot.baseW = region ? std::max(1, static_cast<int>(region->right - region->left)) : 0;
    slot.baseH = region ? std::max(1, static_cast<int>(region->bottom - region->top)) : 0;
}

int VisualizerHost::FindByName(const std::wstring& name) const {
    for (int i = 0; i < Count(); ++i)
        if (name == m_slots[i].viz->Name()) return i;
    return -1;
}

uint32_t VisualizerHost::Random() {   // xorshift: plenty for picking transitions
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

void VisualizerHost::Select(int index, bool animate) {
    if (index < 0 || index >= Count() || index == m_current) return;
    // Already on another monitor: swap the two (no transition, or it would be drawn twice).
    for (int& e : m_extra) {
        if (e != index) continue;
        e = m_current;
        animate = false;
    }
    m_previous = animate ? m_current : -1;
    m_current = index;
    m_fade = animate ? 0.0f : 1.0f;
    if (animate) {
        // A different transition each time; each style has its own natural length.
        static constexpr float kSeconds[kTransitionStyles] = { 1.2f, 0.7f, 1.0f, 1.1f, 1.0f };
        m_style = (m_style + 1 + static_cast<int>(Random() % (kTransitionStyles - 1))) % kTransitionStyles;
        m_fadeSeconds = kSeconds[m_style];
        m_seed = (Random() & 0xFFFF) / 65536.0f;
    }
    m_slots[m_current].viz->Activate();
}

void VisualizerHost::SelectRandom() {
    if (Count() < 2) return;
    // Prefer one that isn't already on another monitor.
    for (int tries = 0; tries < 16; ++tries) {
        const int c = (m_current + 1 + static_cast<int>(Random() % (Count() - 1))) % Count();
        if (std::find(m_extra.begin(), m_extra.end(), c) == m_extra.end() || tries == 15) { Select(c); return; }
    }
}

void VisualizerHost::Update(const AudioFeatures& audio, float dt) {
    if (m_slots.empty()) return;
    if (m_previous >= 0) {
        m_fade += dt / m_fadeSeconds;
        if (m_fade >= 1.0f) {
            m_fade = 1.0f;
            m_slots[m_previous].target.Reset();   // free its memory until it's shown again
            m_previous = -1;
        }
    }
    m_slots[m_current].viz->Update(audio, dt);
    if (m_previous >= 0) m_slots[m_previous].viz->Update(audio, dt);
    for (int e : m_extra) m_slots[e].viz->Update(audio, dt);
}

void VisualizerHost::ViewportSize(const Slot& slot, int& w, int& h) const {
    const int bw = slot.baseW > 0 ? slot.baseW : m_width, bh = slot.baseH > 0 ? slot.baseH : m_height;
    w = std::max(1, static_cast<int>(std::lround(bw * slot.scale)));
    h = std::max(1, static_cast<int>(std::lround(bh * slot.scale)));
}

int VisualizerHost::ActiveRenderWidth() const { int w, h; ViewportSize(m_slots[m_current], w, h); return w; }
int VisualizerHost::ActiveRenderHeight() const { int w, h; ViewportSize(m_slots[m_current], w, h); return h; }

void VisualizerHost::EnsureTarget(Slot& slot) {
    // Allocate at the preferred scale; dynamic scaling renders into a sub-rectangle of it.
    const float pref = slot.viz->RenderScale();
    const int bw = slot.baseW > 0 ? slot.baseW : m_width, bh = slot.baseH > 0 ? slot.baseH : m_height;
    const int w = std::max(1, static_cast<int>(std::lround(bw * pref)));
    const int h = std::max(1, static_cast<int>(std::lround(bh * pref)));
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
    // Adapt the scale before sizing the viewport: the composite (LayerFor) reads the area at the new scale,
    // so drawing at the old one for a frame showed a strip of stale picture along the right and bottom.
    ID3D11DeviceContext* ctx = renderer.Context();
    slot.timer.Collect(ctx);
    AdaptScale(slot, dt);
    int w, h;
    ViewportSize(slot, w, h);
    w = std::min(w, slot.target.width);
    h = std::min(h, slot.target.height);

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

void VisualizerHost::Render(Renderer& renderer, float time, float dt, FilterHost* filters) {
    if (m_slots.empty() || !m_device) return;
    // Composite one area of the screen, through the selected filter if the visualiser allows one.
    auto present = [&](const IVisualizer& viz, const CompositeLayer& a, const CompositeLayer* b, float mix, int style,
                       float seed, const RECT* region) {
        RenderTarget* scene = filters && viz.AllowsFilter() ? filters->SceneTarget(renderer) : nullptr;
        renderer.Composite(a, b, mix, style, seed, region, scene);
        if (scene) filters->Apply(renderer, region);
    };

    const RECT* primary = m_regions.empty() ? nullptr : &m_regions[0];
    if (primary) renderer.ClearBackBuffer();   // gaps between monitors of different sizes
    Slot& cur = m_slots[m_current];
    SetBase(cur, primary);
    RenderSlot(cur, renderer, time, dt);
    const CompositeLayer current = LayerFor(cur);

    if (m_previous >= 0) {
        Slot& prev = m_slots[m_previous];
        SetBase(prev, primary);
        RenderSlot(prev, renderer, time, dt);
        const CompositeLayer previous = LayerFor(prev);
        present(*cur.viz, previous, &current, m_fade, m_style, m_seed, primary);
    } else {
        present(*cur.viz, current, nullptr, 0.0f, 0, 0.0f, primary);
    }

    for (size_t i = 0; i < m_extra.size() && i + 1 < m_regions.size(); ++i) {
        Slot& s = m_slots[m_extra[i]];
        SetBase(s, &m_regions[i + 1]);
        RenderSlot(s, renderer, time, dt);
        present(*s.viz, LayerFor(s), nullptr, 0.0f, 0, 0.0f, &m_regions[i + 1]);
    }
}
