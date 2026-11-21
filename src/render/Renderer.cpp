#include "render/Renderer.h"
#include "g_composite_ps.h"
#include "g_fullscreen_vs.h"

#include <cstring>

namespace {
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

struct CompositeConstants {   // matches shaders/composite_ps.hlsl
    float uvScaleA[2], uvMaxA[2];
    float uvScaleB[2], uvMaxB[2];
    float mix, hasB, style, seed;
    float warmth, hueShift, gradeAmount, gradePad;
};

template <typename T>
void UploadConstants(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const T& data) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        ctx->Unmap(buffer, 0);
    }
}

bool CreateDynamicCB(ID3D11Device* device, UINT size, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = size;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb, nullptr, out));
}
} // namespace

bool Renderer::Init(HWND hwnd, int width, int height) {
    m_hwnd = hwnd;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    if (!CreateDevice() || !CreateSwapChain() || !CreateTargets() || !CreatePipeline()) return false;
    CreateD2D();   // optional: overlays are skipped if this fails
    return true;
}

bool Renderer::Reinit() {
    Shutdown();
    return Init(m_hwnd, m_width, m_height);
}

void Renderer::Shutdown() {
    if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
    m_d2dTarget.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    if (m_context) m_context->ClearState();
    m_compositeCB.Reset();
    m_audioCB.Reset();
    m_frameCB.Reset();
    m_linearClamp.Reset();
    m_compositePS.Reset();
    m_fullscreenVS.Reset();
    m_backBufferRTV.Reset();
    if (m_frameWait) { CloseHandle(m_frameWait); m_frameWait = nullptr; }
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
}

bool Renderer::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // required for Direct2D interop
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
#ifndef NDEBUG
    if (FAILED(hr)) {   // debug layer not installed: retry without it
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
    }
#endif
    return SUCCEEDED(hr);
}

bool Renderer::CreateSwapChain() {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = kBackBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kSwapChainFlags;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr, &sc1)) ||
        FAILED(sc1.As(&m_swapChain)))
        return false;

    // We handle Alt+Enter ourselves (borderless fullscreen), not DXGI's exclusive mode.
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    m_swapChain->SetMaximumFrameLatency(1);
    m_frameWait = m_swapChain->GetFrameLatencyWaitableObject();
    return m_frameWait != nullptr;
}

bool Renderer::CreateTargets() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    return SUCCEEDED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_backBufferRTV));
}

bool Renderer::CreatePipeline() {
    if (FAILED(m_device->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &m_fullscreenVS)) ||
        FAILED(m_device->CreatePixelShader(g_composite_ps, sizeof(g_composite_ps), nullptr, &m_compositePS)))
        return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&s, &m_linearClamp))) return false;

    return CreateDynamicCB(m_device.Get(), sizeof(FrameConstants), &m_frameCB) &&
           CreateDynamicCB(m_device.Get(), sizeof(AudioConstants), &m_audioCB) &&
           CreateDynamicCB(m_device.Get(), sizeof(CompositeConstants), &m_compositeCB);
}

bool Renderer::CreateD2D() {
    ComPtr<IDXGIDevice> dxgiDevice;
    D2D1_FACTORY_OPTIONS opts{};
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, m_d2dFactory.GetAddressOf())) &&
           SUCCEEDED(m_device.As(&dxgiDevice)) &&
           SUCCEEDED(m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice)) &&
           SUCCEEDED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext));
}

