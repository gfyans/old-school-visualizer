#include "ui/Overlay.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {
// Layout is authored for a 1080p-high window and scaled to the real size.
constexpr float kDesignHeight = 1080.0f;

const wchar_t* const kHelpLines[][2] = {
    { L"←  →", L"Previous / next visualiser" },
    { L"F  or  Alt+Enter", L"Toggle fullscreen (or double-click)" },
    { L"N", L"Next look (colours and shapes)" },
    { L"Space", L"Lock / unlock the current look" },
    { L"B", L"Equaliser bars along the bottom" },
    { L"D", L"Cycle audio device" },
    { L"A", L"Album-art colours on / off" },
    { L"T", L"Track titles on / off" },
    { L"F1", L"Stats" },
    { L"H", L"This help" },
    { L"Esc", L"Leave fullscreen / quit" },
};
}

bool Overlay::Init() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()))))
        return false;
    auto make = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        return SUCCEEDED(m_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", out));
    };
    if (!make(L"Segoe UI", 44.0f, DWRITE_FONT_WEIGHT_LIGHT, &m_titleFormat) ||
        !make(L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_NORMAL, &m_bodyFormat) ||
        !make(L"Consolas", 18.0f, DWRITE_FONT_WEIGHT_NORMAL, &m_monoFormat) ||
        !make(L"Segoe UI", 34.0f, DWRITE_FONT_WEIGHT_SEMI_LIGHT, &m_trackFormat) ||
        !make(L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_NORMAL, &m_artistFormat))
        return false;
    for (IDWriteTextFormat* f : { m_trackFormat.Get(), m_artistFormat.Get() }) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        m_dwrite->CreateEllipsisTrimmingSign(f, &ellipsis);
        f->SetTrimming(&trim, ellipsis.Get());   // long titles end in "…"
    }
    m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_monoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

bool Overlay::CreateDeviceResources(ID2D1DeviceContext* ctx) {
    m_brush.Reset();
    return ctx && SUCCEEDED(ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush));
}

void Overlay::ReleaseDeviceResources() { m_brush.Reset(); }

void Overlay::ShowMessage(const std::wstring& text, float seconds) {
    m_message = text;
    m_messageTime = 0.0f;
    m_messageDuration = seconds;
}

void Overlay::ShowTrack(const std::wstring& title, const std::wstring& artist, float seconds) {
    m_trackTitle = title;
    m_trackArtist = artist;
    m_trackTime = 0.0f;
    m_trackDuration = seconds;
}

void Overlay::Update(float dt) {
    if (!m_message.empty()) {
        m_messageTime += dt;
        if (m_messageTime > m_messageDuration) m_message.clear();
    }
    if (!m_trackTitle.empty()) {
        m_trackTime += dt;
        if (m_trackTime > m_trackDuration) m_trackTitle.clear();
    }
}

bool Overlay::HasContent(bool stats, bool help) const {
    return stats || help || !m_message.empty() || !m_trackTitle.empty();
}

void Overlay::DrawText(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* format,
                       const D2D1_RECT_F& rect, float opacity) {
    // Soft drop shadow keeps text readable over bright visuals.
    m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f * opacity));
    const D2D1_RECT_F shadow{ rect.left + 2, rect.top + 2, rect.right + 2, rect.bottom + 2 };
    ctx->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, shadow, m_brush.Get());
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * opacity));
    ctx->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, m_brush.Get());
}

