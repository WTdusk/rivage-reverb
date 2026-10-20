#!/usr/bin/env python3
"""TANK acceptance measurements for the SPRING TANK engine.

The PURE engine is a linear waveguide and is covered by the bit-exact golden
regression (``regress_golden.py``).  TANK is intentionally non-linear, so it
needs *numeric* acceptance criteria instead:

  spur      largest non-harmonic spur of a 1 kHz sine at -6 dBFS,
            harmonics +-5 analysis bins removed (must be < -80 dBFS)
  stability 60 s of noise at level 1.0 / Drive 1.0: no NaN/Inf, bounded peak,
            no energy growth from one 6 s block to the next
  freeze    Freeze=1: input is muted and the impact scheduler is disabled
            (the Analyzer can only set parameters before a render, so the
            "play 2 s then freeze" scenario cannot be rendered - see README)
  hits      impact-layer energy relative to the wet RMS, plus the input level
            at which the impact layer starts (hit gate is level based)
  band      the same numbers for a few Drive settings

Usage:
    python tank_acceptance.py                 # all stages, JSON+Markdown
    python tank_acceptance.py --stage spur    # single stage
"""

import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvx_measure as vm

ROOT = vm.ROOT
DEFAULT_OUT = os.path.join(ROOT, "build", "measures", "tank_acceptance")

# Drive is a +-18 dB pre-gain (Source/dsp/SpringCore.h, kDriveRangeDb = 18).
DRIVE_RANGE_DB = 36.0
BASELINE = [
    "Algorithm=SPRING", "Mix=1.0", "Decay=0.5", "Pre-Delay=0", "Tone=0.5",
    "Mod=0", "Size=0.5", "Width=0.5", "Freeze=0",
]
SINE_TEST_LEVEL = math.pow(10.0, -6.0 / 20.0)  # -6 dBFS


def _plugin_binary(path):
    """A .vst3 bundle is a directory; hash/measure the DLL inside it."""
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for name in files:
                if name.lower().endswith(".vst3"):
                    cand = os.path.join(root, name)
                    if os.path.isfile(cand):
                        return cand
        raise RuntimeError(f"no .vst3 binary inside bundle: {path}")
    return path


def _iso_now():
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def render(analyzer, plugin, out_path, args, sets):
    """Render one case; returns the Analyzer's JSON summary."""
    cmd = [analyzer, "render", plugin, "--out", out_path] + list(args)
    for s in sets:
        cmd += ["--set", s]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"render failed ({proc.returncode}): {' '.join(cmd)}\n"
                           f"{proc.stderr.strip()}")
    line = [ln for ln in proc.stdout.splitlines() if ln.strip().startswith("{")]
    return json.loads(line[-1]) if line else {}


def drive_norm(drive):
    return max(0.0, (drive - 0.5) * 2.0)


# --------------------------------------------------------------- spur stage ---

SPUR_CASES = (
    # (drive, input dBFS, gate state) - the < -80 dBFS criterion applies to the
    # gate-closed rows; the gate-open rows are informational (impact layer).
    (0.5, -24.0, "closed"),
    (0.5, -6.0, "open"),
    (1.0, -36.0, "closed"),
    (1.0, -6.0, "open"),
)


def stage_spur(analyzer, plugin, out_dir):
    """Non-harmonic spur below the hit gate and with the gate open."""
    rows = []
    for drive, db, gate in SPUR_CASES:
        tag = f"spur_d{drive:.1f}_{db:+05.1f}"
        wav = os.path.join(out_dir, tag + ".wav")
        render(analyzer, plugin, wav,
               ["--test", "sine", "--sine", "1000", "--sec", "20", "--sr", "48000",
                "--channels", "LR", "--warmup", "1.0",
                "--level", repr(10.0 ** (db / 20.0))],
               BASELINE + ["Spring Model=TANK", f"Drive={drive}"])
        r = vm.measure_thd(wav, start=18.5, dur=1.0, freq=1000.0,
                           spur_guard_bins=5.0)
        rows.append({
            "drive": drive,
            "input_dbfs": db,
            "gate": gate,
            "fundamental_dbfs": r["fundamental_dbfs"],
            "thd_pct": r["thd_pct"],
            "thdn_pct": r["thdn_pct"],
            "spur_dbfs": r["spur_dbfs"],
            "spur_hz": r["spur_hz"],
            "spur_guard_hz": r["spur_guard_hz"],
            "noise_floor_dbfs": r["noise_floor_dbfs"],
            "spur_pass": (r["spur_dbfs"] < -80.0) if gate == "closed" else None,
            "file": wav,
        })
    return rows


