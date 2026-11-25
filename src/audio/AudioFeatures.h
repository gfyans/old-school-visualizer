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
    float beat = 0;        // 1 on a detected beat (the kick drum), decays to 0 in ~0.15 s
    uint32_t beatCount = 0;

    // --- Instruments (lightweight DSP detectors: good for drums, rough for voice vs synth) ---
    float snare = 0;            // 1 on a snare / clap hit, decays in ~0.15 s
    uint32_t snareCount = 0;
    float hat = 0;              // 1 on a hi-hat / shaker / ride hit, decays in ~0.08 s
    uint32_t hatCount = 0;
    float vocal = 0;            // 0..1: a voice-like sound (tonal, pulsing with syllables) in the vocal range
    float pad = 0;              // 0..1: sustained tonal sound (pads, held synths, strings)

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
    // 0..1: how much this is four-on-the-floor dance music (house, techno, trance, EDM): a kick on
    // every beat, each beat of the bar alike, on a steady grid. Builds over ~4 s, holds through
    // beatless breakdowns, resets in silence. When high, the analyzer expects long breakdowns,
    // drops landing on the beat and 8-bar phrases.
    float edm = 0;

    // --- Harmony and mood ---
    int key = 0;                // 0..11 = C, C#, D ... B (the song's key, from the last ~10 s)
    bool minorKey = false;
    float keyConfidence = 0;    // 0..1
    int chordRoot = 0;          // current chord (major or minor triad), 0..11
    bool chordMinor = false;
    uint32_t chordCount = 0;    // increments on every chord change
    float mood = 0;             // -1 calm / dark (slow, minor) .. +1 bright / energetic (fast, major, loud)

    // --- Song structure ---
    float buildUp = 0;          // 0..1: rising tension (risers, rolls, bass pulled out)
    // The big moment, the same for every visualiser (what Q fakes): a drop (the bass slams back in, or
    // the level jumps after a build), a big build-up letting go, or, in a big section that hasn't had
    // one for a while, the start of a phrase. At most one every 6 s.
    float drop = 0;             // 1 when a big moment lands, decays over ~1 s
    uint32_t dropCount = 0;

    float spectrum[kSpectrumBands] = {};      // log-spaced 30 Hz – 16 kHz
    float waveform[kWaveformPoints] = {};     // ~20 ms, -1..1, triggered on a rising zero crossing

    // Oscilloscope data: 25 ms of stereo audio, triggered, auto-scaled to about -1..1.
    // left = scopeMid + scopeSide, right = scopeMid - scopeSide.
    float scopeMid[kScopePoints] = {};
    float scopeSide[kScopePoints] = {};
};
