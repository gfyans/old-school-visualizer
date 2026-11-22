#include "filter/FilterHost.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// Float like the visualisers' own targets: no banding before the filter restyles the picture.
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R11G11B10_FLOAT;

struct FilterConstants {   // matches shaders/filter_common.hlsli
    float sceneUvScale[2], sceneUvMax[2];
    float glitch, time, pad[2];
    float palette[4][4];   // bg, mid, hi, accent (rgb + pad)
};
static_assert(sizeof(FilterConstants) % 16 == 0);

constexpr int kPaletteCount = 4;
constexpr float kPalettes[kPaletteCount][4][3] = {   // bg, mid, hi, accent
    { { 0.035f, 0.008f, 0.10f }, { 1.00f, 0.16f, 0.43f }, { 0.82f, 0.97f, 1.00f }, { 0.02f, 0.85f, 0.91f } },   // Noir
    { { 0.015f, 0.04f, 0.02f },  { 0.20f, 0.90f, 0.30f }, { 0.85f, 1.00f, 0.60f }, { 1.00f, 0.90f, 0.10f } },   // Acid
    { { 0.05f, 0.00f, 0.01f },   { 0.90f, 0.06f, 0.12f }, { 1.00f, 0.88f, 0.72f }, { 1.00f, 0.50f, 0.10f } },   // Blood
    { { 0.00f, 0.02f, 0.06f },   { 0.12f, 0.45f, 0.95f }, { 0.90f, 0.97f, 1.00f }, { 0.65f, 0.25f, 1.00f } },   // Ice
};
} // namespace

bool ShaderFilter::CreateResources(ID3D11Device* device) {
    return SUCCEEDED(device->CreatePixelShader(m_bytecode, m_size, nullptr, &m_ps));
}

void ShaderFilter::Apply(Renderer& renderer) {
    renderer.DrawFullscreen(m_ps.Get());
}

void FilterHost::Add(std::unique_ptr<IFilter> filter) {
    m_filters.push_back(std::move(filter));
}

bool FilterHost::CreateResources(ID3D11Device* device) {
    m_device = device;
    m_scene.Reset();
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FilterConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bool ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_constants));
    for (auto& f : m_filters) ok &= f->CreateResources(device);
    return ok;
}

void FilterHost::ReleaseResources() {
    for (auto& f : m_filters) f->ReleaseResources();
    m_constants.Reset();
    m_scene.Reset();
    m_device = nullptr;
}

int FilterHost::FindByName(const std::wstring& name) const {
    for (int i = 0; i < Count(); ++i)
        if (name == m_filters[i]->Name()) return i;
    return -1;
}

void FilterHost::Select(int index) {
    m_current = index >= 0 && index < Count() ? index : -1;
    if (!Active()) m_scene.Reset();   // free the memory while no filter is on
}

void FilterHost::Next() {
    Select(m_current + 1 < Count() ? m_current + 1 : -1);
}

void FilterHost::SelectRandom(uint32_t seed) {
    if (!Active()) return;
    std::vector<int> pool;
    for (int i = 0; i < Count(); ++i)
        if (m_filters[i]->InRandom()) pool.push_back(i);
    if (!pool.empty()) Select(pool[seed % pool.size()]);
}

std::wstring FilterHost::Message() const {
    return Active() ? std::wstring(L"Filter: ") + CurrentName() : std::wstring(L"Filter off");
}

void FilterHost::NewTrack() {
    m_palette = (m_palette + 1) % kPaletteCount;
}

void FilterHost::Update(const AudioFeatures& audio, float dt) {
    if (audio.dropCount != m_seenDrop) { m_seenDrop = audio.dropCount; m_palette = (m_palette + 1) % kPaletteCount; }
    // How hard rows tear: snares, build-ups and drops, falling away quickly.
    m_glitch = std::max(m_glitch * std::exp(-dt / 0.12f),
                        std::min(1.0f, 0.7f * audio.snare + 0.6f * audio.buildUp * audio.buildUp + audio.drop));
    m_time = std::fmod(m_time + dt, 1000.0f);
    if (Active()) m_filters[m_current]->Update(audio, dt);
}

RenderTarget* FilterHost::SceneTarget(Renderer& renderer) {
    if (!Active() || !m_device) return nullptr;
    const int w = std::max(1, renderer.Width()), h = std::max(1, renderer.Height());
    if (m_scene.width != w || m_scene.height != h) m_scene.Create(m_device, w, h, kSceneFormat);
    return m_scene.rtv ? &m_scene : nullptr;
}

void FilterHost::Apply(Renderer& renderer, const RECT* region) {
    if (!Active() || !m_scene.srv || !m_constants) return;
    renderer.BindBackBuffer(region);
    const float w = static_cast<float>(region ? region->right - region->left : renderer.Width());
    const float h = static_cast<float>(region ? region->bottom - region->top : renderer.Height());
    const float tw = static_cast<float>(m_scene.width), th = static_cast<float>(m_scene.height);

    FilterConstants fc{};
    fc.sceneUvScale[0] = w / tw;
    fc.sceneUvScale[1] = h / th;
    fc.sceneUvMax[0] = (w - 0.5f) / tw;
    fc.sceneUvMax[1] = (h - 0.5f) / th;
    fc.glitch = m_glitch;
    fc.time = m_time;
    for (int k = 0; k < 4; ++k) memcpy(fc.palette[k], kPalettes[m_palette][k], 3 * sizeof(float));

    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &fc, sizeof(fc));
        ctx->Unmap(m_constants.Get(), 0);
    }
    ID3D11Buffer* cb = m_constants.Get();
    ID3D11ShaderResourceView* srv = m_scene.srv.Get();
    ID3D11SamplerState* sampler = renderer.LinearClamp();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    m_filters[m_current]->Apply(renderer);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);   // so the scene can be a render target again next frame
}
