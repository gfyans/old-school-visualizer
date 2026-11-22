#pragma once
#include <windows.h>
#include <string>

class Renderer;

// Screenshots (F12): the picture as drawn this frame, read back from the back buffer (so it works even
// when the window is covered), saved as a PNG.
namespace Screenshot {
// Pictures\Visualizer (VISUALIZER_SCREENSHOT_DIR overrides it, for testing); created if needed.
std::wstring Folder();
// Saves `region` of this frame's back buffer to Folder(), named by the time. Returns the path, or
// empty on failure. keepAlpha: the back buffer holds premultiplied alpha (a transparent window,
// after Renderer::FinishTransparentFrame) and the PNG keeps it; otherwise the PNG is opaque.
std::wstring Save(Renderer& renderer, const RECT& region, bool keepAlpha = false);
}
