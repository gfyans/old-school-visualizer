#pragma once
#include <cstdint>

// Colours taken from the playing track's album art (display RGB 0..1).
struct TrackPalette {
    float deep[3];   // dark, for backgrounds and thin areas
    float mid[3];    // the dominant colour
    float hot[3];    // a bright accent
};

// Picks a palette from album art pixels (BGRA8, top-down). Looks for the most
// vivid dominant hue and a contrasting accent, ignoring greys, near-black and
// near-white. Returns false for essentially monochrome art (keep default looks).
bool ExtractPalette(const uint8_t* bgra, int width, int height, TrackPalette& out);
