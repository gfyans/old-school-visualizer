#include "viz/spectrum/SpectrumVisualizer.h"
#include "render/Renderer.h"
#include "g_spectrum_ps.h"

bool SpectrumVisualizer::CreateResources(ID3D11Device* device) {
    return SUCCEEDED(device->CreatePixelShader(g_spectrum_ps, sizeof(g_spectrum_ps), nullptr, &m_ps));
}

void SpectrumVisualizer::Render(Renderer& renderer, float, float) {
    renderer.DrawFullscreen(m_ps.Get());
}
