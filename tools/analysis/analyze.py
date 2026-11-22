"""Analyse real recordings with the app's own analyzer and score it.

For each WAV it runs vis_replay (the real AudioAnalyzer), computes an *offline*
reference beat track (Ellis 2007 dynamic programming over the whole song, so it
sees the future, unlike the live tracker), then scores the live beat grid against
it and saves a chart next to the recording.

    python tools/analysis/analyze.py recordings/*.wav

Requires numpy, scipy, matplotlib.
"""
import csv
import glob
import os
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import stft

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPLAY = os.path.join(ROOT, "build", "Release", "vis_replay.exe")
FPS = 100.0  # reference onset envelope rate


# --------------------------------------------------------------------------- reference

def load_mono(path):
    rate, data = wavfile.read(path)
    data = data.astype(np.float32)
    if data.ndim > 1:
        data = data.mean(axis=1)
    if np.abs(data).max() > 1.5:  # integer PCM
        data /= 32768.0
    return rate, data


def onset_envelope(y, rate):
    """Onset strength pooled over 40 log-spaced bands (like a mel onset envelope), so
    broadband hats spanning hundreds of FFT bins don't outweigh the kick."""
    hop = int(round(rate / FPS))
    n = 2048 if rate <= 50000 else 4096
    freqs, _, z = stft(y, fs=rate, nperseg=n, noverlap=n - hop, boundary=None, padded=False)
    mag = np.abs(z)
    edges = np.searchsorted(freqs, np.geomspace(30.0, min(11000.0, rate / 2 - 1), 41))
    edges = np.maximum.accumulate(np.maximum(edges, np.arange(41) + 1))   # every band >= 1 bin
    bands = np.stack([mag[lo:max(hi, lo + 1)].mean(axis=0) for lo, hi in zip(edges[:-1], edges[1:])])
    logb = np.log1p(100.0 * bands / (bands.max() + 1e-12))
    flux = np.maximum(0.0, np.diff(logb, axis=1)).sum(axis=0)
    # STFT frames are stamped at their start; shift by half a window so onsets line up in time.
    flux = np.concatenate([np.zeros(int(round(n / 2 / hop))), flux])
    # remove slow trend, normalise
    k = int(FPS)  # 1 s
    trend = np.convolve(flux, np.ones(k) / k, mode="same")
    env = np.maximum(0.0, flux - trend)
    return env / (env.std() + 1e-9)


def reference_tempo(env):
    env = env - env.mean()
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    lags = np.arange(len(ac))
    lo, hi = int(FPS * 60 / 200), int(FPS * 60 / 60)
    bpm = 60.0 * FPS / np.maximum(lags, 1)
    prior = np.exp(-0.5 * (np.log2(bpm / 120.0)) ** 2)
    score = np.zeros_like(ac)
    score[lo:hi] = ac[lo:hi] * prior[lo:hi]
    period = int(np.argmax(score))
    return 60.0 * FPS / period, period


def reference_beats(env, period, tightness=100.0):
    """Ellis (2007) dynamic-programming beat tracker."""
    n = len(env)
    score = env.copy()
    back = -np.ones(n, dtype=int)
    for t in range(n):
        lo, hi = t - 2 * period, t - period // 2
        if hi <= 0:
            continue
        taus = np.arange(max(lo, 0), hi)
        cost = -tightness * np.log((t - taus) / period) ** 2
        cand = score[taus] + cost
        j = int(np.argmax(cand))
        if cand[j] > 0:
            score[t] += cand[j]
            back[t] = taus[j]
    t = int(np.argmax(score[-period:])) + n - period
    beats = []
    while t >= 0:
        beats.append(t)
        t = back[t]
    return np.array(beats[::-1]) / FPS


# --------------------------------------------------------------------------- scoring

def f_measure(est, ref, tol=0.07):
    if len(est) == 0 or len(ref) == 0:
        return 0.0
    used = np.zeros(len(ref), dtype=bool)
    hits = 0
    for e in est:
        d = np.abs(ref - e)
        d[used] = np.inf
        j = int(np.argmin(d))
        if d[j] <= tol:
            used[j] = True
            hits += 1
    p, r = hits / len(est), hits / len(ref)
    return 0.0 if hits == 0 else 2 * p * r / (p + r)


