#!/usr/bin/env python3
"""Bit-exact golden regression for Rivage Reverb.

Reads `build/golden/manifest.tsv` (sha256<TAB>filename<TAB>render-args...),
re-renders every case with the current plugin build into `build/measures/regress/`
and compares sample-by-sample with `build/golden/<filename>`.

A case PASSES only when all three hold:
  * the re-rendered WAV sha256 equals the manifest sha256,
  * `max|diff| == 0` sample-by-sample against the golden WAV,
  * the data blocks are bitwise identical (no -0.0 / NaN payload drift).

The expectation for the SPRING "TANK" work is 100% bit-exactness for PURE and
for the other five algorithms: any `max|diff| != 0` is reported as FAIL.

Usage:
  python tools/analysis/regress_golden.py                       # current build
  python tools/analysis/regress_golden.py --plugin path/to.vst3 # any build
  python tools/analysis/regress_golden.py --only SPRING         # substring filter

Exit code: 0 = all compared cases pass, 1 = at least one FAIL, 2 = setup error.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvx_measure as vm  # noqa: E402

ROOT = vm.ROOT
DEFAULT_MANIFEST = os.path.join(ROOT, "build", "golden", "manifest.tsv")
DEFAULT_GOLDEN = os.path.join(ROOT, "build", "golden")
DEFAULT_OUT = os.path.join(ROOT, "build", "measures", "regress")
DEFAULT_ANALYZER = vm.DEFAULT_ANALYZER
DEFAULT_PLUGIN = vm.DEFAULT_PLUGIN


def read_manifest(path):
    """Yield (sha256, filename, args) skipping comments/provenance/malformed lines."""
    cases = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            head = line.lstrip()
            if head.startswith("#") or head.startswith("---") or head.startswith("- "):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                print(f"[skip line {lineno}] not a manifest entry: {line[:80]!r}",
                      file=sys.stderr)
                continue
            want, name, args = parts[0].strip(), parts[1].strip(), parts[2].strip()
            if len(want) != 64 or not os.path.splitext(name)[1].lower() == ".wav":
                print(f"[skip line {lineno}] unexpected entry: {line[:80]!r}",
                      file=sys.stderr)
                continue
            cases.append({"sha256": want, "name": name, "args": args, "line": lineno})
    return cases


def render(analyzer, plugin, out_path, args, log_fh):
    cmd = [analyzer, "render", plugin, "--out", out_path] + shlex.split(args)
    log_fh.write("$ " + " ".join(cmd) + "\n")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    log_fh.write(proc.stdout)
    if proc.stderr:
        log_fh.write(proc.stderr)
    log_fh.flush()
    return proc.returncode, proc.stdout, proc.stderr


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--plugin", default=DEFAULT_PLUGIN,
                   help="VST3 bundle or binary to test")
    p.add_argument("--analyzer", default=DEFAULT_ANALYZER)
    p.add_argument("--manifest", default=DEFAULT_MANIFEST)
    p.add_argument("--golden-dir", default=DEFAULT_GOLDEN)
    p.add_argument("--out-dir", default=DEFAULT_OUT)
    p.add_argument("--only", default=None,
                   help="only filenames containing this substring")
    p.add_argument("--fail-fast", action="store_true")
    p.add_argument("--json", default=None, help="write a JSON summary to this path")
    args = p.parse_args(argv)

    for path, what in ((args.plugin, "plugin"), (args.analyzer, "analyzer"),
                       (args.manifest, "manifest")):
        if not os.path.exists(path):
            print(f"ERROR: {what} not found: {path}", file=sys.stderr)
            return 2

    cases = read_manifest(args.manifest)
    if args.only:
        cases = [c for c in cases if args.only in c["name"]]
    if not cases:
        print("ERROR: no manifest cases selected", file=sys.stderr)
        return 2

    os.makedirs(args.out_dir, exist_ok=True)
    log_path = os.path.join(args.out_dir, "regress.log")
    results = []
    with open(log_path, "w", encoding="utf-8") as log_fh:
        header = (f"# golden regression\n# plugin:   {args.plugin}\n"
                  f"# analyzer: {args.analyzer}\n# manifest: {args.manifest}\n"
                  f"# cases:    {len(cases)}\n")
        log_fh.write(header)
        print(header, end="")
        for c in cases:
            name = c["name"]
            golden = os.path.join(args.golden_dir, name)
            out = os.path.join(args.out_dir, name)
            res = {"name": name, "sha256_expected": c["sha256"],
                   "status": "FAIL", "detail": ""}
            if not os.path.exists(golden):
                res.update(status="SKIP", detail="golden file missing")
                results.append(res)
                print(f"SKIP {name}: golden file missing")
                continue
            if os.path.exists(out):
                os.remove(out)
            rc, _out, err = render(args.analyzer, args.plugin, out, c["args"], log_fh)
            if rc != 0 or not os.path.exists(out):
                res.update(detail=f"render rc={rc}: {err.strip()[:200]}")
                results.append(res)
                print(f"FAIL {name}: render rc={rc}")
                if args.fail_fast:
                    break
                continue

            cmp = vm.compare_files(golden, out)
            sha = vm._sha256_file(out)
            res.update(sha256_got=sha, max_abs_diff=cmp["max_abs_diff"],
                       rel_peak_error=cmp["rel_peak_error"],
                       first_diff_sample=cmp["first_diff_sample"],
                       first_diff_time_s=cmp["first_diff_time_s"],
                       bitwise_identical=cmp["bitwise_identical"])
            ok = (sha == c["sha256"] and cmp["max_abs_diff"] == 0.0
                  and cmp["bitwise_identical"])
            if ok:
                res["status"] = "PASS"
                print(f"PASS {name}  sha256 ok, max|diff| 0")
            else:
                res["detail"] = (f"sha256 {'ok' if sha == c['sha256'] else 'DIFF'}, "
                                 f"max|diff| {cmp['max_abs_diff']:.3e} at sample "
                                 f"{cmp['first_diff_sample']} "
                                 f"({cmp['first_diff_time_s']:.6f} s)")
                print(f"FAIL {name}  {res['detail']}")
                if args.fail_fast:
                    results.append(res)
                    break
            results.append(res)

    passed = sum(1 for r in results if r["status"] == "PASS")
    failed = [r for r in results if r["status"] == "FAIL"]
    skipped = sum(1 for r in results if r["status"] == "SKIP")
    print()
    print(f"RESULT: PASS={passed} FAIL={len(failed)} SKIP={skipped} "
          f"(of {len(cases)})   log: {os.path.relpath(log_path, ROOT)}")
    if failed:
        print("FAILED CASES:")
        for r in failed:
            print(f"  - {r['name']}: {r['detail']}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"plugin": args.plugin, "analyzer": args.analyzer,
                       "results": results, "pass": passed,
                       "fail": len(failed), "skip": skipped}, fh, indent=2)
        print(f"wrote {args.json}")
    return 0 if not failed and skipped == 0 else (0 if not failed else 1)


if __name__ == "__main__":
    sys.exit(main())
