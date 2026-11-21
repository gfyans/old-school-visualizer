#pragma once
#include <cstdint>

// What every visualiser gets each frame. Levels are 0..1, auto-gained so
// quiet and loud tracks (and any system volume) fill the same range.
struct AudioFeatures {
    static constexpr int kSpectrumBands = 64;
    static constexpr int kWaveformPoints = 128;
    static constexpr int kScopePoints = 1024;

    // --- Levels ---
    float bass = 0;        //   20 –  250 Hz
    float lowMid = 0;      //  250 – 1000 Hz
    float highMid = 0;     // 1000 – 4000 Hz
    float treble = 0;      // 4000 – 16000 Hz
    float level = 0;       // overall loudness right now
    float intensity = 0;   // slow (~5 s) average of level: how "big" this section of the song is
    bool silent = true;    // digital silence (nothing playing)

    // --- Onset beats (reactive: fire when a hit is heard) ---
    float beat = 0;        // 1 on a detected beat, decays to 0 in ~0.15 s
    uint32_t beatCount = 0;

    // --- Beat grid (predictive: locked to the song's tempo) ---
    float bpm = 0;              // 0 until a tempo is found
    float beatPhase = 0;        // 0..1 through the current beat; wraps to 0 exactly on the beat
    float barPhase = 0;         // 0..1 through the current 4-beat bar
    uint32_t gridBeat = 0;      // increments on every predicted beat
    float tempoConfidence = 0;  // 0..1: use the grid when this is high (> ~0.4)
    float percussive = 0;       // 0..1: drums present above ~0.36; solo piano / pads below ~0.3
    // How fast scenes should animate for this song's tempo: 1 at 120 BPM, about 0.75 at 85,
    // 1.25 at 155 (clamped 0.7..1.4). Glides when the tempo changes, holds through beatless
    // breaks, and eases back to 1 in silence. Multiply animation rates by it.
    float tempoFactor = 1;

    // --- Song structure ---
    float buildUp = 0;          // 0..1: rising tension (risers, rolls, bass pulled out)
    float drop = 0;             // 1 when a drop lands, decays over ~1 s
    uint32_t dropCount = 0;

    float spectrum[kSpectrumBands] = {};      // log-spaced 30 Hz – 16 kHz
    float waveform[kWaveformPoints] = {};     // ~20 ms, -1..1, triggered on a rising zero crossing

    // Oscilloscope data: 25 ms of stereo audio, triggered, auto-scaled to about -1..1.
    // left = scopeMid + scopeSide, right = scopeMid - scopeSide.
    float scopeMid[kScopePoints] = {};
    float scopeSide[kScopePoints] = {};
};
