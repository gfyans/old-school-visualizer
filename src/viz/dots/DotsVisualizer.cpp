#include "viz/dots/DotsVisualizer.h"
#include "render/Renderer.h"
#include "g_dots_vs.h"
#include "g_dots_ps.h"
#include "g_dots_out_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct DotsConstants {   // matches cbuffer Dots in dots.hlsli
    float camRight[3]; float zoom;
    float camUp[3];    float aspect;
    float offsetY, dotBright, minRadius, waveTime;
    float layoutA, layoutB, morph, side;
    float motionA, motionB, motionBlend, histHead;
    float splashT, splashMorph, splashPower, flash;
    float base[3]; float hat;
    float mid[3];  float hatSeed;
    float peak[3]; float glow;
    float bg[3];   float buildUp;
    float scanPos, scanStrength, glitch, glitchSeed;
    float breach, breachSeed, beams, pad;
};
static_assert(sizeof(DotsConstants) % 16 == 0);

struct NamedPalette { const wchar_t* name; DotsVisualizer::Palette p; };
// at rest, rising, peak, backdrop
const NamedPalette kPalettes[] = {
    { L"Neon",       { { 0.05f, 0.35f, 0.60f }, { 0.00f, 0.90f, 1.00f }, { 1.00f, 0.15f, 0.75f }, { 0.006f, 0.012f, 0.030f } } },
    { L"Night City", { { 0.40f, 0.08f, 0.45f }, { 1.00f, 0.10f, 0.55f }, { 1.00f, 0.80f, 0.15f }, { 0.025f, 0.004f, 0.025f } } },
    { L"Acid",       { { 0.15f, 0.40f, 0.10f }, { 0.65f, 1.00f, 0.05f }, { 1.00f, 0.20f, 0.85f }, { 0.006f, 0.016f, 0.006f } } },
    { L"Red Alert",  { { 0.45f, 0.04f, 0.06f }, { 1.00f, 0.08f, 0.12f }, { 1.00f, 0.85f, 0.75f }, { 0.025f, 0.002f, 0.004f } } },
    { L"Holo",       { { 0.15f, 0.40f, 0.55f }, { 0.35f, 0.85f, 1.00f }, { 0.95f, 1.00f, 1.00f }, { 0.006f, 0.016f, 0.025f } } },
};
constexpr int kPaletteCount = static_cast<int>(std::size(kPalettes));

const wchar_t* const kLayouts[] = { L"Grid", L"Scatter", L"Rows", L"Rings", L"Spiral" };
constexpr int kLayoutCount = static_cast<int>(std::size(kLayouts));
const wchar_t* const kMotions[] = { L"Terrain", L"Equaliser" };
constexpr int kMotionCount = static_cast<int>(std::size(kMotions));
constexpr int kSide = 640;   // dots per side: 409k dots

constexpr float kElevation = 0.6154797f;   // true isometric: atan(1 / sqrt 2)
constexpr float kSplashSeconds = 3.4f;

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}
float Ease(float t) { return t * t * (3.0f - 2.0f * t); }
void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

} // namespace

DotsVisualizer::DotsVisualizer() : m_rng(std::random_device{}()), m_hist(static_cast<size_t>(kHistCols) * kHistRows, 0.0f) {
    m_from = m_to = kPalettes[0].p;
}

bool DotsVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    if (FAILED(device->CreateVertexShader(g_dots_vs, sizeof(g_dots_vs), nullptr, &m_dotsVS)) ||
        FAILED(device->CreatePixelShader(g_dots_ps, sizeof(g_dots_ps), nullptr, &m_dotsPS)) ||
        FAILED(device->CreatePixelShader(g_dots_out_ps, sizeof(g_dots_out_ps), nullptr, &m_outPS)))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(DotsConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &m_addBlend))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &m_linear))) return false;
    s.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;   // the history is a ring of rows
    s.MaxLOD = 0.0f;
    if (FAILED(device->CreateSamplerState(&s, &m_histSampler))) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kHistCols;
    td.Height = kHistRows;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{ m_hist.data(), kHistCols * sizeof(float), 0 };
    if (FAILED(device->CreateTexture2D(&td, &init, &m_histTex)) ||
        FAILED(device->CreateShaderResourceView(m_histTex.Get(), nullptr, &m_histSrv)))
        return false;
    m_rowsPending = 0;
    return true;
}