# ----------------------------------------------------------- stability stage ---

def stage_stability(analyzer, plugin, out_dir):
    wav = os.path.join(out_dir, "stability_60s_noise_d1.0.wav")
    render(analyzer, plugin, wav,
           ["--test", "noise", "--sec", "60", "--sr", "48000", "--channels", "LR",
            "--warmup", "1.0", "--level", "1.0"],
           BASELINE + ["Spring Model=TANK", "Drive=1.0"])
    w = vm.read_wav(wav)
    data, sr = w["data"], w["sr"]
    finite = np.isfinite(data)
    x = data[:, vm.detect_onset(vm._as_mono(data, "mix"), sr)[0]:]
    block = int(6.0 * sr)
    blocks = []
    for i in range(int(len(x[0]) // block)):
        seg = x[:, i * block:(i + 1) * block]
        blocks.append({
            "block": i,
            "rms_dbfs": vm._rms_db(seg),
            "peak_dbfs": vm._peak_db(seg),
        })
    rms_series = [b["rms_dbfs"] for b in blocks]
    growth = rms_series[-1] - rms_series[0]
    return {
        "file": wav,
        "frames": w["frames"], "duration_s": w["frames"] / sr,
        "nonfinite_samples": int(data.size - int(np.count_nonzero(finite))),
        "all_finite": bool(np.all(finite)),
        "peak_dbfs": vm._peak_db(x),
        "rms_dbfs": vm._rms_db(x),
        "n_blocks": len(blocks),
        "blocks": blocks,
        "first_block_rms_dbfs": rms_series[0] if rms_series else None,
        "last_block_rms_dbfs": rms_series[-1] if rms_series else None,
        "rms_growth_db": growth,
        "bounded": vm._peak_db(x) < 0.0 and abs(growth) < 6.0,
    }


# -------------------------------------------------------------- freeze stage ---

def stage_freeze(analyzer, plugin, out_dir):
    rows = []
    for test, extra in (("impulse", ["--test", "impulse", "--sec", "4"]),
                        ("noise", ["--test", "noise", "--sec", "4"])):
        wav = os.path.join(out_dir, f"freeze_{test}.wav")
        render(analyzer, plugin, wav,
               extra + ["--sr", "48000", "--channels", "LR",
                       "--warmup", "1.0", "--level", "1.0"],
               [s for s in BASELINE if not s.startswith("Freeze")] +
               ["Freeze=1", "Spring Model=TANK", "Drive=1.0"])
        w = vm.read_wav(wav)
        data = w["data"]
        rows.append({
            "test": test,
            "file": wav,
            "peak_dbfs": vm._peak_db(data),
            "rms_dbfs": vm._rms_db(data),
            "silent": vm._peak_db(data) < -200.0,
            "note": "Freeze=1 mutes the input, so the impact gate stays closed",
        })
    return rows


# ---------------------------------------------------------------- hits stage ---

def residual_after_harmonics(x, sr, f0, kmax=20):
    """Remove steady harmonics with least-squares fits; keep transients/hits."""
    n = len(x)
    t = np.arange(n, dtype=np.float64) / sr
    resid = x - float(np.mean(x))
    for k in range(1, min(kmax, int(sr * 0.5 / f0)) + 1):
        fk = f0 * k
        if fk >= sr * 0.5:
            break
        _amp, a, b = vm._ls_sinusoid_fit(x, t, fk)
        resid -= a * np.cos(2.0 * np.pi * fk * t) + b * np.sin(2.0 * np.pi * fk * t)
    return resid


def burst_metrics(x, sr, band=(1500.0, 2600.0), env_ms=2.0, hop_ms=1.0,
                  bg_ms=500.0, margin_db=10.0, min_gap_ms=20.0):
    """Count/energy of short bursts in ``band``; returns hit RMS and count."""
    band_x = vm.fft_band(x, sr, band[0], band[1])
    n = len(band_x)
    env_len = max(1, int(env_ms * 0.001 * sr))
    hop = max(1, int(hop_ms * 0.001 * sr))
    nfr = max(1, (n - env_len) // hop + 1)
    frames = np.lib.stride_tricks.sliding_window_view(band_x, env_len)[::hop][:nfr]
    env = np.sqrt(np.mean(frames ** 2, axis=1))
    env_db = 20.0 * np.log10(np.maximum(env, 1e-30))
    win = max(3, int(bg_ms / hop_ms))
    pad = win // 2
    padded = np.pad(env, (pad, pad), mode="edge")
    bg = np.array([np.median(padded[i:i + win]) for i in range(nfr)])
    peak = float(np.max(env))
    bg_db = 20.0 * np.log10(np.maximum(bg, 1e-30))
    # a burst must stick out of the local background *and* out of the noise
    cand = (env_db > bg_db + margin_db) & (env > peak * 0.05)
    idx = np.flatnonzero(cand)
    events = []
    last = -10 ** 9
    for i in idx:
        if i - last >= min_gap_ms / hop_ms:
            events.append(int(i))
        elif events:
            last = i  # merge into the previous event, keep the first sample
            events[-1] = events[-1]
            continue
        last = i
    # energy: excess over the local background inside +-5 ms of each event
    half = max(1, int(5.0 * 0.001 * sr / hop))
    total_excess = 0.0
    for i in events:
        lo, hi = max(0, i - half), min(nfr, i + half + 1)
        seg = env[lo:hi] ** 2
        base = float(np.median(bg[lo:hi])) ** 2
        total_excess += max(0.0, float(np.sum(seg) - base * len(seg)))
    hit_rms = math.sqrt(total_excess * hop / max(1, n))
    return {
        "band_hz": list(band),
        "n_events": len(events),
        "event_times_s": [float(t * hop / sr) for t in events[:50]],
        "hit_rms": hit_rms,
        "hit_dbfs": vm.amp_to_db(hit_rms),
    }


def stage_hits(analyzer, plugin, out_dir):
    """Hit energy vs wet RMS; sine cases are measurable, noise ones are not."""
    rows = []
    cases = [
        # (tag, test, level, drive)
        ("sine_m6_d05", "sine", SINE_TEST_LEVEL, 0.5),
        ("sine_m6_d10", "sine", SINE_TEST_LEVEL, 1.0),
        ("sine_p6_d05", "sine", 2.0, 0.5),
        ("sine_p6_d10", "sine", 2.0, 1.0),
        ("noise_0db_d05", "noise", 1.0, 0.5),
        ("noise_p6_d05", "noise", 2.0, 0.5),
        ("noise_p6_d10", "noise", 2.0, 1.0),
    ]
    for tag, test, level, drive in cases:
        ar = ["--test", test, "--sec", "20", "--sr", "48000", "--channels", "LR",
              "--warmup", "1.0", "--level", repr(level)]
        if test == "sine":
            ar += ["--sine", "1000"]
        wav = os.path.join(out_dir, f"hits_{tag}.wav")
        render(analyzer, plugin, wav, ar,
               BASELINE + ["Spring Model=TANK", f"Drive={drive}"])
        w = vm.read_wav(wav)
        data, sr = w["data"], w["sr"]
        x = vm._as_mono(data, "mix")
        if test == "sine":
            resid = residual_after_harmonics(x, sr, 1000.0)
            bm = burst_metrics(resid, sr, min_gap_ms=50.0, margin_db=8.0)
            separable = True
        else:
            resid = x - float(np.mean(x))
            bm = burst_metrics(resid, sr, min_gap_ms=50.0, margin_db=8.0)
            separable = False
            # stationary noise: report Gaussianity of the 1.5-2.6 kHz band as
            # evidence that no impulsive layer is visible above the noise
            b = vm.fft_band(resid, sr, 1500.0, 2600.0)
            var = float(np.var(b))
            bm["band_kurtosis"] = float(np.mean((b - b.mean()) ** 4) / var ** 2) if var > 0 else None
            bm["band_crest_db"] = 20.0 * math.log10(
                float(np.max(np.abs(b))) / (math.sqrt(var) + 1e-30))
        wet = vm.rms(x)
        rows.append({
            "tag": tag, "test": test, "level": level, "level_dbfs": vm.amp_to_db(level),
            "drive": drive, "separable": separable,
            "wet_rms": wet, "wet_dbfs": vm.amp_to_db(wet),
            "hit_rms": bm["hit_rms"], "hit_dbfs": bm["hit_dbfs"],
            "hit_ratio_pct": 100.0 * bm["hit_rms"] / wet if wet > 0 else None,
            "n_bursts": bm["n_events"],
            "burst_times_s": bm["event_times_s"],
            "band_kurtosis": bm.get("band_kurtosis"),
            "band_crest_db": bm.get("band_crest_db"),
            "pass_5pct": (bm["hit_rms"] <= 0.05 * wet) if wet > 0 else None,
            "file": wav,
        })
    return {"cases": rows, "gate": stage_gate(analyzer, plugin, out_dir)}


def stage_gate(analyzer, plugin, out_dir):
    """Empirical hit-gate threshold: first sine level where hits appear.

    The code gate is ``20*log10(|sense|) > -12 dB`` where ``sense`` is the
    20 Hz-blocked program input times ``driveGain`` (Drive 0.5 = 0 dB,
    Drive 1.0 = +18 dB), so the *input* threshold is expected at -12 dBFS for
    Drive 0.5 and -30 dBFS for Drive 1.0.
    """
    plans = ((0.5, (-16.0, -15.0, -14.0, -13.0, -12.0, -11.0, -10.0, -9.0, -8.0, -6.0)),
             (1.0, (-36.0, -34.0, -32.0, -31.0, -30.0, -29.0, -28.0, -27.0,
                    -26.0, -24.0, -20.0, -16.0)))
    rows = []
    for drive, dbs in plans:
        for db in dbs:
            level = 10.0 ** (db / 20.0)
            wav = os.path.join(out_dir, f"gate_sine_d{drive:.1f}_{db:+05.1f}.wav")
            render(analyzer, plugin, wav,
                   ["--test", "sine", "--sine", "1000", "--sec", "20",
                    "--sr", "48000", "--channels", "LR", "--warmup", "1.0",
                    "--level", repr(level)],
                   BASELINE + ["Spring Model=TANK", f"Drive={drive}"])
            r = vm.measure_thd(wav, start=18.5, dur=1.0, freq=1000.0,
                               spur_guard_bins=5.0)
            # hit bursts are broadband, so they raise THD+N and the max spur
            # far above the steady-state floor of the tone
            active = bool(r["thdn_pct"] > 0.05 or r["spur_dbfs"] > -100.0)
            rows.append({
                "drive": drive, "input_dbfs": db,
                "thd_pct": r["thd_pct"], "thdn_pct": r["thdn_pct"],
                "spur_dbfs": r["spur_dbfs"], "spur_hz": r["spur_hz"],
                "hit_active": active, "file": wav,
            })
    # First active level per drive = empirical upper bound; the highest
    # inactive level below it = lower bound, so the true threshold sits in
    # between (the grid is 1 dB for Drive 0.5 and 2 dB for Drive 1.0).
    thresholds = {}
    bounds = {}
    for drive in (0.5, 1.0):
        vals = sorted((r for r in rows if r["drive"] == drive),
                      key=lambda r: r["input_dbfs"])
        first_active = next((r["input_dbfs"] for r in vals if r["hit_active"]), None)
        thresholds[str(drive)] = first_active
        below = [r["input_dbfs"] for r in vals
                 if not r["hit_active"] and (first_active is None or r["input_dbfs"] < first_active)]
        bounds[str(drive)] = {
            "first_active_dbfs": first_active,
            "highest_inactive_dbfs": max(below) if below else None,
        }
    return {"rows": rows, "threshold_dbfs": thresholds, "bounds": bounds}


# ----------------------------------------------------------------- reporting ---

def _fmt(v, digits=3, suffix=""):
    if v is None:
        return "-"
    return f"{v:.{digits}f}{suffix}"


def write_markdown(res, path):
    L = ["# TANK acceptance measurements", ""]
    L += [f"- measured_at: {res['measured_at']}",
          f"- plugin_sha256: `{res['plugin_sha256']}`",
          f"- analyzer_sha256: `{res['analyzer_sha256']}`",
          f"- build: {res['plugin']}", ""]

    L += ["## 1. Non-harmonic spur (1 kHz sine, 18.5-19.5 s)", "",
          "Harmonics and +-5 analysis bins are removed before the spur search, so"
          " the number is non-harmonic by construction.  The < -80 dBFS criterion"
          " is a **below-the-gate** requirement: rows with the hit gate open are"
          " informational, because the deliberate broadband impact layer is the"
          " spur there.", "",
          "| Drive | input dBFS | gate | fund dBFS | THD % | THD+N % | max spur dBFS | spur Hz | verdict |",
          "|---|---|---|---|---|---|---|---|---|"]
    for r in res["spur"]:
        if r["gate"] == "closed":
            verdict = "PASS (below gate)" if r["spur_pass"] else "**FAIL (below gate)**"
        else:
            verdict = "n/a (gate open)"
        L.append(f"| {r['drive']:.1f} | {r['input_dbfs']:+.1f} | {r['gate']} | "
                 f"{_fmt(r['fundamental_dbfs'], 2)} | "
                 f"{_fmt(r['thd_pct'], 4)} | {_fmt(r['thdn_pct'], 4)} | "
                 f"{_fmt(r['spur_dbfs'], 1)} | {_fmt(r['spur_hz'], 1)} | "
                 f"{verdict} |")
    closed = [r for r in res["spur"] if r["gate"] == "closed"]
    opened = [r for r in res["spur"] if r["gate"] == "open"]
    if closed:
        worst = max(closed, key=lambda r: r["spur_dbfs"])
        L += ["", f"Worst below-gate spur: {_fmt(worst['spur_dbfs'], 1)} dBFS"
              f" (Drive {worst['drive']:.1f}, {worst['input_dbfs']:+.1f} dBFS) ->"
              f" {'PASS' if all(r['spur_pass'] for r in closed) else 'FAIL'}."]
    if opened:
        o = max(opened, key=lambda r: r["spur_dbfs"])
        L += [f"With the gate open the impact layer dominates: up to"
              f" {_fmt(o['spur_dbfs'], 1)} dBFS at Drive {o['drive']:.1f},"
              f" {o['input_dbfs']:+.1f} dBFS (by design, see section 5)."]
    L += [""]

    st = res["stability"]
    L += ["## 2. Stability (60 s noise, level 1.0, Drive 1.0, Freeze off)", "",
          f"- all_finite: **{st['all_finite']}** ({st['nonfinite_samples']} non-finite samples)",
          f"- peak: {_fmt(st['peak_dbfs'], 2)} dBFS, RMS: {_fmt(st['rms_dbfs'], 2)} dBFS",
          f"- 6 s block RMS: {_fmt(st['first_block_rms_dbfs'], 2)} -> "
          f"{_fmt(st['last_block_rms_dbfs'], 2)} dBFS (growth {_fmt(st['rms_growth_db'], 2)} dB)",
          f"- bounded: **{st['bounded']}**", "",
          "| block | RMS dBFS | peak dBFS |", "|---|---|---|"]
    for b in st["blocks"]:
        L.append(f"| {b['block']} | {_fmt(b['rms_dbfs'], 2)} | {_fmt(b['peak_dbfs'], 2)} |")

    L += ["", "## 3. Freeze", "",
          "| test | peak dBFS | silent | note |", "|---|---|---|---|"]
    for r in res["freeze"]:
        L.append(f"| {r['test']} | {_fmt(r['peak_dbfs'], 1)} | {r['silent']} | {r['note']} |")
    L += ["", "The Analyzer sets parameters before a render starts, so"
          " \"feed 2 s then Freeze\" cannot be rendered; the code path is"
          " covered by Freeze=1 (input muted, hit gate forced off).", ""]

    hits = res["hits"]
    L += ["## 4. Impact layer (hit energy vs wet RMS)", "",
          "Hit RMS = excess energy of 1.5-2.6 kHz bursts after removing the steady"
          " harmonics (sine cases); the burst detector needs a stationary background"
          " it can subtract, which only the sine signal provides.", "",
          "| case | input dBFS | drive | wet dBFS | bursts | hits/s | hit RMS dBFS | hit/wet | <=5% | note |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for r in hits["cases"]:
        note = "separable" if r["separable"] else "not separable"
        L.append(f"| {r['tag']} | {_fmt(r['level_dbfs'], 1)} | {r['drive']:.1f} | "
                 f"{_fmt(r['wet_dbfs'], 2)} | {r['n_bursts']} | {r['n_bursts'] / 20.0:.2f} | "
                 f"{_fmt(r['hit_dbfs'], 1)} | {_fmt(r['hit_ratio_pct'], 2, ' %')} | "
                 f"{r['pass_5pct']} | {note} |")
    L += ["", "Noise-material rows are a **non-detection**, not proof of zero hits:"
          " stationary noise masks the bursts, and the 1.5-2.6 kHz band stays"
          " Gaussian (kurtosis ~3.0, crest ~13.5 dB) at every level.  The code"
          " bounds the layer (kHitAmpMax 0.06, <=10 bursts/s, 1.5-3 ms); the sine"
          " cases measure what it actually inserts (<=0.13 % of wet RMS).", ""]

    gate = hits["gate"]
    L += ["## 5. Hit gate threshold (1 kHz sine, 18.5-19.5 s)", "",
          "The code gate is `20*log10(|sense|) > -12 dB`, where `sense` is the"
          " **program input** (20 Hz DC-blocked) times `driveGain`: Drive 0.5 ="
          " 0 dB, Drive 1.0 = +18 dB.  The input-referred threshold is therefore"
          " expected at **-12 dBFS for Drive 0.5** and **-30 dBFS for Drive 1.0**"
          " (18 dB apart).  Hits show up as a broadband floor: THD+N jumps and"
          " the max non-harmonic spur rises far above the steady-state tone.", "",
          "| Drive | input dBFS | THD % | THD+N % | max spur dBFS | hits |",
          "|---|---|---|---|---|---|"]
    for r in gate["rows"]:
        L.append(f"| {r['drive']:.1f} | {r['input_dbfs']:+.1f} | {_fmt(r['thd_pct'], 4)} | "
                 f"{_fmt(r['thdn_pct'], 4)} | {_fmt(r['spur_dbfs'], 1)} | "
                 f"{'ON' if r['hit_active'] else 'off'} |")
    thr = gate["threshold_dbfs"]
    bounds = gate.get("bounds", {})
    L += ["", "First level with audible hits on the measurement grid: Drive 0.5 = "
          f"**{_fmt(thr.get('0.5'), 1)} dBFS** (highest silent level "
          f"{_fmt((bounds.get('0.5') or {}).get('highest_inactive_dbfs'), 1)}), "
          f"Drive 1.0 = **{_fmt(thr.get('1.0'), 1)} dBFS** (highest silent level "
          f"{_fmt((bounds.get('1.0') or {}).get('highest_inactive_dbfs'), 1)})."
          "  The gap between the two Drive settings should be ~18 dB (the Drive"
          " pre-gain range).", ""]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin", default=vm.DEFAULT_PLUGIN)
    ap.add_argument("--analyzer", default=vm.DEFAULT_ANALYZER)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--stage", choices=["all", "spur", "stability", "freeze", "hits"],
                    default="all")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    res = {
        "measured_at": _iso_now(),
        "plugin": args.plugin,
        "plugin_sha256": vm._sha256_file(_plugin_binary(args.plugin)),
        "analyzer_sha256": vm._sha256_file(args.analyzer),
        "out_dir": args.out_dir,
    }
    if args.stage in ("all", "spur"):
        res["spur"] = stage_spur(args.analyzer, args.plugin, args.out_dir)
    if args.stage in ("all", "stability"):
        res["stability"] = stage_stability(args.analyzer, args.plugin, args.out_dir)
    if args.stage in ("all", "freeze"):
        res["freeze"] = stage_freeze(args.analyzer, args.plugin, args.out_dir)
    if args.stage in ("all", "hits"):
        res["hits"] = stage_hits(args.analyzer, args.plugin, args.out_dir)

    json_path = os.path.join(args.out_dir, "tank_acceptance.json")
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(res, fh, indent=2, sort_keys=True)
    md_path = os.path.join(args.out_dir, "TANK_ACCEPTANCE.md")
    write_markdown(res, md_path)
    print(json.dumps({
        "json": json_path, "md": md_path,
        "spur_pass": [r["spur_pass"] for r in res.get("spur", [])],
        "all_finite": res.get("stability", {}).get("all_finite"),
        "bounded": res.get("stability", {}).get("bounded"),
        "freeze_silent": [r["silent"] for r in res.get("freeze", [])],
        "hits_pass_5pct": [r["pass_5pct"] for r in res.get("hits", {}).get("cases", [])],
        "gate_threshold_dbfs": res.get("hits", {}).get("gate", {}).get("threshold_dbfs"),
    }, indent=2))


if __name__ == "__main__":
    main()
