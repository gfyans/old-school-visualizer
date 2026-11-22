#include "viz/glasscity/GlassCityVisualizer.h"
#include "render/Renderer.h"
#include "g_glasscity_ps.h"
#include "g_glasscity_post_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct GlassCityConstants {   // matches cbuffer GlassCity in glasscity_ps.hlsl
    float camPos[3];   float focal;
    float camRight[3]; float time;
    float camUp[3];    float speed;
    float camFwd[3];   float ceiling;
    float edge[3];     float lights;
    float tile[3];     float current;
    float spark[3];    float sparkle;
    float floorCol[3]; float buildUp;
    float fog[3];      float over;
    float glass[3];    float shockR;
    float ringAge[4];
    float ringHit[4];
    float ringZ[4];
    float shockPos[2]; float laneX;  float tileSeed;
    float streakPhase, streakAmt, streakLen, pad;
};
static_assert(sizeof(GlassCityConstants) == sizeof(float) * 60);

struct GlassPostConstants {   // matches cbuffer GlassPost in glasscity_post_ps.hlsl
    float vanish[2]; float blur; float bloom;
    float flash, aberr, exposure, grain;
};
static_assert(sizeof(GlassPostConstants) == sizeof(float) * 8);

constexpr float kPi = 3.14159265f;
constexpr int kAvenue = 8;
constexpr float kPeriod = 3600.0f;   // matches glasscity_ps.hlsl

struct NamedPalette { const wchar_t* name; GlassCityVisualizer::Palette p; };
// edges, tiles, warm sparks, floor rings, haze, glass tint
const NamedPalette kLooks[] = {
    { L"Nightfall",  { { 0.25f, 0.75f, 1.00f }, { 1.00f, 0.55f, 0.18f }, { 1.00f, 0.70f, 0.30f }, { 0.60f, 0.30f, 1.00f }, { 0.16f, 0.10f, 0.32f }, { 0.60f, 0.70f, 1.00f } } },
    { L"Ice",        { { 0.70f, 0.90f, 1.00f }, { 0.55f, 0.82f, 1.00f }, { 1.00f, 0.60f, 0.20f }, { 0.30f, 0.60f, 1.00f }, { 0.14f, 0.24f, 0.34f }, { 0.70f, 0.85f, 1.00f } } },
    { L"Datastream", { { 0.10f, 0.60f, 1.00f }, { 0.20f, 0.80f, 1.00f }, { 1.00f, 0.55f, 0.15f }, { 0.10f, 0.40f, 1.00f }, { 0.04f, 0.22f, 0.55f }, { 0.30f, 0.60f, 1.00f } } },
    { L"Synth",      { { 1.00f, 0.30f, 0.80f }, { 0.20f, 0.90f, 1.00f }, { 1.00f, 0.40f, 0.90f }, { 0.70f, 0.20f, 1.00f }, { 0.32f, 0.07f, 0.28f }, { 0.90f, 0.50f, 1.00f } } },
    { L"Ember",      { { 1.00f, 0.50f, 0.15f }, { 1.00f, 0.75f, 0.40f }, { 1.00f, 0.95f, 0.80f }, { 1.00f, 0.30f, 0.10f }, { 0.28f, 0.09f, 0.04f }, { 1.00f, 0.70f, 0.50f } } },
};
constexpr int kLookCount = static_cast<int>(std::size(kLooks));
const wchar_t* kShots[] = { L"Avenue", L"Skim", L"Rooftops", L"Crown", L"Weave" };

float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}
void Copy3(float* d, const float* s) { memcpy(d, s, 3 * sizeof(float)); }

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(Dot(a, a), 1e-12f))); }

} // namespace

