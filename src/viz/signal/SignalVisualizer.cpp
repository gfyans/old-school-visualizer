#include "viz/signal/SignalVisualizer.h"
#include "render/Renderer.h"
#include "g_signal_ps.h"
#include "g_signal_mesh_vs.h"
#include "g_signal_mesh_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace {

struct SignalConstants {   // matches cbuffer Signal in signal_ps.hlsl
    float style, subject, cellScale, time;
    float yaw, pitch, jaw, pulse;
    float glitch, flicker, dissolve, flash;
    float eyes, morph, rain, hasMesh;
    float bg[3];     float nextSubject;
    float mid[3];    float layoutMix;
    float hi[3];     float spin;
    float accent[3]; float scroll;
    float rowsNow, rowsPrev, beatAge, beatHit;
};
static_assert(sizeof(SignalConstants) == sizeof(float) * 36);

constexpr int kMaxInstances = 64;
struct MeshInstances {     // matches cbuffer SignalMesh in signal_mesh.hlsli
    float a[kMaxInstances][4];   // yaw, pitch, scale, eye glow
    float b[kMaxInstances][4];   // tile centre x, y (screen units, y -1..1), tile scale, brightness
};

constexpr float kTileView = 0.68f;   // how much of a tile a subject fills (matches signal_ps.hlsl)
constexpr float kModelScale = 1.25f; // the model fits in -1..1; this matches the distance-field skull's size
constexpr float kCentreZoom = 1.2f;  // the model alone in the centre: close up
constexpr int kBackRows = 4;         // the dim grid behind the centre skull (matches signal_ps.hlsl)
constexpr float kBackDim = 0.3f;     // ...and how bright it is
// ...and raised (screen units, 2 = full height) so each model sits in the middle: skull, head, brain.
constexpr float kCentreLift[3] = { 0.17f, 0.08f, -0.02f };
constexpr int kModelResource = 101;  // RCDATA ids 101 skull, 102 head, 103 brain (CMakeLists.txt writes models.rc)

const wchar_t* kStyles[] = { L"", L"ASCII", L"Dither", L"LED", L"Scan lines" };
const wchar_t* kSubjects[] = { L"Skull", L"Gas mask", L"Brain" };
constexpr int kSubjectCount = 3;
constexpr int kHead = 1;   // the only subject with no distance-field fallback

struct Palette { const wchar_t* name; float c[4][3]; };   // bg, mid, hi, accent
const Palette kPalettes[] = {
    { L"Noir",  { { 0.035f, 0.008f, 0.10f }, { 1.00f, 0.16f, 0.43f }, { 0.82f, 0.97f, 1.00f }, { 0.02f, 0.85f, 0.91f } } },
    { L"Acid",  { { 0.015f, 0.04f, 0.02f },  { 0.20f, 0.90f, 0.30f }, { 0.85f, 1.00f, 0.60f }, { 1.00f, 0.90f, 0.10f } } },
    { L"Blood", { { 0.05f, 0.00f, 0.01f },   { 0.90f, 0.06f, 0.12f }, { 1.00f, 0.88f, 0.72f }, { 1.00f, 0.50f, 0.10f } } },
    { L"Ice",   { { 0.00f, 0.02f, 0.06f },   { 0.12f, 0.45f, 0.95f }, { 0.90f, 0.97f, 1.00f }, { 0.65f, 0.25f, 1.00f } } },
};
constexpr int kPaletteCount = 4;
// Cell size per style, relative to the shared sizes (shaders/cyber.hlsli, the same as the filters'):
// slightly chunkier dither, which also keeps the distance-field subjects' ray count down.
constexpr float kCellScale[5] = { 1.0f, 1.0f, 1.5f, 1.0f, 1.0f };

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

float Hash(int x, int y) {   // 0..1, stable per tile
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

std::wstring LayoutName(int rows) {
    return rows == 0 ? std::wstring(L"Centre") : L"Grid ×" + std::to_wstring(rows);
}

} // namespace

SignalVisualizer::SignalVisualizer() : m_rng(std::random_device{}()) {
    m_style = std::uniform_int_distribution<int>(1, 4)(m_rng);
    m_subject = m_nextSubject = 0;   // start on the skull
    // Developer switch for comparing looks: VISUALIZER_SIGNAL_LOOK=<style 1-4>,<subject 0-2>[,<grid rows 0-4>]
    // fixes and locks it.
    wchar_t env[24];
    if (GetEnvironmentVariableW(L"VISUALIZER_SIGNAL_LOOK", env, ARRAYSIZE(env)) > 0) {
        int style = 0, subject = 0, rows = 0;
        if (swscanf_s(env, L"%d,%d,%d", &style, &subject, &rows) >= 2 && style >= 1 && style <= 4 &&
            subject >= 0 && subject < kSubjectCount && rows >= 0 && rows <= 4) {
            m_style = style;
            m_subject = m_nextSubject = subject;
            m_rows = m_prevRows = rows;
            m_locked = true;
        }
    }
}

