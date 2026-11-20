#include "media/TrackPalette.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kHueBins = 18;   // 20 degrees each

void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
    const float d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 1e-6f) { h = 0.0f; return; }
    if (mx == r) h = std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
}

void HsvToRgb(float h, float s, float v, float out[3]) {
    h -= std::floor(h);
    for (int c = 0; c < 3; ++c) {
        const float k = std::fmod(5.0f - 2.0f * c + h * 6.0f, 6.0f);
        out[c] = v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    }
}

struct Bin {
    float weight = 0;
    float r = 0, g = 0, b = 0;
};

} // namespace

bool ExtractPalette(const uint8_t* bgra, int width, int height, TrackPalette& out) {
    if (!bgra || width <= 0 || height <= 0) return false;

    Bin bins[kHueBins];
    float total = 0.0f;
    const int count = width * height;
    for (int i = 0; i < count; ++i) {
        const float b = bgra[i * 4 + 0] / 255.0f, g = bgra[i * 4 + 1] / 255.0f, r = bgra[i * 4 + 2] / 255.0f;
        float h, s, v;
        RgbToHsv(r, g, b, h, s, v);
        // Vividness: saturated and reasonably bright; greys, black and white count for ~nothing.
        const float w = s * s * std::clamp((v - 0.12f) / 0.5f, 0.0f, 1.0f);
        if (w < 0.02f) continue;
        Bin& bin = bins[std::min(static_cast<int>(h * kHueBins), kHueBins - 1)];
        bin.weight += w;
        bin.r += r * w; bin.g += g * w; bin.b += b * w;
        total += w;
    }
    if (total < 0.04f * count) return false;   // essentially monochrome art

    // Smooth bins circularly so a hue straddling two bins still wins.
    float smoothed[kHueBins];
    for (int i = 0; i < kHueBins; ++i)
        smoothed[i] = bins[i].weight + 0.5f * (bins[(i + 1) % kHueBins].weight + bins[(i + kHueBins - 1) % kHueBins].weight);

    auto colourOf = [&](int centre, float rgb[3]) {   // weighted average of a bin and its neighbours
        float w = 0, r = 0, g = 0, b = 0;
        for (int d = -1; d <= 1; ++d) {
            const Bin& bin = bins[(centre + d + kHueBins) % kHueBins];
            w += bin.weight; r += bin.r; g += bin.g; b += bin.b;
        }
        rgb[0] = r / w; rgb[1] = g / w; rgb[2] = b / w;
    };

    const int top = static_cast<int>(std::max_element(smoothed, smoothed + kHueBins) - smoothed);
    // Accent: strongest hue at least 60 degrees away that is a meaningful part of the image.
    int accent = -1;
    for (int i = 0; i < kHueBins; ++i) {
        const int dist = std::min(std::abs(i - top), kHueBins - std::abs(i - top));
        if (dist >= 3 && smoothed[i] > 0.12f * smoothed[top] && (accent < 0 || smoothed[i] > smoothed[accent])) accent = i;
    }

    float rgb[3], h, s, v;
    colourOf(top, rgb);
    RgbToHsv(rgb[0], rgb[1], rgb[2], h, s, v);
    const float baseHue = h;
    HsvToRgb(h, std::max(s, 0.65f), std::max(v, 0.85f), out.mid);   // vivid version of the dominant colour
    HsvToRgb(h, 0.75f, 0.14f, out.deep);                            // its dark shade

    if (accent >= 0) {
        colourOf(accent, rgb);
        RgbToHsv(rgb[0], rgb[1], rgb[2], h, s, v);
        HsvToRgb(h, std::clamp(s, 0.45f, 0.8f), 1.0f, out.hot);
    } else {
        HsvToRgb(baseHue + 0.08f, 0.4f, 1.0f, out.hot);   // single-hue art: a light, shifted highlight
    }
    return true;
}