GlassCityVisualizer::GlassCityVisualizer() : m_rng(std::random_device{}()) {
    m_look = std::uniform_int_distribution<int>(0, kLookCount - 1)(m_rng);
    m_from = m_to = MakePalette(m_look);
    m_ceilingTarget = m_ceiling = std::uniform_int_distribution<int>(0, 2)(m_rng) == 0 ? 1.0f : 0.0f;
    m_z = std::uniform_real_distribution<float>(0.0f, kPeriod)(m_rng);
    // Developer switch: VISUALIZER_GLASSCITY=<look 0-4>,<shot 0-4>[,<ceiling 0/1>] fixes the look and shot.
    wchar_t env[32];
    if (GetEnvironmentVariableW(L"VISUALIZER_GLASSCITY", env, ARRAYSIZE(env)) > 0) {
        int look = 0, shot = 0, ceiling = -1;
        if (swscanf_s(env, L"%d,%d,%d", &look, &shot, &ceiling) >= 2 && look >= 0 && look < kLookCount && shot >= 0 && shot < kShotCount) {
            m_look = look;
            m_from = m_to = MakePalette(look);
            SetShot(shot);
            m_locked = true;
            if (ceiling >= 0) m_ceiling = m_ceilingTarget = ceiling ? 1.0f : 0.0f;
        }
    }
}

bool GlassCityVisualizer::CreateResources(ID3D11Device* device) {
    m_device = device;
    if (FAILED(device->CreatePixelShader(g_glasscity_ps, sizeof(g_glasscity_ps), nullptr, &m_ps))) return false;
    if (FAILED(device->CreatePixelShader(g_glasscity_post_ps, sizeof(g_glasscity_post_ps), nullptr, &m_postPs))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(GlassCityConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_cb))) return false;
    cb.ByteWidth = sizeof(GlassPostConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_postCb))) return false;
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&s, &m_clamp));
}

void GlassCityVisualizer::ReleaseResources() {
    m_scene.Reset();
    m_clamp.Reset();
    m_postCb.Reset();
    m_cb.Reset();
    m_postPs.Reset();
    m_ps.Reset();
    m_device.Reset();
}

GlassCityVisualizer::Palette GlassCityVisualizer::MakePalette(int look) const {
    Palette p = kLooks[look].p;
    if (m_hasArt) {   // album colours lean into the tiles and the floor rings; the edges keep the look
        for (int i = 0; i < 3; ++i) {
            p.tile[i] = 0.4f * p.tile[i] + 0.7f * m_art.hot[i];
            p.floor[i] = 0.5f * p.floor[i] + 0.5f * m_art.mid[i];
        }
    }
    return p;
}