bool SignalVisualizer::CreateResources(ID3D11Device* device) {
    if (FAILED(device->CreatePixelShader(g_signal_ps, sizeof(g_signal_ps), nullptr, &m_ps))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(SignalConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;
    // The models are optional: the skull and brain fall back to distance fields, the head is left out.
    if (CreateMeshPipeline(device))
        for (int i = 0; i < kModels; ++i) m_models[i].ok = LoadModel(device, i);
    if (!HasModel(kHead)) {
        if (m_subject == kHead) m_subject = 0;
        if (m_nextSubject == kHead) { m_nextSubject = m_subject; m_morph = 0.0f; }
    }
    return true;
}

bool SignalVisualizer::CreateMeshPipeline(ID3D11Device* device) {
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },   // w: cavity
        { "NORMAL",   0, DXGI_FORMAT_R8G8B8A8_SNORM,     0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },   // w: eye mask
    };
    D3D11_BUFFER_DESC icb{};
    icb.ByteWidth = sizeof(MeshInstances);
    icb.Usage = D3D11_USAGE_DYNAMIC;
    icb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    icb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;   // a few thin open parts; the GPU has time to spare
    rs.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateBuffer(&icb, nullptr, &m_instanceCB)) &&
           SUCCEEDED(device->CreateVertexShader(g_signal_mesh_vs, sizeof(g_signal_mesh_vs), nullptr, &m_meshVS)) &&
           SUCCEEDED(device->CreatePixelShader(g_signal_mesh_ps, sizeof(g_signal_mesh_ps), nullptr, &m_meshPS)) &&
           SUCCEEDED(device->CreateInputLayout(layout, ARRAYSIZE(layout), g_signal_mesh_vs, sizeof(g_signal_mesh_vs), &m_meshLayout)) &&
           SUCCEEDED(device->CreateDepthStencilState(&ds, &m_depthState)) &&
           SUCCEEDED(device->CreateRasterizerState(&rs, &m_raster)) &&
           SUCCEEDED(device->CreateSamplerState(&sd, &m_sampler));
}

bool SignalVisualizer::LoadModel(ID3D11Device* device, int subject) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kModelResource + subject), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return false;
    HGLOBAL handle = LoadResource(nullptr, res);
    const auto* data = static_cast<const uint8_t*>(handle ? LockResource(handle) : nullptr);
    const DWORD size = SizeofResource(nullptr, res);
    if (!data || size < 20 || std::memcmp(data, "VMSH", 4) != 0) return false;
    uint32_t header[4];   // version, vertex count, index count, index bytes
    std::memcpy(header, data + 4, sizeof(header));
    const uint32_t vertices = header[1], indices = header[2], indexBytes = header[3];
    constexpr uint32_t kStride = 12;
    if (header[0] != 1 || (indexBytes != 2 && indexBytes != 4) || indices % 3 != 0 ||
        20ull + uint64_t(vertices) * kStride + uint64_t(indices) * indexBytes != size)
        return false;

    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = vertices * kStride;
    vb.Usage = D3D11_USAGE_IMMUTABLE;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vdata{ data + 20, 0, 0 };
    D3D11_BUFFER_DESC ib = vb;
    ib.ByteWidth = indices * indexBytes;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA idata{ data + 20 + vb.ByteWidth, 0, 0 };
    Model& m = m_models[subject];
    if (FAILED(device->CreateBuffer(&vb, &vdata, &m.vb)) || FAILED(device->CreateBuffer(&ib, &idata, &m.ib))) return false;
    m.indexCount = indices;
    m.indexFormat = indexBytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    return true;
}

int SignalVisualizer::RandomSubject(int other) {
    int pick;
    do pick = std::uniform_int_distribution<int>(0, kSubjectCount - 1)(m_rng);
    while (pick == other || (pick == kHead && !HasModel(kHead)));
    return pick;
}

void SignalVisualizer::ReleaseResources() {
    for (auto& s : m_scene) s.Reset();
    m_dsv.Reset();
    m_depth.Reset();
    m_sampler.Reset();
    m_raster.Reset();
    m_depthState.Reset();
    m_instanceCB.Reset();
    for (Model& m : m_models) m = Model{};
    m_meshLayout.Reset();
    m_meshPS.Reset();
    m_meshVS.Reset();
    m_cb.Reset();
    m_ps.Reset();
}

