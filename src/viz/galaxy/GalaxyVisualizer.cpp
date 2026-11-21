#include "viz/galaxy/GalaxyVisualizer.h"
#include "render/Renderer.h"
#include "g_galaxy_vs.h"
#include "g_galaxy_ps.h"
#include "g_galaxy_out_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct GalaxyConstants {   // matches cbuffer Galaxy in galaxy.hlsli
    float camPos[3];   float focal;
    float camRight[3]; float aspect;
    float camUp[3];    float starBright;
    float camFwd[3];   float pattern;
    float shapeA[4];
    float shapeB[4];
    float thickA, thickB, shapeBlend, gravity;
    float waves[4][4];
    float core[3];     float accretion;
    float arm[3];      float twinkle;
    float knot[3];     float jet;
    float disc[3];     float animTime;
    float accSpin, horizon, exposure, flash;
    float dustAmount, sky[3];
    float glow, kick, bhPos[2];
    float jetA[2], jetB[2];
    float quasar, beamWidth, pad0, pad1;
};
static_assert(sizeof(GalaxyConstants) % 16 == 0);
static_assert(sizeof(GalaxyConstants) <= sizeof(float) * 80);

struct NamedPalette { const wchar_t* name; GalaxyVisualizer::Palette p; };
// core, arms, star-forming knots, accretion disc, sky
const NamedPalette kPalettes[] = {
    { L"Milky Way", { { 1.00f, 0.84f, 0.62f }, { 0.42f, 0.58f, 1.00f }, { 1.00f, 0.38f, 0.75f }, { 1.00f, 0.55f, 0.20f }, { 0.006f, 0.016f, 0.050f } } },
    { L"Ember",     { { 1.00f, 0.72f, 0.40f }, { 1.00f, 0.42f, 0.18f }, { 1.00f, 0.85f, 0.40f }, { 1.00f, 0.45f, 0.12f }, { 0.020f, 0.008f, 0.006f } } },
    { L"Nebular",   { { 1.00f, 0.75f, 0.70f }, { 0.50f, 0.45f, 1.00f }, { 1.00f, 0.35f, 0.80f }, { 0.60f, 0.80f, 1.00f }, { 0.012f, 0.006f, 0.030f } } },
    { L"Ice",       { { 1.00f, 0.95f, 0.80f }, { 0.30f, 0.75f, 1.00f }, { 0.60f, 1.00f, 0.95f }, { 0.70f, 0.90f, 1.00f }, { 0.004f, 0.014f, 0.030f } } },
    { L"Toxic",     { { 1.00f, 0.95f, 0.60f }, { 0.25f, 0.90f, 0.60f }, { 1.00f, 0.80f, 0.30f }, { 0.80f, 1.00f, 0.40f }, { 0.004f, 0.016f, 0.012f } } },
};
constexpr int kPaletteCount = static_cast<int>(std::size(kPalettes));

// arms, twist (radians per e-fold of radius), arm width, bulge size, disc thickness, camera elevation
const GalaxyVisualizer::Shape kShapes[] = {
    { 2.0f, 5.0f, 0.42f, 0.40f, 0.025f, 0.30f },   // Andromeda: tightly wound, big bulge, steeply tilted
    { 4.0f, 3.8f, 0.30f, 0.32f, 0.025f, 0.62f },   // many-armed, like the Milky Way
    { 2.0f, 3.6f, 0.45f, 0.30f, 0.025f, 0.48f },   // grand design, like M81
    { 3.0f, 3.0f, 0.36f, 0.28f, 0.030f, 0.80f },   // three arms, seen from above
    { 2.0f, 2.2f, 0.55f, 0.24f, 0.035f, 0.55f },   // loose and open
};
constexpr int kShapeCount = static_cast<int>(std::size(kShapes));

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(Dot(a, a), 1e-12f))); }

const GalaxyVisualizer::Shot kShots[] = {
    // dist   elev    yaw/s  focal  roll    side
    { 1.55f, 0.50f,  0.07f, 1.60f,  0.00f, 0.00f },   // wide orbit (elevation from the look)
    { 0.80f, 0.09f,  0.24f, 1.25f,  0.20f, 0.38f },   // flyby, skimming through the arms
    { 0.40f, 0.50f,  0.20f, 1.45f, -0.12f, 0.00f },   // dive to the black hole
    { 1.45f, 1.38f,  0.12f, 1.60f,  0.00f, 0.00f },   // top-down spin
    { 1.70f, 0.035f, 0.05f, 1.90f,  0.06f, 0.00f },   // edge-on skim
    { 1.05f, 0.16f,  0.10f, 1.50f,  0.22f, 0.00f },   // quasar: the beams across the frame (drops)
};
constexpr int kShotCount = static_cast<int>(std::size(kShots));
constexpr int kShotQuasar = kShotCount - 1;