void DotsVisualizer::ReleaseResources() {
    m_srv.Reset();
    m_rtv.Reset();
    m_tex.Reset();
    m_histSrv.Reset();
    m_histTex.Reset();
    m_histSampler.Reset();
    m_linear.Reset();
    m_addBlend.Reset();
    m_cb.Reset();
    m_outPS.Reset();
    m_dotsPS.Reset();
    m_dotsVS.Reset();
    m_device.Reset();
    m_width = m_height = 0;
}

bool DotsVisualizer::CreateTarget(int w, int h) {
    m_srv.Reset();
    m_rtv.Reset();
    m_tex.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = td.Format;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_tex)) ||
        FAILED(m_device->CreateRenderTargetView(m_tex.Get(), &rv, &m_rtv)) ||
        FAILED(m_device->CreateShaderResourceView(m_tex.Get(), nullptr, &m_srv)))
        return false;
    m_width = w;
    m_height = h;
    return true;
}

DotsVisualizer::Palette DotsVisualizer::CurrentPalette() const {
    const float t = Ease(m_colourBlend);
    Palette r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void DotsVisualizer::NextColours(float seconds) {
    m_from = CurrentPalette();
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    m_to = kPalettes[m_scheme].p;
    if (m_hasArt && m_scheme % 2 == 1) {   // every other scheme takes the album's colours
        Copy3(m_to.mid, m_art.mid);
        Copy3(m_to.peak, m_art.hot);
    }
    m_colourBlend = 0.0f;
    m_colourSeconds = seconds;
}

void DotsVisualizer::NextLayout(float seconds) {
    m_layoutA = m_morph < 0.5f ? m_layoutA : m_layoutB;
    m_layoutB = (m_layoutA + 1 + std::uniform_int_distribution<int>(0, kLayoutCount - 2)(m_rng)) % kLayoutCount;
    m_morph = 0.0f;
    m_morphSeconds = seconds;
}

void DotsVisualizer::NextMotion(float seconds) {
    m_motionA = m_motionBlend < 0.5f ? m_motionA : m_motionB;
    m_motionB = (m_motionA + 1) % kMotionCount;
    m_motionBlend = 0.0f;
    m_motionSeconds = seconds;
}

void DotsVisualizer::Splash(bool morph) {
    m_splashT = 0.0f;
    m_flash = 1.0f;
    m_splashMorph = morph;
    if (morph) {   // the dots land in a new layout
        m_layoutA = m_morph < 0.5f ? m_layoutA : m_layoutB;
        m_layoutB = (m_layoutA + 1 + std::uniform_int_distribution<int>(0, kLayoutCount - 2)(m_rng)) % kLayoutCount;
        m_morph = 0.0f;
    }
}

void DotsVisualizer::Breach() {
    m_breachT = 0.0f;
    m_snapT = -1.0f;
    m_breachSeed = (m_breachSeed * 1103515245u + 12345u) % 1000u;
    m_flash = 1.0f;
    // Red alert while it's broken.
    m_from = CurrentPalette();
    m_to = kPalettes[3].p;
    m_scheme = 3;
    m_colourBlend = 0.0f;
    m_colourSeconds = 0.15f;
}

void DotsVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_silent = audio.silent;
    // The audio history: smoothed so the dots bounce rather than jitter.
    for (int i = 0; i < AudioFeatures::kSpectrumBands; ++i)
        m_smooth[i] = Smooth(m_smooth[i], audio.silent ? 0.0f : audio.spectrum[i], dt, 0.025f, 0.16f);
    m_smooth[64] = Smooth(m_smooth[64], audio.silent ? 0.0f : std::max(audio.bass, audio.beat * 0.9f), dt, 0.02f, 0.2f);
    m_smooth[65] = Smooth(m_smooth[65], audio.silent ? 0.0f : audio.level, dt, 0.05f, 0.3f);
    m_histClock += dt * kHistRate;
    while (m_histClock >= 1.0f) {
        m_histClock -= 1.0f;
        m_histRow = (m_histRow + 1) % kHistRows;
        memcpy(&m_hist[static_cast<size_t>(m_histRow) * kHistCols], m_smooth, sizeof(m_smooth));
        m_rowsPending = std::min(m_rowsPending + 1, kHistRows);
    }

    m_hat = Smooth(m_hat, audio.hat, dt, 0.01f, 0.08f);
    if (audio.hatCount != m_lastHat) { m_lastHat = audio.hatCount; m_hatSeed = 100 + (m_hatSeed * 7 + 13) % 9973; }
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.6f, 0.4f);
    m_glow = Smooth(m_glow, audio.silent ? 0.6f : 1.0f, dt, 0.5f, 2.0f);
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);

    // A scan line sweeps across the plane once a bar, locked to the grid (free-running without one).
    const bool locked = audio.tempoConfidence > 0.4f && !audio.silent;
    m_scanPhase = locked ? audio.barPhase : std::fmod(m_scanPhase + dt * 0.4f, 1.0f);
    m_scan = Smooth(m_scan, audio.silent ? 0.0f : (locked ? 0.6f + 0.6f * m_intensity : 0.3f), dt, 0.5f, 1.0f);

    // Glitches: snares tear a few slices of the plane sideways in big sections; drops tear it apart.
    if (audio.snareCount != m_lastSnare) {
        m_lastSnare = audio.snareCount;
        if (!audio.silent && m_intensity > 0.45f) {
            m_glitch = std::max(m_glitch, 0.35f);
            m_glitchSeed = (m_glitchSeed * 1103515245u + 12345u) % 100000u;
        }
    }
    m_glitch *= std::exp(-dt / 0.12f);
    m_beams = Smooth(m_beams, audio.silent ? 0.0f : 0.3f + 0.8f * m_intensity + 0.6f * audio.buildUp, dt, 0.5f, 1.0f);
    // Build-ups pull the camera down to skim across the dots; it springs back up as it lets go.
    m_skim = Smooth(m_skim, audio.silent ? 0.0f : audio.buildUp, dt, 1.5f, 0.35f);

    // Beats: count them for phrases and camera turns.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    bool beat = false;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; beat |= !gridLocked; }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; beat |= gridLocked; }
    if (beat && !audio.silent) { ++m_beatsSinceChange; ++m_beatsSinceTurn; }
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;

    // The big moment: splash.
    if (audio.dropCount != m_lastDrop || m_fireSplash) {
        m_lastDrop = audio.dropCount;
        m_fireSplash = false;
        if ((m_splashT < 0.0f || m_splashT > 1.5f) && m_breachT < 0.0f) {
            m_glitch = 1.0f;
            m_glitchSeed = (m_glitchSeed * 1103515245u + 12345u) % 100000u;
            if (m_nextBreach) {
                Breach();
            } else {
                Splash(!m_locked);
                if (!m_locked) { NextColours(1.5f); NextMotion(2.0f); }
            }
            m_nextBreach = !m_nextBreach;
            m_timeSinceChange = 0.0f;
            m_beatsSinceChange = 0;
        }
    }
    if (m_splashT >= 0.0f) {
        m_splashT += dt;
        if (m_splashT > kSplashSeconds) {
            m_splashT = -1.0f;
            if (m_splashMorph) { m_layoutA = m_layoutB; m_morph = 1.0f; }
        }
    }
    m_flash *= std::exp(-dt / 0.4f);

    // Breach: burst apart fast, drift while it hangs, then snap back together on a downbeat.
    if (m_breachT >= 0.0f) {
        m_breachT += dt;
        if (m_snapT < 0.0f) {
            const float t = std::min(m_breachT / 0.35f, 1.0f);
            m_breach = (1.0f - (1.0f - t) * (1.0f - t)) * (1.0f + 0.12f * m_breachT);
            if (m_breachT > 1.4f && (gridLocked ? onDownbeat : m_breachT > 2.2f)) m_snapT = 0.0f;
            if (m_breachT > 3.5f) m_snapT = 0.0f;
            m_snapFrom = m_breach;
        } else {
            m_snapT += dt;
            const float s = std::min(m_snapT / 0.22f, 1.0f);
            m_breach = m_snapFrom * (1.0f - s * s);
            if (s >= 1.0f) {   // whole again: a flash, a jolt of glitch and new colours
                m_breach = 0.0f;
                m_breachT = m_snapT = -1.0f;
                m_flash = 0.7f;
                m_glitch = std::max(m_glitch, 0.5f);
                m_glitchSeed = (m_glitchSeed * 1103515245u + 12345u) % 100000u;
                NextColours(0.8f);
            }
        }
    }

    // Phrases: new colours each time; the motion swaps every other phrase (about every 40 s), and
    // the layout often changes on the ones in between.
    m_timeSinceChange += dt;
    const bool phraseDue = m_beatsSinceChange >= 32 && m_timeSinceChange > 20.0f;
    if (!m_locked && m_splashT < 0.0f && ((phraseDue && (!gridLocked || onDownbeat)) || m_timeSinceChange > 60.0f)) {
        NextColours(6.0f);
        if (++m_phrases % 2 == 0) NextMotion(4.0f);
        else if (std::uniform_int_distribution<int>(0, 2)(m_rng) < 2) NextLayout(5.0f);
        m_timeSinceChange = 0.0f;
        m_beatsSinceChange = 0;
    }
    if (m_splashT < 0.0f) m_morph = std::min(1.0f, m_morph + dt / m_morphSeconds);
    m_colourBlend = std::min(1.0f, m_colourBlend + dt / m_colourSeconds);
    m_motionBlend = std::min(1.0f, m_motionBlend + dt / m_motionSeconds);

    // Camera: a slow drift, and an eased 45-degree turn every 4 or 8 bars, on the downbeat.
    m_timeSinceTurn += dt;
    const int turnBeats = audio.intensity > 0.6f ? 16 : 32;
    if ((gridLocked ? (m_beatsSinceTurn >= turnBeats && onDownbeat) : m_timeSinceTurn > 16.0f) && !audio.silent) {
        m_yawTarget += (std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f) * 0.785398f;
        m_beatsSinceTurn = 0;
        m_timeSinceTurn = 0.0f;
    }
    m_yawTarget += dt * 0.012f * audio.tempoFactor;
    m_yaw = m_yawTarget + (m_yaw - m_yawTarget) * std::exp(-dt / 1.1f);
}

void DotsVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    ID3D11DeviceContext* ctx = renderer.Context();
    if ((w != m_width || h != m_height) && !CreateTarget(w, h)) return;

    // Upload the history rows written since last frame.
    if (m_rowsPending >= kHistRows) {
        ctx->UpdateSubresource(m_histTex.Get(), 0, nullptr, m_hist.data(), kHistCols * sizeof(float), 0);
    } else {
        for (int k = 0; k < m_rowsPending; ++k) {
            const int row = (m_histRow - k + kHistRows) % kHistRows;
            D3D11_BOX box{ 0, static_cast<UINT>(row), 0, kHistCols, static_cast<UINT>(row + 1), 1 };
            ctx->UpdateSubresource(m_histTex.Get(), 0, &box, &m_hist[static_cast<size_t>(row) * kHistCols], kHistCols * sizeof(float), 0);
        }
    }
    m_rowsPending = 0;

    DotsConstants c{};
    // Isometric camera (orthographic), turning about the vertical.
    const float elevation = kElevation + (0.16f - kElevation) * Ease(std::min(m_skim, 1.0f));
    const float ce = std::cos(elevation), se = std::sin(elevation);
    const float fwd[3] = { -ce * std::sin(m_yaw), -se, -ce * std::cos(m_yaw) };
    float right[3] = { fwd[2], 0.0f, -fwd[0] };   // cross(worldUp, fwd)
    const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    right[0] /= rl; right[2] /= rl;
    const float up[3] = { fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0] };
    Copy3(c.camRight, right);
    Copy3(c.camUp, up);
    c.aspect = static_cast<float>(w) / h;
    c.zoom = std::min(0.92f * c.aspect / 1.42f, 0.88f) * (1.0f + 0.35f * m_skim);
    c.offsetY = -0.12f + 0.08f * m_skim;
    c.minRadius = 0.7f / (c.zoom * 0.5f * h);
    c.dotBright = 0.9f;
    c.waveTime = m_waveTime;
    c.layoutA = static_cast<float>(m_layoutA);
    c.layoutB = static_cast<float>(m_layoutB);
    c.morph = Ease(m_morph);
    const int side = kSide;
    c.side = static_cast<float>(side);
    c.motionA = static_cast<float>(m_motionA);
    c.motionB = static_cast<float>(m_motionB);
    c.motionBlend = Ease(m_motionBlend);
    c.histHead = static_cast<float>(m_histRow + kHistRows - 1) + m_histClock;
    c.splashT = m_splashT;
    c.splashMorph = m_splashMorph ? 1.0f : 0.0f;
    c.splashPower = 2.6f;
    c.flash = m_flash;
    const Palette p = CurrentPalette();
    Copy3(c.base, p.base);
    Copy3(c.mid, p.mid);
    Copy3(c.peak, p.peak);
    Copy3(c.bg, p.bg);
    c.hat = m_hat;
    c.hatSeed = static_cast<float>(m_hatSeed);
    c.glow = m_glow;
    c.buildUp = m_buildUp;
    c.scanPos = -1.5f + 3.0f * m_scanPhase;
    c.scanStrength = m_scan;
    c.glitch = m_glitch;
    c.glitchSeed = static_cast<float>(m_glitchSeed);
    c.breach = m_breach;
    c.breachSeed = static_cast<float>(m_breachSeed);
    c.beams = m_beams;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();

    // 1. The dots, additively.
    renderer.BindTarget(m_rtv.Get(), w, h, time, dt);
    const float black[4] = {};
    ctx->ClearRenderTargetView(m_rtv.Get(), black);
    ctx->OMSetBlendState(m_addBlend.Get(), nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_dotsVS.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(2, 1, &cb);
    ID3D11ShaderResourceView* hist = m_histSrv.Get();
    ID3D11SamplerState* histSampler = m_histSampler.Get();
    ctx->VSSetShaderResources(0, 1, &hist);
    ctx->VSSetSamplers(0, 1, &histSampler);
    ctx->PSSetShader(m_dotsPS.Get(), nullptr, 0);
    const UINT n = static_cast<UINT>(side) * side;
    ctx->DrawInstanced(6, n + n / 64, 0, 0);   // dots, then beams
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->VSSetShaderResources(0, 1, &none);

    // 2. Glow mips, then the output into the host's target.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(m_srv.Get());
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11ShaderResourceView* srv = m_srv.Get();
    ID3D11SamplerState* linear = m_linear.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &linear);
    renderer.DrawFullscreen(m_outPS.Get());
    ctx->PSSetShaderResources(0, 1, &none);
}

bool DotsVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextColours(2.0f);
        return true;
    }
    if (vk == 'M') {
        if (m_splashT >= 0.0f) return true;
        NextLayout(2.5f);
        return true;
    }
    if (vk == 'Q') {   // the next big hit (they alternate: splash, breach)
        m_fireSplash = true;
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    }
    return false;
}

std::wstring DotsVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring DotsVisualizer::Status() const {
    return std::wstring(kLayouts[m_layoutB]) + L", " + kMotions[m_motionB] + L", " +
           (m_hasArt && m_scheme % 2 == 1 ? L"Album colours" : kPalettes[m_scheme].name) + (m_locked ? L" (locked)" : L"");
}

void DotsVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Palette p = CurrentPalette();
    Copy3(lo, p.mid);
    Copy3(hi, p.peak);
}

void DotsVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked && m_timeSinceChange > 10.0f) { NextColours(5.0f); NextMotion(4.0f); }
}
