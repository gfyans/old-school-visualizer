#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "filter/Filter.h"
#include "render/RenderTarget.h"

class Renderer;

// Owns the registered filters and the one selected (or none), and draws it: the scene is composited
// into an offscreen texture, then the filter draws it to the screen. Also keeps what every filter
// shares: how hard the music is hitting (glitch), a clock, and a four-colour palette that changes
// with each song and drop.
class FilterHost {
public:
    void Add(std::unique_ptr<IFilter> filter);

    bool CreateResources(ID3D11Device* device);
    void ReleaseResources();

    int Count() const { return static_cast<int>(m_filters.size()); }
    const IFilter& At(int index) const { return *m_filters[index]; }
    int Current() const { return m_current; }   // -1 = off
    bool Active() const { return m_current >= 0; }
    const wchar_t* CurrentName() const { return Active() ? m_filters[m_current]->Name() : L""; }
    int FindByName(const std::wstring& name) const;   // -1 if none

    void Select(int index);   // -1 = off
    void Next();              // off -> first -> ... -> last -> off
    void SelectRandom(uint32_t seed);   // another filter Random mode may use (stays off if off)

    void NewTrack();   // a new palette with each song
    void Update(const AudioFeatures& audio, float dt);

    // Drawing, for each area of the screen (region = nullptr: the whole window). The host composites
    // the scene into SceneTarget(), then Apply() draws the filter over that area of the back buffer.
    RenderTarget* SceneTarget(Renderer& renderer);
    void Apply(Renderer& renderer, const RECT* region);

private:
    std::vector<std::unique_ptr<IFilter>> m_filters;
    int m_current = -1;
    ID3D11Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    RenderTarget m_scene;   // window-sized; each area uses its top-left corner
    float m_glitch = 0.0f, m_time = 0.0f;
    int m_palette = 0;
    uint32_t m_seenDrop = 0;
};
