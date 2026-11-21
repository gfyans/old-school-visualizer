#include "capture/Screenshot.h"
#include "render/Renderer.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Screenshot {

std::wstring Folder() {
    wchar_t env[MAX_PATH];
    std::wstring folder;
    if (GetEnvironmentVariableW(L"VISUALIZER_SCREENSHOT_DIR", env, MAX_PATH) > 0) {
        folder = env;
    } else {
        PWSTR pictures = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &pictures))) folder = std::wstring(pictures) + L"\\Visualizer";
        CoTaskMemFree(pictures);
    }
    if (!folder.empty()) SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return folder;
}

std::wstring Save(Renderer& renderer, const RECT& region) {
    ComPtr<ID3D11Texture2D> back = renderer.BackBuffer();
    if (!back) return {};
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    const UINT left = static_cast<UINT>(std::max<LONG>(region.left, 0)), top = static_cast<UINT>(std::max<LONG>(region.top, 0));
    const UINT right = std::min<UINT>(static_cast<UINT>(region.right), bd.Width), bottom = std::min<UINT>(static_cast<UINT>(region.bottom), bd.Height);
    if (right <= left || bottom <= top) return {};
    const UINT w = right - left, h = bottom - top;

    // Copy the region to a texture the CPU can read.
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = bd.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (bd.Format != DXGI_FORMAT_B8G8R8A8_UNORM || FAILED(renderer.Device()->CreateTexture2D(&sd, nullptr, &staging))) return {};
    ID3D11DeviceContext* ctx = renderer.Context();
    const D3D11_BOX box{ left, top, 0, right, bottom, 1 };
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, back.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        uint8_t* out = pixels.data() + size_t(y) * w * 4;
        memcpy(out, row, size_t(w) * 4);
        for (UINT x = 0; x < w; ++x) out[x * 4 + 3] = 255;   // opaque
    }
    ctx->Unmap(staging.Get(), 0);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\Visualizer %04d-%02d-%02d %02d.%02d.%02d.%03d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const std::wstring folder = Folder();
    if (folder.empty()) return {};
    const std::wstring path = folder + name;

    // Encode as PNG with WIC.
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    return path;
}

} // namespace Screenshot