void Overlay::Draw(ID2D1DeviceContext* ctx, int width, int height, const OverlayStats& stats,
                   const AudioFeatures& audio, bool showStats, bool showHelp, float bottomInset) {
    if (!m_brush || !m_dwrite) return;
    const float scale = height / kDesignHeight;
    const float w = width / scale;   // layout width in design units
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (!m_message.empty()) {
        // Fade in 0.2 s, hold, fade out over the last 0.8 s.
        const float t = m_messageTime;
        const float opacity = std::min({ 1.0f, t / 0.2f, (m_messageDuration - t) / 0.8f });
        const float bottom = kDesignHeight - 80 - bottomInset;
        DrawText(ctx, m_message, m_titleFormat.Get(), D2D1::RectF(0, bottom - 70, w, bottom),
                 std::max(opacity, 0.0f));
    }
    if (!m_trackTitle.empty()) {
        // Now playing: top-left (top-right while the stats panel is there). Slides in gently.
        const float t = m_trackTime;
        const float opacity = std::max(0.0f, std::min({ 1.0f, t / 0.6f, (m_trackDuration - t) / 1.5f }));
        const float slide = 12.0f * (1.0f - std::min(1.0f, t / 0.6f));
        const float boxW = std::min(900.0f, w * 0.6f);
        const float x = showStats ? w - boxW - 48.0f : 56.0f;
        const float y = 52.0f;
        // Accent bar to the left of the text.
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.55f * opacity));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 18 + slide, y + 8, x - 14 + slide, y + 80), 2, 2), m_brush.Get());
        DrawText(ctx, m_trackTitle, m_trackFormat.Get(), D2D1::RectF(x + slide, y, x + boxW, y + 48), opacity);
        DrawText(ctx, m_trackArtist, m_artistFormat.Get(), D2D1::RectF(x + slide, y + 50, x + boxW, y + 84), opacity * 0.75f);
    }
    if (showStats) DrawStats(ctx, stats, audio);
    if (showHelp) DrawHelp(ctx, w, kDesignHeight);

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Overlay::DrawStats(ID2D1DeviceContext* ctx, const OverlayStats& s, const AudioFeatures& a) {
    wchar_t tempo[64];
    if (a.bpm > 0.0f) swprintf_s(tempo, L"%.1f bpm (%.0f%%)", a.bpm, a.tempoConfidence * 100.0f);
    else wcscpy_s(tempo, L"--");
    wchar_t buf[1024];
    swprintf_s(buf,
        L"%ls%ls%ls\n"
        L"%d fps (cap %.1f%ls)\n"
        L"window  %d x %d\n"
        L"render  %d x %d  (%.0f%%)\n"
        L"gpu     %.2f ms\n"
        L"audio   %ls\n"
        L"        %u Hz%ls   beats %u\n"
        L"tempo   %ls   build %.2f   drops %u",
        s.visualizer.c_str(), s.visualizerStatus.empty() ? L"" : L"  ·  ", s.visualizerStatus.c_str(),
        s.fps, s.targetFps, s.lowPower ? L", idle" : L"",
        s.width, s.height,
        s.renderWidth, s.renderHeight, s.width ? 100.0 * s.renderWidth / s.width : 0.0,
        s.gpuMs,
        s.audioDevice.c_str(),
        s.sampleRate, a.silent ? L"  (silent)" : L"", a.beatCount,
        tempo, a.buildUp, a.dropCount);

    const D2D1_RECT_F panel = D2D1::RectF(24, 24, 24 + 560, 24 + 355);
    m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(panel, 8, 8), m_brush.Get());
    DrawText(ctx, buf, m_monoFormat.Get(), D2D1::RectF(panel.left + 16, panel.top + 12, panel.right - 16, panel.bottom), 1.0f);

    // Level meters.
    struct Meter { const wchar_t* label; float value; D2D1_COLOR_F colour; };
    const Meter meters[] = {
        { L"bass", a.bass, D2D1::ColorF(1.0f, 0.35f, 0.25f) },
        { L"low", a.lowMid, D2D1::ColorF(1.0f, 0.75f, 0.25f) },
        { L"high", a.highMid, D2D1::ColorF(0.45f, 1.0f, 0.45f) },
        { L"treb", a.treble, D2D1::ColorF(0.35f, 0.75f, 1.0f) },
        { L"level", a.level, D2D1::ColorF(0.9f, 0.9f, 0.9f) },
        { L"inten", a.intensity, D2D1::ColorF(0.65f, 0.5f, 1.0f) },
    };
    float y = panel.top + 215;
    for (const Meter& m : meters) {
        DrawText(ctx, m.label, m_monoFormat.Get(), D2D1::RectF(panel.left + 16, y - 3, panel.left + 90, y + 20), 0.8f);
        const D2D1_RECT_F bar = D2D1::RectF(panel.left + 90, y, panel.right - 70, y + 14);
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
        ctx->FillRectangle(bar, m_brush.Get());
        m_brush->SetColor(m.colour);
        ctx->FillRectangle(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * std::clamp(m.value, 0.0f, 1.0f), bar.bottom), m_brush.Get());
        y += 22;
    }
    // Beat lamp.
    // Beat lamps: heard onsets (top) and the predicted grid (bottom, flashes exactly on the beat).
    const D2D1_RECT_F lamp = D2D1::RectF(panel.right - 54, panel.top + 215, panel.right - 18, panel.top + 215 + 36);
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.1f + 0.9f * a.beat));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(lamp, 4, 4), m_brush.Get());
    const float grid = a.tempoConfidence > 0.4f ? std::pow(1.0f - a.beatPhase, 6.0f) : 0.0f;
    const D2D1_RECT_F gridLamp = D2D1::RectF(lamp.left, lamp.bottom + 10, lamp.right, lamp.bottom + 46);
    m_brush->SetColor(D2D1::ColorF(0.4f, 0.8f, 1.0f, 0.1f + 0.9f * grid));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(gridLamp, 4, 4), m_brush.Get());
}

void Overlay::DrawHelp(ID2D1DeviceContext* ctx, float width, float height) {
    const float rowH = 40.0f;
    const float panelW = 720.0f;
    const float panelH = 110.0f + rowH * ARRAYSIZE(kHelpLines);
    const D2D1_RECT_F panel = D2D1::RectF((width - panelW) / 2, (height - panelH) / 2,
                                          (width + panelW) / 2, (height + panelH) / 2);
    m_brush->SetColor(D2D1::ColorF(0.02f, 0.02f, 0.05f, 0.8f));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(panel, 12, 12), m_brush.Get());
    DrawText(ctx, L"Visualizer", m_titleFormat.Get(), D2D1::RectF(panel.left, panel.top + 18, panel.right, panel.top + 80), 1.0f);
    float y = panel.top + 95;
    for (const auto& line : kHelpLines) {
        DrawText(ctx, line[0], m_bodyFormat.Get(), D2D1::RectF(panel.left + 50, y, panel.left + 280, y + rowH), 0.75f);
        DrawText(ctx, line[1], m_bodyFormat.Get(), D2D1::RectF(panel.left + 280, y, panel.right - 30, y + rowH), 1.0f);
        y += rowH;
    }
}