void SignalVisualizer::SetLayout(int rows) {
    if (rows == m_rows) return;
    m_prevRows = m_rows;
    m_rows = rows;
    m_layoutMix = 0.0f;
    m_glitch = std::max(m_glitch, 0.6f);
}

void SignalVisualizer::NextLook(bool newSubject, int rows) {
    m_style = 1 + (m_style + std::uniform_int_distribution<int>(0, 2)(m_rng)) % 4;   // a different style
    m_palette = (m_palette + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    if (newSubject && m_morph <= 0.0f) {
        m_nextSubject = RandomSubject(m_subject);
        m_morph = 0.001f;
    }
    // Alternate one subject in the centre with a grid of them (mostly; now and then a different grid).
    if (rows >= 0) SetLayout(rows);
    else if (m_rows == 0) SetLayout(std::uniform_int_distribution<int>(2, 4)(m_rng));
    else if (std::uniform_int_distribution<int>(0, 3)(m_rng) > 0) SetLayout(0);
    else SetLayout(2 + (m_rows - 1 + std::uniform_int_distribution<int>(0, 1)(m_rng)) % 3);
    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
}

void SignalVisualizer::Update(const AudioFeatures& audio, float dt) {
    const float tf = audio.tempoFactor;
    m_time += dt;
    m_spin += dt * tf * (audio.silent ? 0.12f : 0.25f + 0.25f * audio.intensity);
    m_scroll += dt * tf * (audio.silent ? 0.03f : 0.06f + 0.1f * audio.intensity);   // grids drift upward

    // The jaw follows the voice (and the bass, for instrumentals); kicks pulse; snares flash the eyes.
    m_jaw = Smooth(m_jaw, std::min(1.0f, 1.2f * audio.vocal + 0.45f * audio.bass), dt, 0.04f, 0.12f);
    m_pulse = Smooth(m_pulse, audio.beat, dt, 0.02f, 0.15f);
    m_eyes = Smooth(m_eyes, audio.snare, dt, 0.01f, 0.2f);
    m_dissolve = Smooth(m_dissolve, audio.buildUp * audio.buildUp, dt, 0.4f, 0.2f);
    m_rain = Smooth(m_rain, audio.silent ? 0.3f : 0.5f + 0.8f * audio.intensity, dt, 1.0f, 2.0f);
    m_glitch = std::max(m_glitch * std::exp(-dt / 0.12f), std::min(1.0f, 0.8f * audio.snare + 0.6f * m_dissolve + audio.drop));
    m_flash *= std::exp(-dt / 0.12f);
    m_beatAge += dt;
    if (m_layoutMix < 1.0f) m_layoutMix = std::min(1.0f, m_layoutMix + dt / 0.45f);

    // Drops: flash, flood the screen with a grid, new subject and style. Phrases (8 bars): new style and
    // palette, centre <-> grid.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; if (gridLocked) ++m_beatsSinceChange; }
    if (audio.beatCount != m_lastBeat) {
        m_lastBeat = audio.beatCount;
        if (!gridLocked) ++m_beatsSinceChange;
        m_beatAge = 0.0f;   // a ripple out across the grid
        m_beatHit = std::min(1.0f, 0.4f + 0.8f * audio.bass);
    }
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop) {
        m_lastDrop = audio.dropCount;
        m_flash = 1.0f;
        if (!m_locked) NextLook(true, std::uniform_int_distribution<int>(3, 4)(m_rng));
    }
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_beatsSinceChange >= 32 && (!gridLocked || onDownbeat) && m_timeSinceChange > 15.0f) || m_timeSinceChange > 60.0f))
        NextLook(m_timeSinceChange > 30.0f);   // a new subject now and then, not every phrase
    if (m_morph > 0.0f) {
        m_morph += dt / 1.5f;
        if (m_morph >= 1.0f) { m_subject = m_nextSubject; m_morph = 0.0f; }
    }

    SignalConstants c{};
    c.style = static_cast<float>(m_style);
    c.subject = static_cast<float>(m_subject);
    c.nextSubject = static_cast<float>(m_nextSubject);
    c.morph = m_morph;
    c.cellScale = kCellScale[m_style];
    c.time = std::fmod(m_time, 1000.0f);
    // A slow turn with a sway: the skull and head three-quarter, the brain more side on. The models of the
    // skull and head nod with the voice instead of working a jaw.
    const int shown = m_morph > 0.5f ? m_nextSubject : m_subject;
    const float base = shown <= kHead ? -0.5f : -0.7f;
    m_yaw = base + (shown <= kHead ? 0.3f : 0.55f) * std::sin(m_spin) + 0.15f * std::sin(m_spin * 2.3f);
    m_pitch = 0.12f * std::sin(m_spin * 0.7f) - 0.05f - (HasModel(shown) && shown <= kHead ? 0.12f * m_jaw : 0.0f);
    c.yaw = m_yaw;
    c.pitch = m_pitch;
    c.jaw = shown == 0 ? m_jaw : 0.0f;
    c.pulse = m_pulse;
    c.glitch = m_glitch;
    c.flicker = audio.hat;
    c.dissolve = m_dissolve;
    c.flash = m_flash;
    c.eyes = m_eyes;
    c.rain = m_rain;
    c.hasMesh = static_cast<float>((HasModel(0) ? 1 : 0) | (HasModel(1) ? 2 : 0) | (HasModel(2) ? 4 : 0));   // one bit per model
    c.layoutMix = m_layoutMix;
    c.spin = m_spin;
    c.scroll = std::fmod(m_scroll, 240.0f);   // a multiple of every tile size, so the wrap is invisible
    c.rowsNow = static_cast<float>(m_rows);
    c.rowsPrev = static_cast<float>(m_prevRows);
    c.beatAge = m_beatAge;
    c.beatHit = m_beatHit;
    const Palette& p = kPalettes[m_palette];
    memcpy(c.bg, p.c[0], sizeof(c.bg));
    memcpy(c.mid, p.c[1], sizeof(c.mid));
    memcpy(c.hi, p.c[2], sizeof(c.hi));
    memcpy(c.accent, p.c[3], sizeof(c.accent));
    if (m_hasArt && (m_palette % 2 == 1)) {   // album colours on alternate palettes
        memcpy(c.mid, m_art.mid, sizeof(c.mid));
        memcpy(c.accent, m_art.hot, sizeof(c.accent));
    }
    memcpy(m_constants, &c, sizeof(c));
}

