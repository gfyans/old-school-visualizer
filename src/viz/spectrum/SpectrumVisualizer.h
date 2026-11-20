#pragma once
#include <wrl/client.h>
#include "viz/Visualizer.h"

// Classic bar spectrum with a mirrored floor reflection. Simple, and handy for
// checking the audio analysis by eye.
class SpectrumVisualizer : public IVisualizer {
public:
    const wchar_t* Name() const override { return L"Spectrum"; }
    bool CreateResources(ID3D11Device* device) override;
    void ReleaseResources() override { m_ps.Reset(); }
    void Update(const AudioFeatures&, float) override {}
    void Render(Renderer& renderer, float time, float dt) override;

private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
};