GalaxyVisualizer::Palette FromArt(const TrackPalette& a, const GalaxyVisualizer::Palette& base) {
    GalaxyVisualizer::Palette p = base;   // keep the core and disc; arms and knots from the album
    Copy3(p.arm, a.mid);
    Copy3(p.knot, a.hot);
    return p;
}

} // namespace

GalaxyVisualizer::GalaxyVisualizer() : m_rng(std::random_device{}()) {
    m_from = m_to = kPalettes[0].p;
    m_shapeA = m_shapeB = kShapes[0];
    m_yaw = std::uniform_real_distribution<float>(0.0f, 6.28f)(m_rng);
    m_pattern = std::uniform_real_distribution<float>(0.0f, 6.28f)(m_rng);
    m_cam = kShots[0];
}

bool GalaxyVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    if (FAILED(device->CreateVertexShader(g_galaxy_vs, sizeof(g_galaxy_vs), nullptr, &m_starsVS)) ||
        FAILED(device->CreatePixelShader(g_galaxy_ps, sizeof(g_galaxy_ps), nullptr, &m_starsPS)) ||
        FAILED(device->CreatePixelShader(g_galaxy_out_ps, sizeof(g_galaxy_out_ps), nullptr, &m_outPS)))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(GalaxyConstants);
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
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT;   // dest - src
    if (FAILED(device->CreateBlendState(&bd, &m_subBlend))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&s, &m_sampler));
}

void GalaxyVisualizer::ReleaseResources() {
    m_srv.Reset();
    m_rtv.Reset();
    m_tex.Reset();
    m_sampler.Reset();
    m_subBlend.Reset();
    m_addBlend.Reset();
    m_cb.Reset();
    m_outPS.Reset();
    m_starsPS.Reset();
    m_starsVS.Reset();
    m_device.Reset();
    m_width = m_height = 0;
}

