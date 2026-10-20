#!/usr/bin/env python3
"""Capture (or refresh) the SPRING TANK golden set.

Writes one WAV per case plus ``manifest_tank.tsv`` in the same format as the
PURE ``build/golden/manifest.tsv``::

    sha256<TAB>filename<TAB>render-args...

so the normal regression runner can verify it::

    python tools/analysis/regress_golden.py \
        --manifest build/golden/manifest_tank.tsv \
        --golden-dir build/golden \
        --out-dir build/measures/regress_tank

By default everything is written into ``build/measures/tank_golden/`` (the
verifier's write scope).  The Lead can capture straight into the golden tree::

    python tools/analysis/capture_tank_golden.py --out-dir build/golden

TANK is intentionally non-linear (transducer + pickup saturation, gated impact
layer), so its golden is *only* a regression baseline for the current voicing —
unlike PURE it is not required to match the pre-TANK capture.  The renderer is
deterministic (fixed LCG seed / reset state), so a re-render on the same build
must be bit-identical.

Cases cover: default voicing, Drive=0, Drive=0.5 explicit, Drive=1.0,
high-level sine (impact layer firing), high-level noise, Freeze on, and the
default voicing at 44.1 / 96 kHz.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import rvx_measure as vm  # noqa: E402

DEFAULT_ANALYZER = vm.DEFAULT_ANALYZER
DEFAULT_PLUGIN = vm.DEFAULT_PLUGIN
DEFAULT_OUT = os.path.join(ROOT, "build", "measures", "tank_golden")

BASE = ["Algorithm=SPRING", "Mix=1.0", "Decay=0.5", "Pre-Delay=0",
        "Tone=0.5", "Mod=0", "Size=0.5", "Width=0.5", "Freeze=0"]


def _plugin_binary(path):
    """A .vst3 bundle is a directory; hash/report the DLL inside it."""
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for name in files:
                if name.lower().endswith(".vst3"):
                    cand = os.path.join(root, name)
                    if os.path.isfile(cand):
                        return cand
        raise RuntimeError(f"no .vst3 binary inside bundle: {path}")
    return path


def cases():
    """(filename, render args) — args never contain --out."""
    common = ["--sr", "48000", "--channels", "LR", "--warmup", "1.0"]
    out = []

    def add(name, args, sets, sr=None, channels="LR"):
        a = ["--sr", str(sr or 48000), "--channels", channels, "--warmup", "1.0"]
        out.append((name, a + list(args), list(sets)))

    # default voicing (Drive default 0.5 == "0.0 dB")
    add("tank_default_impulse.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK"])
    add("tank_default_impulse_44k.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK"], sr=44100)
    add("tank_default_impulse_96k.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK"], sr=96000)

    # Drive extremes
    add("tank_drive0_impulse.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK", "Drive=0.0"])
    add("tank_drive05_impulse.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK", "Drive=0.5"])
    add("tank_drive10_impulse.wav",
        ["--test", "impulse", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK", "Drive=1.0"])

    # impact layer firing / not firing
    add("tank_sine_high.wav",
        ["--test", "sine", "--sine", "1000", "--sec", "8", "--level", "2.0"],
        BASE + ["Spring Model=TANK", "Drive=1.0"])
    add("tank_sine_low.wav",
        ["--test", "sine", "--sine", "1000", "--sec", "8", "--level", "0.5012"],
        BASE + ["Spring Model=TANK"])
    add("tank_noise_high.wav",
        ["--test", "noise", "--sec", "6", "--level", "2.0"],
        BASE + ["Spring Model=TANK", "Drive=1.0"])
    add("tank_noise_default.wav",
        ["--test", "noise", "--sec", "6", "--level", "0.5012"],
        BASE + ["Spring Model=TANK"])

    # freeze
    add("tank_freeze_impulse.wav",
        ["--test", "impulse", "--sec", "3", "--level", "1.0"],
        [s for s in BASE if not s.startswith("Freeze")] +
        ["Freeze=1", "Spring Model=TANK", "Drive=1.0"])

    # multi-hit spacing (event detector regression)
    add("tank_hits3.wav",
        ["--test", "impulse", "--sec", "8", "--level", "1.0", "--hits", "3",
         "--spacing", "2.0"],
        BASE + ["Spring Model=TANK", "Drive=1.0"])
    return out


def render(analyzer, plugin, out_path, args, sets):
    cmd = [analyzer, "render", plugin, "--out", out_path] + list(args)
    for s in sets:
        cmd += ["--set", s]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"render failed ({proc.returncode}): {' '.join(cmd)}\n"
                           f"{proc.stderr.strip()}")
    return " ".join(args), sets


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin", default=DEFAULT_PLUGIN)
    ap.add_argument("--analyzer", default=DEFAULT_ANALYZER)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--manifest", default=None,
                    help="default: <out-dir>/manifest_tank.tsv")
    ap.add_argument("--force", action="store_true",
                    help="re-render WAVs that already exist")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    manifest = args.manifest or os.path.join(args.out_dir, "manifest_tank.tsv")
    rows = []
    for name, render_args, sets in cases():
        path = os.path.join(args.out_dir, name)
        if args.force or not os.path.exists(path):
            at, st = render(args.analyzer, args.plugin, path, render_args, sets)
            print(f"rendered {name}")
        else:
            at, st = " ".join(render_args), list(sets)
        sha = vm._sha256_file(path)
        # manifest stores the args in command order, quoting only values with spaces
        flat = list(at.split())
        for s in st:
            flat += ["--set", f'"{s}"' if " " in s else s]
        arg_txt = " ".join(flat)
        rows.append((sha, name, arg_txt))

    rows.sort(key=lambda r: r[1])
    with open(manifest, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"# TANK golden capture — plugin {vm._sha256_file(_plugin_binary(args.plugin))}"
                 f"  analyzer {vm._sha256_file(args.analyzer)}\n")
        fh.write("# format: sha256<TAB>filename<TAB>render-args (no --out)\n")
        for sha, name, arg_txt in rows:
            fh.write(f"{sha}\t{name}\t{arg_txt}\n")
    print(f"wrote {manifest} ({len(rows)} cases)")

    prov = os.path.join(args.out_dir, "provenance_tank.txt")
    with open(prov, "w", encoding="utf-8") as fh:
        fh.write(f"plugin_sha256={vm._sha256_file(_plugin_binary(args.plugin))}\n")
        fh.write(f"analyzer_sha256={vm._sha256_file(args.analyzer)}\n")
        fh.write(f"cases={len(rows)}\n")
    print(f"wrote {prov}")


if __name__ == "__main__":
    main()