void Renderer::Resize(int width, int height) {
    if (!m_swapChain || width <= 0 || height <= 0) return;
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
    m_d2dTarget.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_backBufferRTV.Reset();
    m_context->Flush();
    m_swapChain->ResizeBuffers(0, m_width, m_height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    CreateTargets();
}

void Renderer::WaitForNextFrame() {
    if (m_frameWait) WaitForSingleObjectEx(m_frameWait, 1000, TRUE);
}

void Renderer::UploadAudio(const AudioFeatures& audio) {
    AudioConstants ac{};
    ac.bands[0] = audio.bass;
    ac.bands[1] = audio.lowMid;
    ac.bands[2] = audio.highMid;
    ac.bands[3] = audio.treble;
    ac.level = audio.level;
    ac.intensity = audio.intensity;
    ac.beat = audio.beat;
    ac.beatCount = static_cast<float>(audio.beatCount);
    memcpy(ac.spectrum, audio.spectrum, sizeof(ac.spectrum));
    ac.tempo[0] = audio.bpm;
    ac.tempo[1] = audio.beatPhase;
    ac.tempo[2] = audio.barPhase;
    ac.tempo[3] = audio.tempoConfidence;
    ac.structure[0] = audio.buildUp;
    ac.structure[1] = audio.drop;
    ac.structure[2] = static_cast<float>(audio.gridBeat);
    ac.structure[3] = static_cast<float>(audio.dropCount);
    memcpy(ac.waveform, audio.waveform, sizeof(ac.waveform));
    UploadConstants(m_context.Get(), m_audioCB.Get(), ac);
}

void Renderer::BindTarget(ID3D11RenderTargetView* rtv, int width, int height, float time, float deltaTime) {
    UploadConstants(m_context.Get(), m_frameCB.Get(),
        FrameConstants{ time, deltaTime, static_cast<float>(width), static_cast<float>(height) });

    m_boundRTV = rtv;
    m_boundW = width;
    m_boundH = height;
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    m_context->RSSetViewports(1, &vp);

    ID3D11Buffer* cbs[2] = { m_frameCB.Get(), m_audioCB.Get() };
    m_context->VSSetConstantBuffers(0, 2, cbs);
    m_context->PSSetConstantBuffers(0, 2, cbs);
}

void Renderer::DrawFullscreen(ID3D11PixelShader* ps) {
    // Single oversized triangle generated from SV_VertexID: no vertex buffer needed.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
    m_context->PSSetShader(ps, nullptr, 0);
    m_context->Draw(3, 0);
}

void Renderer::BindBackBuffer(const RECT* region) {
    if (!region) {
        BindTarget(m_backBufferRTV.Get(), m_width, m_height, 0.0f, 0.0f);
        return;
    }
    const int w = region->right - region->left, h = region->bottom - region->top;
    BindTarget(m_backBufferRTV.Get(), w, h, 0.0f, 0.0f);
    D3D11_VIEWPORT vp{ static_cast<float>(region->left), static_cast<float>(region->top), static_cast<float>(w), static_cast<float>(h), 0, 1 };
    m_context->RSSetViewports(1, &vp);
}

void Renderer::ClearBackBuffer(float r, float g, float b) {
    const float colour[4] = { r, g, b, 1 };
    if (m_backBufferRTV) m_context->ClearRenderTargetView(m_backBufferRTV.Get(), colour);
}

void Renderer::Composite(const CompositeLayer& a, const CompositeLayer* b, float mix, int style, float seed, const RECT* region) {
    BindBackBuffer(region);

    CompositeConstants cc{};
    memcpy(cc.uvScaleA, a.uvScale, sizeof(cc.uvScaleA));
    memcpy(cc.uvMaxA, a.uvMax, sizeof(cc.uvMaxA));
    if (b) {
        memcpy(cc.uvScaleB, b->uvScale, sizeof(cc.uvScaleB));
        memcpy(cc.uvMaxB, b->uvMax, sizeof(cc.uvMaxB));
    }
    cc.mix = mix;
    cc.hasB = b ? 1.0f : 0.0f;
    cc.style = static_cast<float>(style);
    cc.seed = seed;
    cc.warmth = m_gradeWarmth;
    cc.hueShift = m_gradeHue;
    cc.gradeAmount = m_gradeAmount;
    UploadConstants(m_context.Get(), m_compositeCB.Get(), cc);

    ID3D11ShaderResourceView* srvs[2] = { a.srv, b ? b->srv : nullptr };
    ID3D11Buffer* cb = m_compositeCB.Get();
    ID3D11SamplerState* sampler = m_linearClamp.Get();
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetConstantBuffers(2, 1, &cb);
    m_context->PSSetSamplers(0, 1, &sampler);
    DrawFullscreen(m_compositePS.Get());

    ID3D11ShaderResourceView* none[2] = {};
    m_context->PSSetShaderResources(0, 2, none);   // so the layers can be render targets next frame
}

ID2D1DeviceContext* Renderer::BeginOverlay() {
    if (!m_d2dContext) return nullptr;
    if (!m_d2dTarget) {
        ComPtr<IDXGISurface> surface;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(kBackBufferFormat, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&surface))) ||
            FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, &m_d2dTarget)))
            return nullptr;
        m_d2dContext->SetTarget(m_d2dTarget.Get());
    }
    m_d2dContext->BeginDraw();
    return m_d2dContext.Get();
}

void Renderer::EndOverlay() {
    if (m_d2dContext && m_d2dContext->EndDraw() == D2DERR_RECREATE_TARGET) {
        m_d2dContext->SetTarget(nullptr);
        m_d2dTarget.Reset();
    }
}

PresentResult Renderer::Present() {
    const HRESULT hr = m_swapChain->Present(1, 0);   // vsync (if the driver honours it)
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return PresentResult::DeviceLost;
    if (hr == DXGI_STATUS_OCCLUDED) return PresentResult::Occluded;
    return PresentResult::Ok;
}
