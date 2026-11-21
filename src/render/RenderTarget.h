#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// An offscreen colour texture usable both as a render target and a shader input.
struct RenderTarget {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0, height = 0;

    // mips: allocate a full mip chain that GenerateMips can fill (e.g. for bloom).
    bool Create(ID3D11Device* device, int w, int h, DXGI_FORMAT format, bool mips = false) {
        Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = mips ? 0 : 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &texture)) ||
            FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
            Reset();
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    void Reset() {
        srv.Reset();
        rtv.Reset();
        texture.Reset();
        width = height = 0;
    }
};