GlassCityVisualizer::Palette GlassCityVisualizer::CurrentPalette() const {
    const float t = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    Palette r;
    const float* a = reinterpret_cast<const float*>(&m_from);
    const float* b = reinterpret_cast<const float*>(&m_to);
    float* o = reinterpret_cast<float*>(&r);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(float); ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

void GlassCityVisualizer::NextLook(bool instant) {
    m_look = (m_look + 1 + std::uniform_int_distribution<int>(0, kLookCount - 2)(m_rng)) % kLookCount;
    m_from = instant ? MakePalette(m_look) : CurrentPalette();
    m_to = MakePalette(m_look);
    m_blend = instant ? 1.0f : 0.0f;
    m_ceilingTarget = std::uniform_int_distribution<int>(0, 2)(m_rng) == 0 ? 1.0f : 0.0f;
}

void GlassCityVisualizer::SetShot(int shot) {
    m_shot = shot;
    m_shotTime = 0.0f;
    m_beatsInShot = 0;
    m_side = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1 : -1;
    // From up high, sometimes hop to another avenue (applied once the camera is above the roofs).
    if ((shot == Rooftops || shot == Crown) && std::uniform_int_distribution<int>(0, 1)(m_rng)) {
        int step = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1 : -1;
        if (std::abs(m_lane + step) > 4) step = -step;
        m_pendingLane = m_lane + step;
    }
}

void GlassCityVisualizer::PickShot() {
    int next;
    do next = std::uniform_int_distribution<int>(0, kShotCount - 1)(m_rng);
    while (next == m_shot);
    SetShot(next);
    // Big sections sometimes tilt the world 45 degrees for a shot; otherwise come back level.
    if (m_over < 0.3f && m_turnT >= 1.0f) {
        const bool tilted = std::fabs(m_turn) > 0.01f;
        if (tilted) Turn(-m_turn, 1.6f);
        else if (m_intensity > 0.6f && std::uniform_int_distribution<int>(0, 2)(m_rng) == 0)
            Turn((std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f) * kPi * 0.25f, 1.6f);
    }
}

void GlassCityVisualizer::Turn(float radians, float seconds) {
    m_turnFrom = m_turn;
    m_turnTo = m_turn + radians;
    m_turnT = 0.0f;
    m_turnDur = std::max(seconds, 0.2f);
}

void GlassCityVisualizer::Overdrive() {
    m_hyper = 1.0f;
    m_flash = 1.0f;
    m_aberr = 1.0f;
    m_over = 1.0f;
    m_shockR = 0.01f;
    m_shockX = m_x;
    m_shockZ = m_z;
    m_sinceOver = 0.0f;
    m_peakBuild = 0.0f;
    m_snap = 1.5f;
    // A half or quarter barrel roll as it hits.
    const float dir = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f;
    Turn(dir * (std::uniform_int_distribution<int>(0, 2)(m_rng) ? kPi : kPi * 0.5f), 1.3f);
    if (!m_locked) {
        NextLook(true);   // behind the flash
        SetShot(std::uniform_int_distribution<int>(0, 1)(m_rng) ? Avenue : Weave);   // dive down into the avenue
    }
}

void GlassCityVisualizer::Update(const AudioFeatures& audio, float dt) {
    const float tf = audio.tempoFactor;
    m_silent = audio.silent;
    m_time += dt;
    m_shotTime += dt;
    m_sinceOver += dt;
    m_intensity = Smooth(m_intensity, audio.silent ? 0.0f : audio.intensity, dt, 1.0f, 2.0f);
    m_bass = Smooth(m_bass, audio.silent ? 0.0f : audio.bass, dt, 0.05f, 0.3f);
    m_buildUp = Smooth(m_buildUp, audio.buildUp, dt, 0.6f, 0.5f);
    m_hat = Smooth(m_hat, std::max(audio.hat, audio.treble * 0.4f), dt, 0.02f, 0.15f);
    m_vocal = Smooth(m_vocal, audio.vocal, dt, 0.2f, 0.8f);
    for (float& a : m_ringAge) a += dt;

    // Beats: a ring of light from ahead (on the grid when it's locked, else on heard kicks).
    const bool gridLocked = audio.tempoConfidence > 0.4f;
    bool beat = false;
    if (audio.beatCount != m_lastBeat) { m_lastBeat = audio.beatCount; beat |= !gridLocked; }
    if (audio.gridBeat != m_lastGrid) { m_lastGrid = audio.gridBeat; beat |= gridLocked; }
    if (beat && !audio.silent) {
        for (int i = 3; i > 0; --i) { m_ringAge[i] = m_ringAge[i - 1]; m_ringHit[i] = m_ringHit[i - 1]; m_ringZ[i] = m_ringZ[i - 1]; }
        m_ringAge[0] = 0.0f;
        m_ringHit[0] = std::min(1.4f, 0.35f + 0.7f * audio.bass + 0.4f * m_over);
        m_ringZ[0] = m_z + 8.0f + 0.8f * m_speed;
        ++m_beatsInShot;
    }
    if (audio.snareCount != m_lastSnare) { m_lastSnare = audio.snareCount; ++m_tileSeed; }
    const bool onDownbeat = audio.barPhase < m_lastBarPhase;
    const bool onBeat3 = m_lastBarPhase < 0.5f && audio.barPhase >= 0.5f;
    m_lastBarPhase = audio.barPhase;

    // Overdrive: on a drop; when a big build-up lets go; or after a long big stretch without either.
    bool fire = m_fire;
    m_fire = false;
    if (audio.dropCount != m_lastDrop) { m_lastDrop = audio.dropCount; fire = true; }
    if (!audio.silent && m_hyper < 0.1f) {
        m_peakBuild = std::max(m_peakBuild, m_buildUp);
        if (m_peakBuild > 0.55f && audio.buildUp < 0.2f && m_intensity > 0.35f && m_sinceOver > 8.0f) fire = true;
        else if (m_peakBuild > 0.0f && m_buildUp < 0.05f) m_peakBuild = 0.0f;
        if (m_sinceOver > 60.0f && m_intensity > 0.45f && (onDownbeat || !gridLocked)) fire = true;
    }
    if (fire) Overdrive();
    m_hyper *= std::exp(-dt / 1.3f);
    m_flash *= std::exp(-dt / 0.22f);
    m_aberr *= std::exp(-dt / 0.6f);
    m_over *= std::exp(-dt / (m_intensity > 0.5f ? 14.0f : 3.0f));   // stays supercharged while the song is big
    if (m_shockR > 0.0f && (m_shockR += dt * 75.0f) > 230.0f) m_shockR = 0.0f;
    m_snap = std::max(0.0f, m_snap - dt);

    // Rolls. In Overdrive: every two bars, start on beat 3 and land on the next downbeat (or every few
    // seconds without a beat grid). Once it's over, ease back to level (a 45-degree tilt can stay).
    if (onBeat3) ++m_halfBars;
    if (m_turnT >= 1.0f && m_hyper < 0.5f && !audio.silent) {
        const bool due = gridLocked ? (onBeat3 && m_halfBars % 2 == 0) : std::fmod(m_time, 4.0f) < dt;
        if (m_over > 0.3f && due) {
            const int r = std::uniform_int_distribution<int>(0, 19)(m_rng);
            const float angle = r < 10 ? 0.25f * kPi : r < 17 ? 0.5f * kPi : kPi;
            const float dir = std::uniform_int_distribution<int>(0, 1)(m_rng) ? 1.0f : -1.0f;
            const float halfBar = gridLocked && audio.bpm > 0.0f ? 120.0f / audio.bpm : 0.9f;
            Turn(dir * angle, std::clamp(halfBar, 0.5f, 1.4f));
        } else if (m_over < 0.25f && std::fabs(m_turn) > 0.25f * kPi + 0.01f) {
            Turn(-m_turn, 1.6f);
        }
    }
    if (m_turnT < 1.0f) {
        m_turnT = std::min(1.0f, m_turnT + dt / m_turnDur);
        const float t = m_turnT, e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);   // smootherstep
        m_turn = m_turnFrom + (m_turnTo - m_turnFrom) * e;
        if (m_turnT >= 1.0f) m_turn = m_turnTo = m_turnFrom = std::remainder(m_turnTo, 2.0f * kPi);
    }

    // The director: a new shot every 8 bars (4 when it's big), on the downbeat.
    const bool big = m_intensity > 0.6f;
    if (!m_locked && m_hyper < 0.3f &&
        ((m_beatsInShot >= (big ? 16 : 32) && onDownbeat && m_shotTime > 6.0f) || (!gridLocked && m_shotTime > (big ? 12.0f : 20.0f)) ||
         m_shotTime > 40.0f))
        PickShot();
    m_blend = std::min(1.0f, m_blend + dt / 3.0f);
    m_ceiling = Smooth(m_ceiling, m_ceilingTarget, dt, 2.5f, 2.5f);

    // The flight: faster when the song is big and during build-ups; hyperspeed on Overdrive.
    const float cruise = audio.silent ? 1.2f : (2.2f + 4.5f * m_intensity + 5.0f * m_buildUp * m_buildUp + 2.0f * m_over) * tf;
    m_speed = Smooth(m_speed, cruise, dt, 1.5f, 2.5f);
    const float v = m_speed + 45.0f * m_hyper * m_hyper;
    m_z += dt * v;
    if (m_z > kPeriod) {   // the city repeats: wrap without a seam
        m_z -= kPeriod;
        m_shockZ -= kPeriod;
        for (float& z : m_ringZ) z -= kPeriod;
    }
    m_streakPhase += dt * (0.08f + 0.02f * v);

    // Where the shot wants the camera.
    const float bar = gridLocked ? audio.barPhase : std::fmod(m_time * 0.25f, 1.0f);
    float height = 0.55f, offset = 0.0f, pitch = 0.0f;
    switch (m_shot) {
    case Avenue: height = 0.55f + 0.1f * std::sin(m_time * 0.3f); break;
    case Skim: height = 0.2f; offset = 0.6f * m_side; pitch = 0.03f; break;
    case Rooftops: height = 3.7f; pitch = -0.14f; break;
    case Crown:
        if (m_ceilingTarget > 0.5f) { height = 6.3f; pitch = 0.07f; }
        else { height = 8.4f; pitch = -0.34f; }   // stays inside the city's slab (below 9)
        break;
    default: height = 1.0f; offset = 0.5f * std::sin(2.0f * kPi * bar); pitch = 0.0f; break;   // Weave: a slalom in time
    }
    if (m_pendingLane != m_lane && m_h > 3.2f) m_lane = m_pendingLane;
    const float tau = m_snap > 0.0f ? 0.35f : 1.1f;
    const float prevX = m_x;
    m_h = std::min(Smooth(m_h, height, dt, tau, tau), 8.6f);
    m_x = Smooth(m_x, kAvenue * m_lane + 1.0f + offset, dt, m_shot == Weave ? 0.25f : tau, m_shot == Weave ? 0.25f : tau);
    m_pitch = Smooth(m_pitch, pitch, dt, tau, tau);
    const float vx = (m_x - prevX) / std::max(dt, 1e-4f);
    m_roll = Smooth(m_roll, std::clamp(-vx * 0.1f, -0.45f, 0.45f), dt, 0.3f, 0.3f);
    m_yaw = Smooth(m_yaw, std::clamp(vx * 0.03f, -0.2f, 0.2f), dt, 0.4f, 0.4f);

    const V3 pos{ m_x, m_h, m_z };
    const V3 fwd = Normalize({ std::sin(m_yaw), m_pitch, std::cos(m_yaw) });
    const V3 right0 = Normalize(Cross({ 0, 1, 0 }, fwd));
    const V3 up0 = Cross(fwd, right0);
    const float roll = m_roll + m_turn + (audio.silent ? 0.0f : 0.035f * std::sin(2.0f * kPi * bar));   // a gentle sway with the bar
    const float cr = std::cos(roll), sr = std::sin(roll);
    const V3 right = right0 * cr + up0 * sr, up = up0 * cr + right0 * -sr;

    const Palette pal = CurrentPalette();
    GlassCityConstants c{};
    c.camPos[0] = pos.x; c.camPos[1] = pos.y; c.camPos[2] = pos.z;
    c.camRight[0] = right.x; c.camRight[1] = right.y; c.camRight[2] = right.z;
    c.camUp[0] = up.x; c.camUp[1] = up.y; c.camUp[2] = up.z;
    c.camFwd[0] = fwd.x; c.camFwd[1] = fwd.y; c.camFwd[2] = fwd.z;
    c.focal = 1.25f * (1.0f - 0.38f * m_hyper - 0.08f * m_buildUp);
    c.time = std::fmod(m_time, 3000.0f);
    c.speed = v;
    c.ceiling = m_ceiling;
    Copy3(c.edge, pal.edge);
    Copy3(c.tile, pal.tile);
    Copy3(c.spark, pal.spark);
    Copy3(c.floorCol, pal.floor);
    Copy3(c.fog, pal.fog);
    Copy3(c.glass, pal.glass);
    c.lights = audio.silent ? 0.15f : 0.25f + 0.35f * m_intensity + 0.15f * m_vocal + 0.15f * m_over;
    c.current = audio.silent ? 0.15f : 0.3f + 0.6f * m_intensity + 0.8f * m_buildUp + 0.8f * m_over;
    c.sparkle = m_hat;
    c.buildUp = m_buildUp;
    c.over = m_over;
    c.shockR = m_shockR;
    memcpy(c.ringAge, m_ringAge, sizeof(c.ringAge));
    memcpy(c.ringHit, m_ringHit, sizeof(c.ringHit));
    memcpy(c.ringZ, m_ringZ, sizeof(c.ringZ));
    c.shockPos[0] = m_shockX;
    c.shockPos[1] = m_shockZ;
    c.laneX = kAvenue * m_lane + 1.0f;
    c.tileSeed = static_cast<float>(m_tileSeed % 4096);
    c.streakPhase = std::fmod(m_streakPhase, 10000.0f);
    c.streakAmt = audio.silent ? 0.1f : std::min(1.0f, 0.2f + 0.5f * m_intensity + 0.4f * m_buildUp + 0.6f * m_over + m_hyper);
    c.streakLen = 1.5f + 0.25f * v;
    memcpy(m_constants, &c, sizeof(c));
    Copy3(m_camFwd, c.camFwd);
    Copy3(m_camRight, c.camRight);
    Copy3(m_camUp, c.camUp);
    m_focal = c.focal;

    GlassPostConstants p{};
    p.blur = std::min(0.45f, 0.002f * m_speed + 0.3f * m_hyper);
    p.bloom = 1.0f + 0.5f * m_buildUp + 0.4f * m_over + 0.8f * m_hyper;
    p.flash = 0.4f * m_flash;
    p.aberr = 0.002f + 0.025f * m_aberr + 0.004f * m_buildUp;
    p.exposure = 0.9f + 0.2f * m_over;
    p.grain = 0.025f;
    memcpy(m_post, &p, sizeof(p));
}

