# Roadmap

Ideas for where this could go, roughly in order of impact. ✅ = done.

## 1. Smarter listening
- ✅ **Tempo and beat-grid tracking.** Lock to the song's BPM and *predict* beats, so motion lands exactly on them (including moves across 4/8 bars).
- ✅ **Song structure.** Detect build-ups and drops: tighten and accelerate during a build, explode into a new look on the drop.
- ✅ **Real-music tuning.** Recorder and replay tools; tracker, confidence and structure calibrated on six real songs.
- **Harder rhythms.** Beat placement on thrash metal and syncopated techno (occasional quarter-beat slips), e.g. a
  look-ahead dynamic-programming phase stage. Lock faster after a beatless intro.
- **Mood and key.** Minor/slow songs pull towards calm, cool palettes; energetic tracks go warm and fast. Chord changes shift hue.
- **Instrument separation.** Real-time source separation (kick, snare, vocals, synths) so each can drive its own visual element.
- ✅ **Now playing.** Read title and album art from Windows media controls; take the palette from the album art; show the title at song start.
- ✅ **Equaliser bars overlay.** Stylish glass bars along the bottom, over any visualiser (`B`).

## 2. More visualisers
- ✅ **Milkdrop-style feedback** ("Liquid"). Each frame warps and fades the previous one: infinite tunnels and liquid trails.
- ✅ **Particle galaxy** ("Galaxy"). A million GPU particles around a black hole with lensing; bass drives gravity, beats send shockwaves through the arms, a camera director cuts on downbeats, drops fire a quasar.
- ✅ **Oscilloscope / vector display.** The real waveform on a CRT: phosphor afterglow, curved glass, four modes including an X/Y vectorscope.
- ✅ **Fractal flights.** Flying through five infinite raymarched fractal worlds that breathe with the music; hyperspeed on drops.
- ✅ **Synthwave terrain** ("Neon Grid"). Neon grid landscape whose mountains are the scrolling spectrum history.
- ✅ **Aurora.** Soft ribbons of northern lights over a frozen forest lake, reflected in the ice.
- **More worlds for Fractal flights.** A Mandelbulb set piece and a working Kleinian cave world.

## 3. Nebula upgrades
- ✅ **Temporal accumulation (TAA-style).** Reproject and blend previous frames to remove grain: sharper *and* cheaper.
- **Real light scattering.** Light marching through the gas for bright rims and dark undersides.
- **Bloom** on the core and stars; subtle lens effects.
- **Events.** Comets, a pulsar sweeping on the beat, a supernova on a huge drop.
- **Directed flight paths.** Steer towards interesting dense regions instead of wandering.

## 4. Feel and interaction
- ✅ **Auto-cycle mode.** Switch visualisers on song change or every N minutes.
- ✅ **Transitions with character.** Dissolves, zoom-throughs and warps instead of crossfades.
- **Mouse / gamepad control.** Steer the camera; a "chill ↔ intense" reactivity slider.
- **Presets and favourites.** Save looks you love.
- **Screensaver build.** The same exe as a `.scr`.

## 5. Beyond the screen
- **Multi-monitor.** Span monitors, or a different visualiser on each.
- **RGB sync.** Drive SteelSeries / Razer / Hue lighting from the same analysis and palette.
- **Stream / capture mode.** Chroma-key or transparent window for OBS; one-key 30 s clip recording.
- **Shader hot-reload and live editor.** Write visualisers while music plays; load Shadertoy shaders with audio mapped in.
- **AI visualiser generation.** Describe a scene and have Claude write the shader into the framework.
