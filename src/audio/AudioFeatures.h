#pragma once
#include <cstdint>

// What every visualiser gets each frame. All levels are 0..1, auto-gained so
// quiet and loud tracks (and any system volume) fill the same range.
struct AudioFeatures {
    static constexpr int kSpectrumBands = 64;

    float bass = 0;        //   20 –  250 Hz
    float lowMid = 0;      //  250 – 1000 Hz
    float highMid = 0;     // 1000 – 4000 Hz
    float treble = 0;      // 4000 – 16000 Hz
    float level = 0;       // overall loudness right now
    float intensity = 0;   // slow (~5 s) average of level: how "big" this section of the song is
    float beat = 0;        // 1 on a detected beat, decays to 0 in ~0.15 s
    uint32_t beatCount = 0;
    bool silent = true;    // digital silence (nothing playing)

    float spectrum[kSpectrumBands] = {};   // log-spaced 30 Hz – 16 kHz
};