void GlassCityVisualizer::Render(Renderer& renderer, float time, float dt) {
    ID3D11DeviceContext* ctx = renderer.Context();
    ID3D11RenderTargetView* hostTarget = renderer.BoundTarget();
    const int w = renderer.BoundWidth(), h = renderer.BoundHeight();
    if (m_scene.width != w || m_scene.height != h) m_scene.Create(m_device.Get(), w, h, DXGI_FORMAT_R11G11B10_FLOAT, true);
    if (!m_scene.rtv) return;

    // The vanishing point (straight down the flight path) on screen, for the speed blur.
    GlassPostConstants post;
    memcpy(&post, m_post, sizeof(post));
    {
        const float aspect = static_cast<float>(w) / std::max(h, 1);
        const float z = std::max(m_camFwd[2], 0.2f);
        post.vanish[0] = std::clamp((m_focal * m_camRight[2] / z / aspect + 1.0f) * 0.5f, 0.0f, 1.0f);
        post.vanish[1] = std::clamp((1.0f - m_focal * m_camUp[2] / z) * 0.5f, 0.0f, 1.0f);
    }

    auto upload = [&](ID3D11Buffer* buffer, const void* data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, data, size);
            ctx->Unmap(buffer, 0);
        }
    };
    upload(m_cb.Get(), m_constants, sizeof(GlassCityConstants));
    upload(m_postCb.Get(), &post, sizeof(post));

    // Pass 1: the city, in HDR.
    renderer.BindTarget(m_scene.rtv.Get(), w, h, time, dt);
    ID3D11Buffer* cb = m_cb.Get();
    ctx->PSSetConstantBuffers(2, 1, &cb);
    renderer.DrawFullscreen(m_ps.Get());

    // Pass 2: speed blur, bloom and tone mapping into the host's target.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->GenerateMips(m_scene.srv.Get());
    renderer.BindTarget(hostTarget, w, h, time, dt);
    ID3D11Buffer* postCb = m_postCb.Get();
    ID3D11ShaderResourceView* srv = m_scene.srv.Get();
    ID3D11SamplerState* clamp = m_clamp.Get();
    ctx->PSSetConstantBuffers(3, 1, &postCb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &clamp);
    renderer.DrawFullscreen(m_postPs.Get());
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

