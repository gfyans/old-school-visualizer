#pragma once
#include <windows.h>
#include <d3d11.h>
#include <string>
#include "audio/AudioFeatures.h"

class Renderer;

// The plug-in contract. To add a visualiser: implement this, then register it in
// main.cpp (RegisterVisualizers). The host owns an offscreen target per visualiser,
// binds it before Render(), and handles scaling, crossfades and device loss.
class IVisualizer {
public:
    virtual ~IVisualizer() = default;

    virtual const wchar_t* Name() const = 0;

    // Create/release GPU resources. Called at startup and again after a device loss.
    virtual bool CreateResources(ID3D11Device* device) = 0;
    virtual void ReleaseResources() = 0;

    // Preferred internal resolution as a fraction of the window (soft, blurry looks can
    // use 0.5 for ~4x less GPU work). The host may go lower on slow GPUs.
    virtual float RenderScale() const { return 1.0f; }

    // Called when it becomes the visible visualiser.
    virtual void Activate() {}

    // Only called while visible (including during a crossfade).
    virtual void Update(const AudioFeatures& audio, float dt) = 0;

    // Target is bound and FrameConstants (b0) / AudioConstants (b1) are set; slots b2+, t0+, s0+ are free.
    // Typically: bind own resources, then renderer.DrawFullscreen(ps).
    virtual void Render(Renderer& renderer, float time, float dt) = 0;

    // Optional per-visualiser keys (e.g. N = next look, Space = lock). Return true if handled.
    virtual bool OnKey(UINT /*vk*/) { return false; }

    // Optional one-off notification to show on screen (e.g. "Look locked"); cleared when read.
    virtual std::wstring TakeMessage() { return {}; }

    // Optional status line for the F1 stats overlay.
    virtual std::wstring Status() const { return {}; }
};
