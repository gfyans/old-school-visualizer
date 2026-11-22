#pragma once
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <cstring>
#include <wrl/client.h>
#include "audio/AudioFeatures.h"
#include "render/RenderTarget.h"

using Microsoft::WRL::ComPtr;

// Per-pass constants, bound at b0 for every shader (matches shaders/common.hlsli).
struct FrameConstants {
    float time;
    float deltaTime;
    float width;    // size of the current render target viewport
    float height;
};

// Audio features, bound at b1 for every shader (matches shaders/common.hlsli).
struct AudioConstants {
    float bands[4];   // bass, low-mid, high-mid, treble
    float level;
    float intensity;
    float beat;
    float beatCount;
    float spectrum[AudioFeatures::kSpectrumBands];
    float tempo[4];       // bpm, beat phase, bar phase, confidence
    float structure[4];   // build-up, drop pulse, grid beat count, drop count
    float instruments[4]; // snare, hi-hat, vocal, pad
    float waveform[AudioFeatures::kWaveformPoints];
};
static_assert(sizeof(AudioConstants) % 16 == 0);

// One input to the final composite: a visualiser's offscreen image.
struct CompositeLayer {
    ID3D11ShaderResourceView* srv = nullptr;
    float uvScale[2] = { 1, 1 };   // used region / allocated texture size (dynamic resolution)
    float uvMax[2] = { 1, 1 };     // clamp so bilinear filtering never reads outside the used region
};

enum class PresentResult { Ok, Occluded, DeviceLost };

// Owns the D3D11 device, swap chain and the Direct2D context used for text overlays.
// Flip-model swap chain with a frame-latency waitable object. Transparent: a DirectComposition
// swap chain with premultiplied alpha, for a window created with WS_EX_NOREDIRECTIONBITMAP, so
// what's black in the frame shows the desktop (or the game) behind.
class Renderer {
public:
    ~Renderer() { Shutdown(); }

    bool Init(HWND hwnd, int width, int height, bool transparent = false);
    bool Reinit();          // after device loss
    void Shutdown();
    void Resize(int width, int height);
    // A new swap chain for another window (the device and everything made on it stay). For a
    // transparent window, call FinishTransparentFrame() just before each Present().
    bool SetWindow(HWND hwnd, int width, int height, bool transparent);
    bool Transparent() const { return m_transparent; }
    // Transparent only: turns the finished frame's brightness into its alpha (shaders/alpha_ps.hlsl),
    // so black is see-through. Draw everything first, recording and screenshots included (they stay opaque).
    void FinishTransparentFrame();

    void WaitForNextFrame();
    void UploadAudio(const AudioFeatures& audio);   // once per frame; stays bound at b1
    void BindTarget(ID3D11RenderTargetView* rtv, int width, int height, float time, float deltaTime);
    // The target most recently bound with BindTarget (lets multi-pass visualisers restore it).
    ID3D11RenderTargetView* BoundTarget() const { return m_boundRTV; }
    int BoundWidth() const { return m_boundW; }
    int BoundHeight() const { return m_boundH; }
    void DrawFullscreen(ID3D11PixelShader* ps);
    // Draws layer a (or the a->b transition at `mix`, in `style`) scaled to the back buffer, or
    // to `region` of it (multi-monitor: one region per monitor).
    // Styles: 0 crossfade, 1 glitch cut, 2 zoom-through, 3 iris, 4 pixel dissolve.
    // into: draw the region's picture into the top-left of this texture instead, for a filter to
    // restyle (no mood grade: the filter sets the colours).
    void Composite(const CompositeLayer& a, const CompositeLayer* b, float mix, int style = 0, float seed = 0.0f,
                   const RECT* region = nullptr, const RenderTarget* into = nullptr);
    // Binds the back buffer (or a region of it: the viewport and iResolution match the region).
    void BindBackBuffer(const RECT* region = nullptr);
    // Mood colour grade applied by Composite: warmth -1 (cool) .. +1 (warm), hue in radians,
    // amount 0 (off) .. 1.
    void SetGrade(float warmth, float hue, float amount) { m_gradeWarmth = warmth; m_gradeHue = hue; m_gradeAmount = amount; }
    void ClearBackBuffer(float r = 0.0f, float g = 0.0f, float b = 0.0f);

    ID2D1DeviceContext* BeginOverlay();   // draws onto the back buffer; nullptr if unavailable
    void EndOverlay();
    PresentResult Present();

    ID3D11Device* Device() const { return m_device.Get(); }
    // The back buffer being drawn this frame (don't hold on to it across a Resize).
    ComPtr<ID3D11Texture2D> BackBuffer() const {
        ComPtr<ID3D11Texture2D> t;
        if (m_swapChain) m_swapChain->GetBuffer(0, IID_PPV_ARGS(&t));
        return t;
    }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    ID3D11SamplerState* LinearClamp() const { return m_linearClamp.Get(); }
    ID2D1DeviceContext* D2DContext() const { return m_d2dContext.Get(); }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

private:
    bool CreateDevice();
    bool CreateSwapChain();
    void ReleaseSwapChain();
    bool CreateTargets();
    bool CreatePipeline();
    bool CreateD2D();

    HWND m_hwnd = nullptr;
    bool m_transparent = false;
    int m_width = 0, m_height = 0;
    ID3D11RenderTargetView* m_boundRTV = nullptr;   // not owned
    int m_boundW = 0, m_boundH = 0;
    float m_gradeWarmth = 0.0f, m_gradeHue = 0.0f, m_gradeAmount = 0.0f;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain2> m_swapChain;
    HANDLE m_frameWait = nullptr;
    ComPtr<ID3D11RenderTargetView> m_backBufferRTV;

    // Transparent windows: the composition tree that shows the swap chain, and the copy of the
    // frame the alpha pass reads (it can't read the back buffer it's writing).
    ComPtr<IDCompositionDevice> m_dcompDevice;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;
    ComPtr<ID3D11PixelShader> m_alphaPS;
    ComPtr<ID3D11Texture2D> m_alphaCopy;
    ComPtr<ID3D11ShaderResourceView> m_alphaCopySRV;

    ComPtr<ID3D11VertexShader> m_fullscreenVS;
    ComPtr<ID3D11PixelShader> m_compositePS;
    ComPtr<ID3D11SamplerState> m_linearClamp;
    ComPtr<ID3D11Buffer> m_frameCB;
    ComPtr<ID3D11Buffer> m_audioCB;
    ComPtr<ID3D11Buffer> m_compositeCB;

    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<ID2D1Bitmap1> m_d2dTarget;
};
