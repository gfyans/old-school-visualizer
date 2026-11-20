# Tuning on real music

Two dev tools plus a script for checking the analyzer (tempo, beats, build-ups, drops) against real songs.
They're built with the main project (`.\build.ps1`) into `build\Release\`.

## 1. Record

```powershell
.\build\Release\vis_record.exe            # options: --seconds 75  --out recordings
```

Play some music. The recorder writes one WAV per song into `recordings\`, named after the track (from the
Windows media session), and only records while audio is playing. It stops a song at 75 s, and you can skip
to the next song any time. Press Ctrl+C to finish. It uses the same audio device as the app (the `D` key setting).

**Recordings are copyrighted audio: `recordings/` and `*.wav` are git-ignored. Never commit them.**

A good mix is a few genres: EDM with build-ups and drops, rock or metal, hip-hop, something with
swing or a loose tempo, and something ambient without drums.

## 2. Analyse

```powershell
python tools\analysis\analyze.py                     # all of recordings\*.wav
python tools\analysis\analyze.py path\to\song.wav    # specific files
```

For each recording:
- `vis_replay.exe` runs the app's real `AudioAnalyzer` over it (same 72 fps frames + sample stream) and writes
  `song.wav.csv`, one row per frame.
- An **offline reference** beat tracker (Ellis 2007 dynamic programming over a log-band onset envelope) sees
  the whole song at once, unlike the live tracker.
- **Scores:** tempo agreement (including half/double), beat F-measure (±70 ms) of the confident live grid
  against the reference, time to lock, build-up peak and drop times.
- **Chart:** `song.png` with a 10 s beat zoom (reference vs live grid vs onsets), BPM and confidence over time,
  band levels, and build-ups and drops.

The reference is a strong but not infallible guide: on drumless or very syncopated music, read the chart and
judge by ear.