bool GalaxyVisualizer::CreateTarget(int w, int h) {
    m_srv.Reset();
    m_rtv.Reset();
    m_tex.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 0;   // full chain: the output pass samples small mips for bloom
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

GalaxyVisualizer::Palette GalaxyVisualizer::CurrentPalette() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Palette r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void GalaxyVisualizer::NextLook(float blendSeconds) {
    m_from = CurrentPalette();
    m_scheme = (m_scheme + 1 + std::uniform_int_distribution<int>(0, kPaletteCount - 2)(m_rng)) % kPaletteCount;
    m_to = m_hasArt && (m_scheme % 2 == 1) ? FromArt(m_art, kPalettes[m_scheme].p) : kPalettes[m_scheme].p;
    m_blend = 0.0f;
    m_blendSeconds = blendSeconds;

    // The shape morphs too, from wherever the last morph was heading (arm counts can't be
    // blended three ways, so a morph cut short jumps to its end first).
    m_shapeA = m_shapeB;
    m_shape = (m_shape + 1 + std::uniform_int_distribution<int>(0, kShapeCount - 2)(m_rng)) % kShapeCount;
    m_shapeB = kShapes[m_shape];
    m_shapeBlend = 0.0f;
    m_shapeSeconds = std::max(blendSeconds, 3.0f);

    m_timeSinceChange = 0.0f;
    m_beatsSinceChange = 0;
    m_changePending = false;
}

void GalaxyVisualizer::NextShot(int shot, bool cut) {
    if (shot < 0) shot = (m_shot + 1 + std::uniform_int_distribution<int>(0, kShotCount - 3)(m_rng)) % (kShotCount - 1);
    m_shot = shot;
    m_shotBeats = 0;
    m_shotTime = 0.0f;
    m_elevSign = (shot == 1 || shot == 4) && std::uniform_int_distribution<int>(0, 2)(m_rng) == 0 ? -1.0f : 1.0f;   // sometimes from below
    if (cut) {
        const Shot& t = kShots[shot];
        m_cam = t;
        m_cam.elev *= m_elevSign;
        m_yaw += std::uniform_real_distribution<float>(1.0f, 5.0f)(m_rng);
    }
    m_camTau = cut ? 2.0f : 2.8f;
}

void GalaxyVisualizer::Update(const AudioFeatures& audio, float dt) {
    m_time += dt;
    m_silent = audio.silent;
    m_gravity = Smooth(m_gravity, audio.silent ? 0.0f : audio.bass, dt, 0.06f, 0.5f);
    m_treble = Smooth(m_treble, std::max(audio.treble * 0.6f, audio.hat), dt, 0.05f, 0.3f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.6f, 0.4f);
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);

    // Beats: shockwaves through the arms, a zoom punch, and (when it's really hitting) a quasar burst.
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    bool beat = false;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; beat |= !gridLocked; }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; beat |= gridLocked; }
    m_quasarCooldown -= dt;
    if (audio.snareCount != m_lastSnare) {   // snares: a smaller ripple through the arms
        m_lastSnare = audio.snareCount;
        if (!audio.silent) {
            m_waves[m_nextWave][0] = 0.03f;
            m_waves[m_nextWave][1] = 0.3f;
            m_nextWave = (m_nextWave + 1) % 4;
        }
    }
    m_vocal = Smooth(m_vocal, audio.vocal, dt, 0.15f, 0.6f);
    if (beat && !audio.silent) {
        m_waves[m_nextWave][0] = 0.03f;
        m_waves[m_nextWave][1] = 0.6f + 0.9f * audio.bass;
        m_nextWave = (m_nextWave + 1) % 4;
        m_beatGlow = 1.0f;
        m_kick = std::max(m_kick, 0.35f + 0.8f * audio.bass);
        ++m_beatsSinceChange;
        ++m_shotBeats;
        if (audio.bass > 0.8125f && m_intensity > 0.5125f && m_quasarCooldown <= 0.0f) {
            m_quasar = std::max(m_quasar, 0.45f + 0.35f * audio.bass);   // harder hits, bigger bursts
            m_quasarCooldown = 10.5f;
        }
    }
    const float waveSpeed = 0.9f + 0.7f * audio.intensity;
    for (auto& w : m_waves) {
        w[0] += waveSpeed * dt;
        w[1] *= std::exp(-dt / 1.1f);
        if (w[0] > 1.8f) w[1] = 0.0f;
    }
    m_beatGlow *= std::exp(-dt / 0.25f);
    m_kick *= std::exp(-dt / 0.12f);

    // Drops: hard cut, the quasar fires, a flash, a huge shockwave and a new look.
    m_timeSinceChange += dt;
    if (audio.dropCount != m_lastDrop || m_fireQuasar) {
        m_lastDrop = audio.dropCount;
        m_fireQuasar = false;
        m_quasar = 1.0f;
        m_quasarCooldown = 6.0f;
        m_flash = 1.0f;
        m_waves[m_nextWave][0] = 0.02f;
        m_waves[m_nextWave][1] = 2.5f;
        m_nextWave = (m_nextWave + 1) % 4;
        NextShot(kShotQuasar, true);
        if (!m_locked && m_timeSinceChange > 4.0f) NextLook(2.5f);
    }
    m_quasar = std::max(m_quasar * std::exp(-dt / 2.8f), 0.25f * m_buildUp);
    m_flash *= std::exp(-dt / 0.35f);
    if (m_beatsSinceChange >= 32 && m_timeSinceChange > 20.0f) m_changePending = true;
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    m_lastBarPhase = audio.barPhase;
    if (!m_locked && ((m_changePending && (!gridLocked || onDownbeat)) || m_timeSinceChange > 75.0f)) NextLook(8.0f);
    m_blend = std::min(1.0f, m_blend + dt / m_blendSeconds);
    m_shapeBlend = std::min(1.0f, m_shapeBlend + dt / m_shapeSeconds);

    // Camera director: a new shot every 4 bars when it's intense (8 otherwise), on the downbeat.
    // Intense sections cut hard; calmer ones glide.
    m_shotTime += dt;
    const int shotBeats = m_intensity > 0.6f ? 16 : 32;
    const bool shotDue = gridLocked ? (m_shotBeats >= shotBeats && onDownbeat)
                                    : m_shotTime > (audio.silent ? 30.0f : 12.0f);
    if (shotDue) NextShot(-1, !audio.silent && m_intensity > 0.5f && std::uniform_int_distribution<int>(0, 1)(m_rng) == 0);

    Shot target = kShots[m_shot];
    if (m_shot == 0) {   // the wide orbit uses the look's preferred angle
        const float sb = m_shapeBlend * m_shapeBlend * (3.0f - 2.0f * m_shapeBlend);
        target.elev = m_shapeA.elevation + (m_shapeB.elevation - m_shapeA.elevation) * sb;
    }
    target.elev *= m_elevSign;
    target.dist *= 1.0f - 0.45f * m_buildUp;                       // build-ups push in
    target.roll += 0.25f * m_buildUp * std::sin(m_time * 0.7f);
    m_cam.dist = Smooth(m_cam.dist, target.dist, dt, m_camTau, m_camTau);
    m_cam.elev = Smooth(m_cam.elev, target.elev + 0.05f * std::sin(m_time * 0.11f), dt, m_camTau, m_camTau);
    m_cam.yawSpeed = Smooth(m_cam.yawSpeed, target.yawSpeed, dt, m_camTau, m_camTau);
    m_cam.focal = Smooth(m_cam.focal, target.focal, dt, m_camTau, m_camTau);
    m_cam.roll = Smooth(m_cam.roll, target.roll, dt, m_camTau, m_camTau);
    m_cam.side = Smooth(m_cam.side, target.side, dt, m_camTau, m_camTau);
    const float tf = audio.tempoFactor;   // faster songs animate faster
    m_yaw += dt * tf * m_cam.yawSpeed * (audio.silent ? 0.5f : 1.0f + 1.2f * m_gravity + 2.0f * m_buildUp);

    // Galaxy motion: gravity (bass) spins everything faster.
    const float spin = audio.silent ? 0.03f : 0.05f + 0.25f * m_gravity + 0.15f * m_buildUp;
    m_pattern += dt * tf * spin;
    m_accSpin += dt * tf * (audio.silent ? 0.6f : 1.2f + 4.0f * m_gravity + 3.0f * m_quasar);
    m_anim += dt;
}

void GalaxyVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    ID3D11DeviceContext* ctx = renderer.Context();
    if ((w != m_width || h != m_height) && !CreateTarget(w, h)) return;

    // Camera: orbits a target (the black hole, or a point beside it for flybys), with roll and
    // a punch of zoom and shake on each kick.
    GalaxyConstants c{};
    const float ce = std::cos(m_cam.elev), se = std::sin(m_cam.elev);
    const V3 dir{ ce * std::sin(m_yaw), se, ce * std::cos(m_yaw) };          // from target to camera
    const V3 sideAxis = Normalize(Cross(V3{ 0, 1, 0 }, V3{ -dir.x, 0.0f, -dir.z }));
    const V3 tgt = sideAxis * m_cam.side;
    const float shake = 0.006f * m_kick * m_cam.dist;
    V3 pos = tgt + dir * m_cam.dist;
    const V3 fwd = Normalize(tgt - pos);
    V3 right = Normalize(Cross(V3{ 0, 1, 0 }, fwd));
    V3 up = Cross(fwd, right);
    pos = pos + right * (shake * std::sin(m_time * 61.0f)) + up * (shake * std::cos(m_time * 47.0f));
    const float roll = m_cam.roll + 0.02f * m_kick * std::sin(m_time * 37.0f);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const V3 r2 = right * cr + up * sr, u2 = up * cr - right * sr;
    right = r2; up = u2;
    c.camPos[0] = pos.x; c.camPos[1] = pos.y; c.camPos[2] = pos.z;
    c.camRight[0] = right.x; c.camRight[1] = right.y; c.camRight[2] = right.z;
    c.camUp[0] = up.x; c.camUp[1] = up.y; c.camUp[2] = up.z;
    c.camFwd[0] = fwd.x; c.camFwd[1] = fwd.y; c.camFwd[2] = fwd.z;
    c.focal = m_cam.focal * (1.0f + 0.07f * m_kick);
    c.aspect = static_cast<float>(w) / h;

    // Where the black hole and the quasar beams land on screen (height units, y down).
    auto project = [&](V3 p, float out[2]) {
        const V3 v = p - pos;
        const float z = Dot(v, fwd);
        out[0] = 0.5f * Dot(v, right) * c.focal / z;
        out[1] = -0.5f * Dot(v, up) * c.focal / z;
        return z;
    };
    const float kNear = 0.05f;
    const float zbh = project(V3{ 0, 0, 0 }, c.bhPos);
    const float beamLen = 0.4f + 2.6f * m_quasar;
    if (zbh > kNear) {
        c.horizon = 0.010f * c.focal / zbh * 0.5f;
        c.beamWidth = 0.012f * c.focal / zbh * 0.5f;
        for (int s = 0; s < 2; ++s) {
            V3 tip{ 0.0f, s == 0 ? beamLen : -beamLen, 0.0f };
            const float zt = Dot(tip - pos, fwd);
            if (zt < kNear) tip = tip * ((zbh - kNear) / (zbh - zt));   // clip at the near plane
            project(tip, s == 0 ? c.jetA : c.jetB);
        }
        c.quasar = m_quasar;
    } else {
        c.horizon = 0.0f;
        c.quasar = 0.0f;
        c.bhPos[0] = c.bhPos[1] = 100.0f;
    }

    // Same total light whatever the resolution (points are one pixel each).
    c.starBright = 0.06f * (static_cast<float>(w) * h) / (1920.0f * 1080.0f) * (m_silent ? 0.7f : 1.0f);
    c.pattern = m_pattern;
    const float sb = m_shapeBlend * m_shapeBlend * (3.0f - 2.0f * m_shapeBlend);
    const float sa[4] = { m_shapeA.arms, m_shapeA.twist, m_shapeA.width, m_shapeA.bulge };
    const float sbv[4] = { m_shapeB.arms, m_shapeB.twist, m_shapeB.width, m_shapeB.bulge };
    memcpy(c.shapeA, sa, sizeof(sa));
    memcpy(c.shapeB, sbv, sizeof(sbv));
    c.thickA = m_shapeA.thickness;
    c.thickB = m_shapeB.thickness;
    c.shapeBlend = sb;
    c.gravity = m_gravity;
    for (int i = 0; i < 4; ++i) { c.waves[i][0] = m_waves[i][0]; c.waves[i][1] = m_waves[i][1]; }
    const Palette p = CurrentPalette();
    Copy3(c.core, p.core);
    Copy3(c.arm, p.arm);
    Copy3(c.knot, p.knot);
    Copy3(c.disc, p.disc);
    c.accretion = 0.6f + 1.6f * m_gravity + 0.8f * m_beatGlow + 1.5f * m_quasar + m_flash;
    c.twinkle = m_treble;
    c.jet = m_quasar;
    c.animTime = m_anim;
    c.accSpin = m_accSpin;
    c.exposure = 1.0f;
    c.flash = m_flash;
    c.dustAmount = c.starBright * 2.0f;
    Copy3(c.sky, p.sky);
    c.glow = (m_silent ? 0.7f : 1.0f) * (1.0f + 0.5f * m_beatGlow + 0.8f * m_flash + 0.3f * m_quasar + 0.25f * m_vocal);
    c.kick = m_kick;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(m_cb.Get(), 0);
    }
    ID3D11Buffer* cb = m_cb.Get();

    // 1. Stars, additively, one point each.
    renderer.BindTarget(m_rtv.Get(), w, h, time, dt);
    const float black[4] = {};
    ctx->ClearRenderTargetView(m_rtv.Get(), black);
    ctx->OMSetBlendState(m_addBlend.Get(), nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->VSSetShader(m_starsVS.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShader(m_starsPS.Get(), nullptr, 0);
    ctx->Draw(kParticles, 0);
    ctx->OMSetBlendState(m_subBlend.Get(), nullptr, 0xFFFFFFFF);   // dust lanes take light away
    ctx->Draw(kDust, kParticles);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // 2. Bloom mips, then lensing and tone mapping into the host's target.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(m_srv.Get());
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11ShaderResourceView* srv = m_srv.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_outPS.Get());
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

bool GalaxyVisualizer::OnKey(UINT vk) {
    if (vk == 'N') {
        NextLook(2.5f);
        m_message = m_hasArt && (m_scheme % 2 == 1) ? L"Album colours" : kPalettes[m_scheme].name;
        return true;
    }
    if (vk == 'Q') {   // fire the quasar, as on a drop
        m_fireQuasar = true;
        return true;
    }
    if (vk == 'M') {   // next camera shot
        NextShot(-1, true);
        m_message = L"New shot";
        return true;
    }
    if (vk == VK_SPACE) {
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    }
    return false;
}

std::wstring GalaxyVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring GalaxyVisualizer::Status() const {
    return std::wstring(m_hasArt && (m_scheme % 2 == 1) ? L"Album colours" : kPalettes[m_scheme].name) +
           L", " + std::to_wstring(static_cast<int>(m_shapeB.arms)) + L" arms" + (m_locked ? L" (locked)" : L"");
}

void GalaxyVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Palette p = CurrentPalette();
    Copy3(lo, p.arm);
    Copy3(hi, p.knot);
}

void GalaxyVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked && m_timeSinceChange > 10.0f) NextLook(6.0f);
}