bool GlassCityVisualizer::OnKey(UINT vk) {
    switch (vk) {
    case 'N':
        NextLook(false);
        m_message = kLooks[m_look].name;
        return true;
    case 'M':
        PickShot();
        m_message = kShots[m_shot];
        return true;
    case 'Q':
        m_fire = true;
        return true;
    case VK_SPACE:
        m_locked = !m_locked;
        m_message = m_locked ? L"Look locked" : L"Look unlocked";
        return true;
    default:
        return false;
    }
}

std::wstring GlassCityVisualizer::TakeMessage() {
    std::wstring m;
    m.swap(m_message);
    return m;
}

std::wstring GlassCityVisualizer::Status() const {
    return std::wstring(kLooks[m_look].name) + (m_ceilingTarget > 0.5f ? L" (canyon)" : L"") + L" · " + kShots[m_shot] +
           (m_over > 0.3f ? L" · overdrive" : L"") + (m_locked ? L" (locked)" : L"");
}

void GlassCityVisualizer::AccentColours(float lo[3], float hi[3]) const {
    const Palette p = CurrentPalette();
    Copy3(lo, p.edge);
    Copy3(hi, p.tile);
}

void GlassCityVisualizer::SetTrackPalette(const TrackPalette* palette, bool newTrack) {
    m_hasArt = palette != nullptr;
    if (palette) m_art = *palette;
    if (newTrack && !m_locked) NextLook(false);
    else m_to = MakePalette(m_look);
}
