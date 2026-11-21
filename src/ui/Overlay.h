#pragma once
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

#include "audio/AudioFeatures.h"

struct OverlayStats {
    int fps = 0;
    double targetFps = 0;
    bool lowPower = false;
    int width = 0, height = 0;
    int renderWidth = 0, renderHeight = 0;
    float gpuMs = 0;
    std::wstring audioDevice;
    uint32_t sampleRate = 0;
    std::wstring visualizer;
    std::wstring visualizerStatus;
};

// Text drawn over the visualisation with Direct2D/DirectWrite: transient messages
// (visualiser name, device changes), the F1 stats panel and the H help panel.
// Draws nothing (and costs nothing) when there is nothing to show.
class Overlay {
public:
    bool Init();                                          // device-independent (DirectWrite)
    bool CreateDeviceResources(ID2D1DeviceContext* ctx);  // brushes
    void ReleaseDeviceResources();

    void ShowMessage(const std::wstring& text, float seconds = 2.5f);
    // Now-playing caption, shown for a few seconds when a song starts.
    void ShowTrack(const std::wstring& title, const std::wstring& artist, float seconds = 7.0f);
    // A persistent status in the top-right corner (e.g. "REC 0:42"); empty hides it.
    void SetIndicator(const std::wstring& text, bool recording) { m_indicator = text; m_indicatorRec = recording; }
    void ClearTransient() { m_message.clear(); m_trackTitle.clear(); }   // drop messages and captions now
    void Update(float dt);
    bool HasContent(bool stats, bool help) const;
    // bottomInset: design-space pixels (1080p) kept clear at the bottom, e.g. for the EQ bars.
    // originX/Y: top-left of the area to draw in (multi-monitor: the primary monitor's region).
    void Draw(ID2D1DeviceContext* ctx, int width, int height, const OverlayStats& stats,
              const AudioFeatures& audio, bool showStats, bool showHelp, float bottomInset = 0.0f,
              float originX = 0.0f, float originY = 0.0f);

private:
    void DrawText(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* format,
                  const D2D1_RECT_F& rect, float opacity);
    void DrawStats(ID2D1DeviceContext* ctx, const OverlayStats& stats, const AudioFeatures& audio);
    void DrawHelp(ID2D1DeviceContext* ctx, float width, float height);

    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat, m_bodyFormat, m_monoFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_trackFormat, m_artistFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;

    std::wstring m_indicator;
    bool m_indicatorRec = false;
    std::wstring m_message;
    float m_messageTime = 0, m_messageDuration = 0;
    std::wstring m_trackTitle, m_trackArtist;
    float m_trackTime = 0, m_trackDuration = 0;
};