bool SignalVisualizer::EnsureSceneTargets(ID3D11Device* device, int w, int h) {
    if (m_scene[0].width == w && m_scene[0].height == h && m_dsv) return true;
    m_dsv.Reset();
    m_depth.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    return m_scene[0].Create(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
           m_scene[1].Create(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
           m_scene[2].Create(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
           SUCCEEDED(device->CreateTexture2D(&d, nullptr, &m_depth)) &&
           SUCCEEDED(device->CreateDepthStencilView(m_depth.Get(), nullptr, &m_dsv));
}

// Draws a subject's model into m_scene[slot]: one in the centre, or one per grid tile (posed like
// signal_ps.hlsl's TileView poses the distance-field subjects).
void SignalVisualizer::DrawModel(Renderer& renderer, int slot, int subject, int rows, int w, int h, float time, float dt) {
    const Model& model = m_models[subject];
    MeshInstances inst{};
    int count = 0;
    // One skull per grid tile; the centre layout also has a dim grid of smaller ones behind its skull.
    auto addGrid = [&](int rows, float brightness) {
        const float T = 2.0f / static_cast<float>(rows);
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        const float scroll = std::fmod(m_scroll, 240.0f);
        const float margin = 0.65f * T;   // tiles just off screen still show a turned skull's edge
        const int rowLo = static_cast<int>(std::floor((-1.0f - margin - scroll) / T));
        const int rowHi = static_cast<int>(std::floor((1.0f + margin - scroll) / T));
        for (int row = rowLo; row <= rowHi && count < kMaxInstances; ++row) {
            const float cy = (row + 0.5f) * T + scroll;
            if (std::fabs(cy) > 1.0f + margin) continue;
            const float shift = (row * 0.5f - std::floor(row * 0.5f)) * T;   // alternate rows staggered
            const int colLo = static_cast<int>(std::ceil((-aspect - margin - shift) / T));
            const int colHi = static_cast<int>(std::floor((aspect + margin - shift) / T));
            for (int col = colLo; col <= colHi && count < kMaxInstances; ++col) {
                const float cx = col * T + shift;
                const float dist = std::sqrt(cx * cx + cy * cy) / T;
                const float wave = dist - m_beatAge * 9.0f;
                const float ripple = m_beatHit * std::exp(-wave * wave * 0.8f) * std::exp(-m_beatAge * 3.0f);
                const float a[4] = {
                    m_yaw + 0.45f * std::sin(m_spin * 2.0f - 0.6f * static_cast<float>(col + row)),
                    m_pitch + 0.08f * std::sin(m_spin * 1.7f - 0.5f * static_cast<float>(row)),
                    kModelScale * (1.0f + 0.12f * ripple),
                    m_eyes * (0.4f + 0.6f * Hash(col, row)) };
                const float b[4] = { cx, cy, T * 0.5f / kTileView, brightness };
                memcpy(inst.a[count], a, sizeof(a));
                memcpy(inst.b[count], b, sizeof(b));
                ++count;
            }
        }
    };
    if (rows == 0) {
        const float a[4] = { m_yaw, m_pitch, kModelScale * (1.0f + 0.05f * m_pulse), m_eyes };
        const float b[4] = { 0.0f, kCentreLift[subject], kCentreZoom, 1.0f };
        memcpy(inst.a[0], a, sizeof(a));
        memcpy(inst.b[0], b, sizeof(b));
        count = 1;
        addGrid(kBackRows, kBackDim);
    } else {
        addGrid(rows, 1.0f);
    }

    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_instanceCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &inst, sizeof(inst));
        ctx->Unmap(m_instanceCB.Get(), 0);
    }
    RenderTarget& target = m_scene[slot];
    renderer.BindTarget(target.rtv.Get(), w, h, time, dt);   // frame constants: this size
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, m_dsv.Get());
    const float clear[4] = {};
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetDepthStencilState(m_depthState.Get(), 0);
    ctx->RSSetState(m_raster.Get());
    const UINT stride = 12, offset = 0;
    ID3D11Buffer* vb = model.vb.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(model.ib.Get(), model.indexFormat, 0);
    ctx->IASetInputLayout(m_meshLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* icb = m_instanceCB.Get();
    ctx->VSSetConstantBuffers(3, 1, &icb);
    ctx->VSSetShader(m_meshVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_meshPS.Get(), nullptr, 0);
    ctx->DrawIndexedInstanced(model.indexCount, static_cast<UINT>(count), 0, 0, 0);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(target.srv.Get());   // signal_ps averages over a cell by reading a mip
}

void SignalVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11DeviceContext* ctx = renderer.Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_constants, sizeof(SignalConstants));
        ctx->Unmap(m_cb.Get(), 0);
    }

    // The model, when it's on screen: this layout, and the previous one while wiping between them.
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    const bool modelNow = HasModel(m_subject), modelNext = m_morph > 0.0f && HasModel(m_nextSubject);
    const bool model = (modelNow || modelNext) && EnsureSceneTargets(renderer.Device(), w, h);
    if (model) {
        if (modelNow) DrawModel(renderer, 0, m_subject, m_rows, w, h, time, dt);
        if (modelNow && m_layoutMix < 1.0f) DrawModel(renderer, 1, m_subject, m_prevRows, w, h, time, dt);
        if (modelNext) DrawModel(renderer, 2, m_nextSubject, m_rows, w, h, time, dt);
        renderer.BindTarget(hostTarget, w, h, time, dt);
    }

    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ID3D11ShaderResourceView* srvs[3] = { model ? m_scene[0].srv.Get() : nullptr, model ? m_scene[1].srv.Get() : nullptr,
                                          model ? m_scene[2].srv.Get() : nullptr };
    ctx->PSSetShaderResources(0, 3, srvs);
    ID3D11SamplerState* sampler = m_sampler.Get();
    if (sampler) ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_ps.Get());
    ID3D11ShaderResourceView* none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
}

