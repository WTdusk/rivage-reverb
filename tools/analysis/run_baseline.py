#!/usr/bin/env python3
"""One-shot baseline measurement for the SPRING PURE model.

Renders the four probe signals through the *snapshot* PURE plugin (so the
baseline cannot be contaminated by later rebuilds), measures them with
`rvx_measure.py`, and writes `build/measures/baseline_pure/BASELINE.md` +
`baseline.json` next to the WAVs.  Also records size/peak/RMS of a default-
setting impulse for all six algorithms.

Default reference settings (SPRING): Algorithm=SPRING, Mix=100 %, Decay=50,
Pre-Delay=0, Tone=50, Mod=0, Size=50, Width=100 %, Freeze=off.

Usage:
  python tools/analysis/run_baseline.py                     # snapshot, if present
  python tools/analysis/run_baseline.py --plugin <vst3>     # any build (e.g. TANK)
  python tools/analysis/run_baseline.py --tag tank --sets "Spring Model=TANK" "Drive=0.8"
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvx_measure as vm  # noqa: E402

ROOT = vm.ROOT
SNAPSHOT = os.path.join(ROOT, "build", "measures", "baseline_pure",
                        "pure_snapshot.vst3")
DEFAULT_OUT = os.path.join(ROOT, "build", "measures", "baseline_pure")
ALGORITHMS = ["ROOM", "PLATE", "HALL", "SPRING", "SHIMMER", "CLOUD"]

# |10^(-6/20)| = -6 dBFS, |10^(-20/20)| = -20 dBFS; --level is linear amplitude.
LEVEL_6DB = 10.0 ** (-6.0 / 20.0)
LEVEL_20DB = 10.0 ** (-20.0 / 20.0)

SPRING_SETS = ["Algorithm=SPRING", "Mix=1.0", "Decay=0.5", "Pre-Delay=0",
               "Tone=0.5", "Mod=0", "Size=0.5", "Width=0.5", "Freeze=0"]


def _plugin_binary(path):
    """A .vst3 bundle is a directory; hash the real DLL inside it."""
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for name in files:
                if name.lower().endswith(".vst3"):
                    cand = os.path.join(root, name)
                    if os.path.isfile(cand):
                        return cand
        raise RuntimeError(f"no .vst3 binary inside bundle: {path}")
    return path


def sha256(path):
    path = _plugin_binary(path)
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def render(analyzer, plugin, out, extra, force=False):
    if os.path.exists(out) and not force:
        return {"out": out, "cached": True}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [analyzer, "render", plugin, "--out", out] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"render failed rc={proc.returncode}: {proc.stderr.strip()}")
    for line in reversed(proc.stdout.strip().splitlines()):
        if line.strip().startswith("{"):
            return json.loads(line)
    return {"out": out}


def _safe(d, *keys, default=None):
    cur = d
    for k in keys:
        if not isinstance(cur, dict) or k not in cur:
            return default
        cur = cur[k]
    return cur


def measure_wav(path, sine_start):
    w = vm.read_wav(path)
    data, sr = w["data"], w["sr"]
    mono = vm._as_mono(data, "mix")
    onset, onset_s = vm.detect_onset(mono, sr)
    active = mono[onset:]
    info = {
        "sr": sr, "frames": int(data.shape[1]), "channels": int(data.shape[0]),
        "duration_s": data.shape[1] / sr,
        "file_bytes": os.path.getsize(path),
        "onset_s": onset_s,
        "peak_dbfs": vm._peak_db(active), "rms_dbfs": vm._rms_db(active),
        "dc": float(abs(mono.mean())),
    }
    return data, sr, info


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--plugin", default=SNAPSHOT if os.path.exists(SNAPSHOT)
                   else vm.DEFAULT_PLUGIN)
    p.add_argument("--analyzer", default=vm.DEFAULT_ANALYZER)
    p.add_argument("--out-dir", default=DEFAULT_OUT)
    p.add_argument("--tag", default="pure", help="filename/report tag")
    p.add_argument("--sets", nargs="*", default=None,
                   help="extra --set items appended to the SPRING baseline set")
    p.add_argument("--force", action="store_true", help="re-render existing WAVs")
    p.add_argument("--sec", type=float, default=None,
                   help="override render length for impulse/noise/sweep")
    args = p.parse_args(argv)

    plugin = os.path.abspath(args.plugin)
    if not os.path.exists(plugin):
        print(f"ERROR: plugin not found: {plugin}", file=sys.stderr)
        return 2
    sets = list(SPRING_SETS) + list(args.sets or [])
    os.makedirs(args.out_dir, exist_ok=True)
    tag = args.tag
    sec_i = args.sec or 6.0

    cases = [
        ("impulse", ["--test", "impulse", "--sec", str(sec_i), "--level", repr(LEVEL_6DB)]),
        ("sine", ["--test", "sine", "--sec", "20", "--level", repr(LEVEL_20DB),
                  "--sine", "1000"]),
        ("noise", ["--test", "noise", "--sec", str(sec_i), "--level", repr(LEVEL_6DB)]),
        ("sweep", ["--test", "sweep", "--sec", str(sec_i), "--level", repr(LEVEL_6DB)]),
    ]
    base_common = ["--sr", "48000", "--channels", "LR", "--warmup", "1.0"]
    set_args = []
    for item in sets:
        set_args += ["--set", item]

    report = {"tag": tag, "plugin": plugin, "analyzer": args.analyzer,
              "plugin_sha256": sha256(plugin), "analyzer_sha256": sha256(args.analyzer),
              "sets": sets, "cases": {}, "algorithms": {}}
    print(f"plugin : {plugin}")
    print(f"sha256 : {report['plugin_sha256']}")
    print(f"sets   : {' '.join(sets)}")

    wavs = {}
    for test, extra in cases:
        out = os.path.join(args.out_dir, f"{tag}_{test}.wav")
        summary = render(args.analyzer, plugin, out,
                         base_common + extra + set_args, force=args.force)
        data, sr, info = measure_wav(out, 1.5)
        entry = {"file": os.path.basename(out), "render": summary, "info": info}
        print(f"\n== {test}: {os.path.basename(out)}")
        try:
            if test == "impulse":
                rt = vm.measure_rt60(data, sr, bands=vm.THIRD_OCTAVE)
                entry["rt60"] = rt
                c = vm.measure_centroid(data, sr)
                entry["centroid"] = c
                ev = vm.detect_events(data, sr)
                entry["events"] = ev
                s = vm.rt60_summary(rt)
                print(f"  onset {info['onset_s']:.4f} s  peak {info['peak_dbfs']:.2f} dBFS  "
                      f"rms {info['rms_dbfs']:.2f} dBFS")
                print(f"  RT60 EDT {_fmt(s['edt_s'])} / T20 {_fmt(s['rt60_t20_s'])} / "
                      f"T30 {_fmt(s['rt60_t30_s'])} s  truncated={s['truncated']}")
                print(f"  centroid {c['centroid_hz']:.1f} Hz  HF>{c['hf_hz']:.0f} Hz "
                      f"{100 * c['hf_ratio']:.1f} %  events {ev['event_count']}")
            elif test == "sine":
                t = vm.measure_thd(data, sr, start=18.5, dur=1.0,
                                   freq=1000.0, max_harmonics=20)
                entry["thd"] = t
                print(f"  f0 {t['f0_hz']:.4f} Hz  THD {t['thd_pct']:.6g} % "
                      f"({t['thd_db']:.1f} dB)  THD+N {t['thdn_pct']:.6g} % "
                      f"({t['thdn_db']:.1f} dB)")
                print(f"  spur {t['spur_dbfs']:.1f} dBFS @ {t['spur_hz']:.1f} Hz")
            elif test == "noise":
                c = vm.measure_centroid(data, sr)
                entry["centroid"] = c
                ev = vm.detect_events(data, sr)
                entry["events"] = ev
                print(f"  centroid {c['centroid_hz']:.1f} Hz  HF {100 * c['hf_ratio']:.1f} % "
                      f" events {ev['event_count']} ({ev['events_per_sec']:.3f} /s)")
            elif test == "sweep":
                c = vm.measure_centroid(data, sr)
                entry["centroid"] = c
                print(f"  centroid {c['centroid_hz']:.1f} Hz  HF {100 * c['hf_ratio']:.1f} %")
        except Exception as exc:                    # keep the report alive
            entry["error"] = f"{type(exc).__name__}: {exc}"
            print(f"  ERROR {entry['error']}")
        report["cases"][test] = entry
        wavs[test] = out

    # Six-algorithm default-setting impulse: size / peak / RMS.
    print("\n== default-setting impulse, six algorithms")
    for alg in ALGORITHMS:
        out = os.path.join(args.out_dir, f"algo_{alg}_default_impulse.wav")
        summary = render(args.analyzer, plugin, out,
                         base_common + ["--test", "impulse", "--sec", "6",
                                        "--level", "0.5", "--set",
                                        f"Algorithm={alg}"], force=args.force)
        _data, _sr, info = measure_wav(out, 1.5)
        payload = {"render": summary, "info": info, "file_bytes": info["file_bytes"]}
        report["algorithms"][alg] = payload
        print(f"  {alg:8s} {info['file_bytes']:>8d} bytes  peak "
              f"{info['peak_dbfs']:7.2f} dBFS  rms {info['rms_dbfs']:8.2f} dBFS")

    # ------------------------------------------------------------------ md --
    md = []
    md.append(f"# Baseline measurement — SPRING `{tag}`\n")
    md.append(f"- plugin: `{os.path.relpath(plugin, ROOT)}`\n")
    md.append(f"- plugin sha256: `{report['plugin_sha256']}`\n")
    md.append(f"- analyzer sha256: `{report['analyzer_sha256']}`\n")
    md.append(f"- params: `{'`, `'.join(sets)}`\n")
    md.append("- levels: impulse/noise/sweep −6 dBFS (level 0.5012), sine −20 dBFS "
              "(level 0.1, 1 kHz); 48 kHz, stereo, warmup 1.0 s\n")
    md.append("\n## Probe signals\n")
    md.append("| test | file | frames | peak dBFS | RMS dBFS | notes |\n")
    md.append("|---|---|---|---|---|---|\n")
    for test, e in report["cases"].items():
        notes = ""
        if "rt60" in e:
            s = vm.rt60_summary(e["rt60"])
            notes = (f"RT60 EDT {_fmt(s['edt_s'])} / T20 {_fmt(s['rt60_t20_s'])} / "
                     f"T30 {_fmt(s['rt60_t30_s'])} s, truncated={s['truncated']}")
        elif "thd" in e:
            t = e["thd"]
            notes = (f"f0 {t['f0_hz']:.1f} Hz, THD {t['thd_pct']:.4g} %, "
                     f"THD+N {t['thdn_pct']:.4g} %, spur {t['spur_dbfs']:.1f} dBFS "
                     f"@ {t['spur_hz']:.0f} Hz")
        elif "centroid" in e:
            c = e["centroid"]
            notes = (f"centroid {c['centroid_hz']:.0f} Hz, HF {100 * c['hf_ratio']:.1f} %")
            if "events" in e:
                notes += (f", events {e['events']['event_count']} "
                          f"({e['events']['events_per_sec']:.3f} /s)")
        md.append(f"| {test} | `{e['file']}` | {e['info']['frames']} | "
                  f"{e['info']['peak_dbfs']:.2f} | {e['info']['rms_dbfs']:.2f} | {notes} |\n")

    if "rt60" in report["cases"].get("impulse", {}):
        rt = report["cases"]["impulse"]["rt60"]
        md.append("\n## SPRING PURE impulse — broadband RT60\n")
        bb = rt["broadband"]
        md.append("| metric | L | R | mean | r² |\n|---|---|---|---|---|\n")
        for name, key in (("EDT", "edt_s"), ("T20", "t20_s"), ("T30", "t30_s")):
            md.append(f"| {name} | {_fmt(_safe(bb, 'L', key))} | "
                      f"{_fmt(_safe(bb, 'R', key))} | {_fmt(_safe(bb, 'mean', key))} | "
                      f"{_fmt(_safe(bb, 'mean', key.replace('_s', '_r2')))} |\n")
        if "bands" in rt:
            md.append("\n## SPRING PURE impulse — per-band RT60 (1/3 octave)\n")
            md.append("| band Hz | EDT s | T20 s | T30 s | T30 r² | truncated |\n")
            md.append("|---|---|---|---|---|---|\n")
            for centre, b in rt["bands"].items():
                md.append(f"| {centre} | {_fmt(b.get('edt_s'))} | {_fmt(b.get('t20_s'))} | "
                          f"{_fmt(b.get('t30_s'))} | {_fmt(b.get('t30_r2'))} | "
                          f"{b.get('truncated')} |\n")
        c = report["cases"]["impulse"].get("centroid") or {}
        ev = report["cases"]["impulse"]["events"]
        md.append("\n## SPRING PURE impulse — centroid / events\n")
        md.append(f"- spectral centroid {c.get('centroid_hz', float('nan')):.1f} Hz, "
                  f"median {c.get('centroid_median_hz', float('nan')):.1f} Hz, "
                  f"HF>{c.get('hf_hz', 0):.0f} Hz share {100 * c.get('hf_ratio', 0):.1f} %\n")
        md.append(f"- onset {report['cases']['impulse']['info']['onset_s']:.4f} s; "
                  f"events {ev['event_count']} ({ev['events_per_sec']:.3f} /s)\n")
        if ev["events"]:
            md.append("\n| event t_s | peak dBFS | env dBFS |\n|---|---|---|\n")
            for evt in ev["events"]:
                md.append(f"| {evt['t_s']:.3f} | {evt['peak_dbfs']:.2f} | "
                          f"{evt['env_db']:.1f} |\n")

    md.append("\n## Default-setting impulse, all algorithms\n")
    md.append("| algorithm | bytes | peak dBFS | RMS dBFS |\n|---|---|---|---|\n")
    for alg, e in report["algorithms"].items():
        md.append(f"| {alg} | {e['file_bytes']} | {e['info']['peak_dbfs']:.2f} | "
                  f"{e['info']['rms_dbfs']:.2f} |\n")

    md_path = os.path.join(args.out_dir, f"BASELINE_{tag}.md")
    json_path = os.path.join(args.out_dir, f"baseline_{tag}.json")
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write("".join(md))
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2)
    print(f"\nwrote {os.path.relpath(md_path, ROOT)}")
    print(f"wrote {os.path.relpath(json_path, ROOT)}")
    return 0


def _fmt(value, digits=3):
    return "—" if value is None else f"{value:.{digits}f}"


if __name__ == "__main__":
    sys.exit(main())