def tempo_match(a, b, tol=0.04):
    return any(abs(a / (b * k) - 1) < tol for k in (1, 0.5, 2, 2 / 3, 1.5))


def grid_jumps(t, grid_beat, bpm, settled):
    beats = np.flatnonzero((grid_beat > 0) & settled)
    if len(beats) < 2:
        return 0.0
    gaps = np.diff(t[beats])
    expected = 60.0 / np.maximum(bpm[beats[1:]], 1.0)
    jumps = int(np.sum(np.abs(gaps / expected - 1.0) > 0.12))
    minutes = (t[beats[-1]] - t[beats[0]]) / 60.0
    return jumps / max(minutes, 1e-6)


def read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return {k: np.array([float(r[k]) for r in rows]) for k in rows[0]}


# --------------------------------------------------------------------------- main

def analyse(wav):
    csv_path = wav + ".csv"
    summary = subprocess.run([REPLAY, wav, csv_path], capture_output=True, text=True, encoding="utf-8").stdout
    live = read_csv(csv_path)
    rate, y = load_mono(wav)
    env = onset_envelope(y, rate)
    ref_bpm, period = reference_tempo(env)
    ref = reference_beats(env, period)

    t = live["t"]
    confident = live["confidence"] > 0.4
    grid = t[(live["grid_beat"] > 0) & confident]
    onsets = t[live["onset_beat"] > 0]
    settled = t > 10.0
    live_bpm = float(np.median(live["bpm"][settled & (live["bpm"] > 0)])) if np.any(settled & (live["bpm"] > 0)) else 0.0
    lock = t[confident][0] if np.any(confident) else float("nan")

    # Score only where both trackers are running (after the live tracker's warm-up).
    ref_s = ref[ref > 10.0]
    grid_s = grid[grid > 10.0]
    f_grid = f_measure(grid_s, ref_s)
    # Half-time locks: compare against every other reference beat (best of both phases).
    f_half = max(f_measure(grid_s, ref_s[0::2]), f_measure(grid_s, ref_s[1::2]))
    f_onset = f_measure(onsets[onsets > 10.0], ref_s)
    result = dict(
        name=os.path.basename(wav)[:-4], live_bpm=live_bpm, ref_bpm=ref_bpm,
        tempo_ok=tempo_match(live_bpm, ref_bpm) if live_bpm else False,
        conf=float(np.median(live["confidence"][settled])) if np.any(settled) else 0.0,
        lock=lock, f_grid=max(f_grid, f_half), f_onset=f_onset,
        drops=list(t[live["drop"] > 0]), build=float(live["build_up"].max()),
        # Phase accuracy regardless of confidence:
        f_all=max(f_measure(t[(live["grid_beat"] > 0) & settled], ref_s),
                  f_measure(t[(live["grid_beat"] > 0) & settled], ref_s[0::2]),
                  f_measure(t[(live["grid_beat"] > 0) & settled], ref_s[1::2])),
        # Grid jumps per minute: consecutive grid beats whose spacing is off the published tempo
        # by more than 12% (a phase hop or a switch of metrical level). This is what shows on
        # screen; F can't see it, since it marks a steady grid at another valid level as wrong.
        jumps=grid_jumps(t, live["grid_beat"], live["bpm"], settled),
        # Median of each tracker diagnostic after warm-up.
        diag={k: float(np.median(live[k][settled])) for k in ("periodicity", "phase_error", "percussive") if k in live},
    )

    # --- chart ---
    fig, ax = plt.subplots(4, 1, figsize=(16, 11), gridspec_kw=dict(height_ratios=[2, 1.2, 1, 1]))
    fig.suptitle(f"{result['name']}\nlive {live_bpm:.1f} BPM vs reference {ref_bpm:.1f} BPM | "
                 f"grid F={result['f_grid']:.2f} onset F={f_onset:.2f} | lock {lock:.1f}s", fontsize=11)
    z0 = 20.0 if t[-1] > 32 else 0.0
    z1 = z0 + 10.0
    ts = np.arange(len(y)) / rate
    m = (ts >= z0) & (ts < z1)
    ax[0].plot(ts[m][::16], y[m][::16], color="0.75", lw=0.5)
    for b in ref[(ref >= z0) & (ref < z1)]:
        ax[0].axvline(b, color="black", lw=1.2, alpha=0.6)
    for b in grid[(grid >= z0) & (grid < z1)]:
        ax[0].axvline(b, ymin=0.5, ymax=1.0, color="tab:cyan", lw=2)
    for b in onsets[(onsets >= z0) & (onsets < z1)]:
        ax[0].axvline(b, ymin=0.0, ymax=0.5, color="tab:orange", lw=2)
    ax[0].set_xlim(z0, z1)
    ax[0].set_title("10 s zoom: black = reference beats, cyan (top) = live grid beats, orange (bottom) = onset beats", fontsize=9)

    ax[1].plot(t, live["bpm"], label="live BPM")
    ax[1].axhline(ref_bpm, color="black", ls="--", label="reference BPM")
    ax1b = ax[1].twinx()
    ax1b.plot(t, live["confidence"], color="tab:green", alpha=0.6, label="confidence")
    ax1b.set_ylim(0, 1.05)
    ax[1].legend(loc="upper left", fontsize=8)
    ax1b.legend(loc="upper right", fontsize=8)

    for k, c in (("bass", "tab:red"), ("lowmid", "tab:orange"), ("highmid", "tab:green"), ("treble", "tab:blue"), ("intensity", "tab:purple")):
        ax[2].plot(t, live[k], color=c, lw=0.8, label=k)
    ax[2].legend(loc="upper left", fontsize=8, ncol=5)
    ax[2].set_ylim(0, 1.05)

    ax[3].plot(t, live["build_up"], color="tab:purple", label="build-up")
    for d in result["drops"]:
        ax[3].axvline(d, color="tab:red", lw=2)
    ax[3].set_ylim(0, 1.05)
    ax[3].set_title("build-up (purple) and drops (red lines)", fontsize=9)
    ax[3].set_xlabel("seconds")
    for a in ax[1:]:
        a.set_xlim(0, t[-1])
    fig.tight_layout()
    fig.savefig(wav[:-4] + ".png", dpi=80)
    plt.close(fig)
    return result, summary


