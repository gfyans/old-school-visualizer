#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "audio/AudioFeatures.h"

class Renderer;

// The plug-in contract for filters: a look applied over whichever visualiser is showing (X cycles
// them). To add one: implement this (or use ShaderFilter for a single pixel shader), then register it
// in main.cpp (RegisterFilters). See docs/adding-a-filter.md.
//
// The FilterHost draws the finished scene (visualiser, transition) into a texture, then calls Apply()
// with the screen as the target, so the filter decides every pixel the viewer sees.
class IFilter {
public:
    virtual ~IFilter() = default;

    virtual const wchar_t* Name() const = 0;   // shown in the menu and on screen

    // Create/release GPU resources. Called at startup and again after a device loss.
    virtual bool CreateResources(ID3D11Device* device) = 0;
    virtual void ReleaseResources() = 0;

    // Called every frame while this filter is selected.
    virtual void Update(const AudioFeatures& /*audio*/, float /*dt*/) {}

    // The target is bound, with FrameConstants (b0), AudioConstants (b1) and FilterConstants (b2,
    // see shaders/filter_common.hlsli) set, the scene at t0 and a linear clamp sampler at s0.
    // Slots b3+, t1+ and s1+ are free. Typically: bind own resources, then renderer.DrawFullscreen(ps).
    virtual void Apply(Renderer& renderer) = 0;

    // Whether Random mode may switch to this filter.
    virtual bool InRandom() const { return true; }
};

// A filter that is just one pixel shader (most of them):
//   filters.Add(std::make_unique<ShaderFilter>(L"VHS", g_filter_vhs_ps, sizeof(g_filter_vhs_ps)));
class ShaderFilter : public IFilter {
public:
    ShaderFilter(const wchar_t* name, const BYTE* bytecode, size_t size)
        : m_name(name), m_bytecode(bytecode), m_size(size) {}

    const wchar_t* Name() const override { return m_name; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override { m_ps.Reset(); }
    void Apply(Renderer& renderer) override;

protected:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;

private:
    const wchar_t* m_name;
    const BYTE* m_bytecode;
    size_t m_size;
};