bool SignalVisualizer::OnKey(UINT vk) {
    if (vk == 'Q') {   // the drop's moment, by hand: the next update sees a "new" drop
        m_lastDrop = ~0u;
        return true;
    }
    if (vk == 'N') {
        NextLook(true);
        m_message = std::wstring(kSubjects[m_nextSubject]) + L"  ·  " + kStyles[m_style] + L"  ·  " + kPalettes[m_palette].name;
        return true;
    }
    if (vk == 'M') {
        SetLayout(m_rows == 0 ? std::uniform_int_distribution<int>(2, 4)(m_rng) : 0);
        m_message = LayoutName(m_rows);
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    }
    return false;
}

std::wstring SignalVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring SignalVisualizer::Status() const {
    return std::wstring(kSubjects[m_subject]) + L" · " + kStyles[m_style] + L" · " + kPalettes[m_palette].name +
           L" · " + LayoutName(m_rows) + (m_locked ? L" (locked)" : L"");
}

void SignalVisualizer::AccentColours(float lo[3], float hi[3]) const {
    memcpy(lo, kPalettes[m_palette].c[1], 3 * sizeof(float));
    memcpy(hi, kPalettes[m_palette].c[3], 3 * sizeof(float));
}

void SignalVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked && m_timeSinceChange > 10.0f) NextLook(true);
}