def main():
    files = sorted(sum((glob.glob(a) for a in sys.argv[1:]), [])) or sorted(glob.glob(os.path.join(ROOT, "recordings", "*.wav")))
    if not files:
        print("no recordings found")
        return 1
    results = []
    for wav in files:
        if os.path.getsize(wav) < 2_000_000:   # under ~10 s: too short to score (a stopped recording)
            print(f"{os.path.basename(wav)[:48]:48}  skipped: too short")
            continue
        r, _ = analyse(wav)
        results.append(r)
        print(f"{r['name'][:48]:48}  live {r['live_bpm']:6.1f}  ref {r['ref_bpm']:6.1f}  "
              f"{'OK ' if r['tempo_ok'] else 'BAD'}  conf {r['conf']:.2f}  lock {r['lock']:5.1f}s  "
              f"gridF {r['f_grid']:.2f} (all {r['f_all']:.2f})  jumps/min {r['jumps']:4.1f}  " +
              " ".join(f"{k[:5]} {v:.2f}" for k, v in r["diag"].items()) + f"  build {r['build']:.2f}  "
              f"drops {', '.join(f'{d:.0f}s' for d in r['drops']) or '-'}")
    ok = sum(r["tempo_ok"] for r in results)
    print(f"\ntempo agreement {ok}/{len(results)}, mean grid F {np.mean([r['f_grid'] for r in results]):.2f}, "
          f"mean onset F {np.mean([r['f_onset'] for r in results]):.2f}, "
          f"mean grid jumps/min {np.mean([r['jumps'] for r in results]):.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
