# Rivage Reverb — analysis & measurement toolkit

Everything here drives the existing offline host
`build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe` and measures the
float32 WAVs it writes.  No separate VST3 host, no plugin code changes.

| file | purpose |
|---|---|
| `rvx_measure.py` | CLI + library: WAV reader, THD, RT60, centroid, events, compare, level sweep |
| `regress_golden.py` | bit-exact golden regression against `build/golden/manifest.tsv` (or a manifest passed with `--manifest`) |
| `run_baseline.py` | one-shot baseline report (WAVs + `BASELINE_<tag>.md` + `baseline_<tag>.json`) |
| `gate_scan.py` | level sweep of the TANK impact gate (hit-band energy vs fundamental) |
| `burst_tail.py` | loud/quiet noise burst → tail RT60, showing the level dependent damping |
| `tank_regimes.py` | the four TANK regimes (clean / warm / compressed / impacting) in one table |
| `tank_acceptance.py` | TANK acceptance battery (gate off, hit energy, stability, CPU) |
| `capture_tank_golden.py` | writes the 12-render TANK golden set + manifest + provenance |
| `README.md` | this file |

Any Python 3.9+ with `numpy` works (`pip install numpy`); the scripts also run
under the Python bundled with the desktop app.

Run all commands from the repository root, e.g.
`python tools/analysis/rvx_measure.py info build/golden/alg_SPRING_wet.wav`.

---

## 1. Analyzer CLI — verified semantics

```
Analyzer.exe scan   <plugin.vst3>
Analyzer.exe render <plugin.vst3> --out <file.wav>
        [--sr 48000] [--sec N] [--bs 512] [--warmup 1.0]
        [--test impulse|sweep|noise|sine] [--hits N] [--spacing S]
        [--channels LR|L|R] [--level A] [--set id=norm|text] [--setraw id=natural]
        [--in material.wav] [--sine HZ] [--sweep-end HZ] [--meta]
```

* `--level` is **linear amplitude**: `0.0002` = −74 dBFS, `1.0` = 0 dBFS,
  `2.0` = +6 dBFS.  There is no internal clipping on the wet path.
* `--set id=value`: a **numeric** value is the *normalised* 0..1 parameter value;
  a non-numeric value goes through `getValueForText()` and then a step scan that
  matches `getText()` case-insensitively (`SPRING`, `spring`, `TANK`, `On/Off`,
  `50 %` are all accepted).  `--set Algorithm=0.4` and `--set Algorithm=HALL`
  render byte-identically — numeric is always normalised, never an enum index.
* `--setraw id=value` is accepted by the CLI but **does not work for this
  plugin**: every parameter is declared on a normalised 0..1 range, so the
  analyser exits 2 with `ERROR: parameter <name> has no natural range`.
  Use `--set` for everything; e.g. Pre-Delay 45 ms = `--set Pre-Delay=0.15`
  (normalised × 300 ms, linear).
* Parameters can be addressed by **name** or **`paramN`** (`param0`…).  Names
  work, including names with spaces/hyphens when quoted:
  `--set "Pre-Delay=0" --set "Spring Model=TANK"`.
  (`scan` prints the id as `paramN` because the hosted VST3 parameters are not
  `AudioProcessorParameterWithID`; the analyser resolves names via
  `getName(128)` — this was verified by giving the same `--set` both ways and
  getting byte-identical output.)
* Hidden options that the usage text omits: `--sine <hz>` (default 1000),
  `--sweep-end <hz>`, `--meta`.
* `--warmup` seconds of silence precede everything.  The impulse is injected at
  sample `warmup*sr`; `--sec` is the **total** render length including warmup.
  `--hits N --spacing S` puts N impulses S seconds apart.
* Output WAV: 44-byte RIFF header, `fmt` tag **3 = IEEE float32**, 2 channels,
  frame-interleaved.  `latencySamples` is 0.  Renders are **deterministic**
  (same args → same sha256).
* stdout ends with one JSON line:
  `{"out","material","sampleRate","samples","channels","latencySamples","peak","rms","hits"}`.
  Exit codes: 1 plugin load, 2 bad parameter, 3 WAV write/read.

### Parameter map (current build)

| idx | name | kind | text |
|---|---|---|---|
| 0 | `Algorithm` | choice | ROOM, PLATE, HALL, SPRING, SHIMMER, CLOUD (default HALL) |
| 1 | `Mix` | 0..1 | `0 %` … `100 %` |
| 2 | `Decay` | 0..1 | `0` … `100` |
| 3 | `Pre-Delay` | 0..1 | `0.0 ms` … `300 ms` |
| 4 | `Tone` | 0..1 | `0` … `100` |
| 5 | `Mod` | 0..1 | `0` … `100` (inert for SPRING, see pitfalls) |
| 6 | `Size` | 0..1 | `0` … `100` |
| 7 | `Width` | 0..1 | `0 %` … `200 %` |
| 8 | `Freeze` | bool | `Off` / `On` (`Freeze=1.0`) |
| 9 | `Spring Model` | choice | `PURE` / `TANK` (new SPRING build; default PURE) |
| 10 | `Drive` | 0..1 | `-18.0 dB` … `+18.0 dB` (SPRING only) |
| 11 | `Bypass` | bool | `Off` / `On` |

Stable source parameter ids (`Source/Parameters.h`):
`algorithm, mix, decay, predelay, tone, mod, size, width, freeze` (+ `springModel`,
`drive` after the TANK change).  Use the *display names* above with `--set`;
`paramN` is only a fallback.

---

## 2. `rvx_measure.py`

Conventions used everywhere:

* `read_wav()` is a hand-written RIFF parser (no `wave` module): IEEE float
  32/64, PCM 8/16/24/32, `WAVE_FORMAT_EXTENSIBLE`; mono or stereo; returns
  `data (channels, frames) float64`.
* **Onset** = first sample above −60 dBFS relative to the file peak
  (`detect_onset`).  RT60/centroid/event times are onset-relative (`t_s`) and
  also reported absolute (`t_abs_s`).
* `--json` on every metrics subcommand prints one JSON object.

```
python tools/analysis/rvx_measure.py info     <wav> [--json]
python tools/analysis/rvx_measure.py thd      <wav> [--start 18.5 --dur 1 --freq 1000 --channel L --json]
python tools/analysis/rvx_measure.py rt60     <wav> [--no-bands --json]
python tools/analysis/rvx_measure.py centroid <wav> [--min-db 60 --json]
python tools/analysis/rvx_measure.py events   <wav> [--band-lo 4000 --band-hi 8000 --json]
python tools/analysis/rvx_measure.py compare  <a.wav> <b.wav> [--fail-on-diff] [--json]
python tools/analysis/rvx_measure.py sweep    [<vst3>] --prefix pure --sets ... --levels ...
```

### info
Sample rate, frames/duration, channels, bits/format tag, whole-file peak/RMS/DC
plus per-channel values.  For the "active part" statistics it trims the warmup
silence at the onset.

### thd (steady sine renders only)
Fundamental `f0` is estimated from the spectrum (parabolic interpolation), then
amplitude/phase are found by least squares at exact multiples of `f0`
(`_ls_sinusoid_fit`, no window-leakage bias).  Reports `thd_pct/db`,
`thdn_pct/db`, `f0_hz`, `spur_dbfs/spur_hz` (largest non-harmonic peak) and the
noise floor per bin.  THD+N uses a **band-limited residual**
(`hp_hz` … 0.49·sr) because sub-audio drift would otherwise dominate it.

> Use a **long** sine render (20 s) and analyse the **last second**.  The SPRING
> onset transient is still 40+ dB above the steady state for several seconds
> (RT60 ≈ 2.6 s), so a 4 s render gives a meaningless 100 %+ THD.

### rt60
Schroeder backwards energy integration on the onset-trimmed IR
(`decay_curve`: cumsum of `x²` from the end, truncated at the last 1 ms envelope
frame above *peak−60 dB*).  Fits: EDT 0…−10 dB, T20 −5…−25 dB, T30 −5…−35 dB,
each with r², per channel and per 1/3-octave band
(`THIRD_OCTAVE` = 125, 160, 200, 250, 315, 400, 500, 630, 800, 1k, 1.25k, 1.6k,
2k, 2.5k, 3.15k, 4k, 5k, 6.3k, 8k Hz).
`truncated: true` means the file ended before the envelope fell 60 dB below the
IR peak, i.e. the tail was cut — then T30 is noisy and T20/EDT are safer.

### centroid
Spectrum on Hann-windowed 2048-sample frames, 100 ms hop.  Reports the
energy-weighted centroid over the whole analysed section, its median/p10/p90,
the fraction of energy above `--hf` (default 8 kHz), and a trajectory
(`t_abs_s`, `centroid_hz`, `hf_ratio`, `rms_dbfs`, `valid`).  Frames more than
`--min-db` (default 60) below the loudest frame are flagged `valid: false` and
excluded from the statistics (a reverb tail quickly reaches the float noise
floor; without this the centroid would be random).

### events
Detects transient hits/impacts (not noise events).  Envelope = `--env-ms`
(default 2 ms) RMS at a 1 ms hop, optionally band-passed
(`fft_band`, `--band-lo/--band-hi`).  An event is a local maximum that

1. lies within `--floor-db` (6 dB) of the loudest envelope frame,
2. exceeds the local median (±`--median-ms`/2, 500 ms) by `--margin-db` (6 dB),
3. has risen at least `--rise-db` (16 dB) out of the quietest point of the
   previous `--rise-ms` (100 ms), and
4. comes at least `--refractory` (0.5 s) after the previous event.

Event time is the envelope-peak time; `peak_dbfs` is the broadband peak within
±10 ms.  One physical hit → exactly 1 event for all six algorithms; 3 hits 2 s
apart → 3 events; a stationary tone or noise → 0 events.  Hits closer than the
refractory time merge into one event.  `--band-lo 4000` exposes the SPRING
waveguide's dispersive micro-arrivals (several per hit).

### compare
Sample-aligned comparison of two WAVs: `max_abs_diff`, `rel_peak_error`
(relative to the golden peak), `n_diff_samples`, `first_diff_sample/channel/time`,
`bitwise_identical` (same fmt + identical data bytes), file sha256 equality,
length mismatch handling (shorter file compared over its length).
This is the primitive used by the golden regression.

### sweep
Renders a `tests × levels` matrix and writes CSV + Markdown.

```
python tools/analysis/rvx_measure.py sweep "<vst3>" --prefix pure \
  --levels 0.0002 0.0006 0.002 0.006 0.02 0.063 0.2 0.63 1.0 2.0 \
  --tests impulse,sine,noise \
  --sets "Algorithm=SPRING" "Mix=1.0" "Decay=0.5" "Pre-Delay=0" "Tone=0.5" \
         "Mod=0" "Size=0.5" "Width=0.5" "Freeze=0" \
  --csv-out build/measures/level_sweep_pure.csv \
  --md-out  build/measures/level_sweep_pure.md
```

Defaults: `SWEEP_LEVELS` = the 10 levels above (−74…+6 dBFS); render lengths
`TEST_SECONDS` = impulse 6 s, sine 20 s, noise 4 s, sweep 6 s; sine analysis
window = last second (`--sine-start` auto = `sec−1.5`).  Existing WAVs are
re-used unless `--force`.  CSV columns: see `CSV_COLUMNS`
(`peak/rms/crest/peak_gain_db/rt60…/f0/thd/thdn/spur/centroid/hf_ratio/event_count/events_per_sec/…`).
`peak_gain_db` = output peak dBFS − input level dBFS; it stays constant across
the sweep for a linear model — the quickest linearity check.

---

## 3. `regress_golden.py` — bit-exact golden regression

```
python tools/analysis/regress_golden.py                       # current build
python tools/analysis/regress_golden.py --only SPRING         # subset
python tools/analysis/regress_golden.py --plugin "<other build>.vst3"
```

Reads `build/golden/manifest.tsv`: `sha256<TAB>filename<TAB>render-args...`
(args **without** `--out`; lines starting with `#` or `---` are skipped).
It re-renders into `build/measures/regress/`, compares sample-by-sample with
`build/golden/<filename>` and logs to `build/measures/regress/regress.log`.
A case **PASSES** only if *all three* hold: sha256 equals the manifest value,
`max|diff| == 0`, and the data bytes are bitwise identical.  Exit codes:
0 all pass, 1 any FAIL, 2 setup error.  On the current build **31/31 PASS** —
PURE and the five other algorithms are bit-identical to the golden capture.
The golden was captured before the TANK change (`build/golden/provenance.txt`:
git `51e753ef`, dll `421b6ee5…`, analyzer `8a7b09af…`).

---

## 4. `run_baseline.py` — baseline report

Renders the four probe signals through **`build/measures/baseline_pure/pure_snapshot.vst3`**
(the golden-era binary, sha `421b6ee5…`, copied out of the build tree so later
rebuilds cannot contaminate it), measures them and writes
`build/measures/baseline_pure/BASELINE_pure.md` + `baseline_pure.json` + WAVs.

Default reference settings: `Algorithm=SPRING, Mix=1.0, Decay=0.5,
Pre-Delay=0, Tone=0.5, Mod=0, Size=0.5, Width=0.5, Freeze=0`.
Signals: impulse −6 dBFS (6 s), sine −20 dBFS 1 kHz (20 s), noise −6 dBFS
(6 s), sweep −6 dBFS (6 s); plus a default-setting impulse for all six
algorithms (size/peak/RMS).  Any build can be measured:
`--plugin "<vst3>" --tag tank --sets "Spring Model=TANK" "Drive=0.8"`.

### Baseline numbers (PURE, snapshot)

| measurement | value |
|---|---|
| impulse peak / RMS (onset-trimmed) | −46.59 / −73.67 dBFS (onset 1.0000 s) |
| RT60 EDT / T20 / T30 | 2.631 / 2.575 / 2.565 s (r² 0.997/0.999/1.000, not truncated) |
| spectral centroid (impulse) | 706 Hz median 419 Hz, HF>8 kHz 0.024 % |
| events (impulse) | 1 event at t=1.038 s, peak −46.59 dBFS |
| sine THD / THD+N | 3.39e-5 % (−129.4 dB) / 4.58e-3 % (−86.8 dB) |
| largest spur (1 kHz drive) | −142 dBFS @ 1012.3 Hz |
| noise events | 0 (stationary) |
| default impulse, all six algorithms | 2 306 092 bytes each, peak −9.76 dBFS |

---

## 5. Level-sweep results on disk

| file | content |
|---|---|
| `build/measures/level_sweep_pure.csv` / `.md` | SPRING **PURE**, drive n/a |
| `build/measures/level_sweep_tank.csv` / `.md` | SPRING **TANK**, Drive default 0 dB |
| `build/measures/level_sweep_tank_d08.csv` / `.md` | SPRING **TANK**, `Drive=0.8` |

All of them were measured on plugin binary sha256
`7f83b6eed001aeaed17a152e4cab7eb169beb4592f1048596e0d72f43b17660a`
(2026-10-20 19:23) with analyzer `372b8a83…`; see
`build/measures/MEASUREMENTS_PROVENANCE.txt`.  Re-run the `sweep` command from
§2 after any rebuild — TANK has no golden yet, so those numbers are a snapshot,
not a regression gate.

Current-build observations (TANK files are a development snapshot, not golden):

* **PURE is numerically linear**: `peak_gain_db` = −40.59 dB at *every* level
  from −74 to +6 dBFS, THD 2–4·10⁻⁵ % flat, no clipping; RT60 is identical at
  all levels → no level-dependent behaviour to regress.
* **TANK (current build)** is different physics: T30 2.042 s (vs 2.565 s),
  centroid 337 Hz (vs 706 Hz), impulse peak gain −41.9 dB.  At default
  `Drive` (0.5 = 0.0 dB) the sine THD rises with drive level:
  0.006 % @ −74 dBFS, 0.044 % @ −44 dBFS, 1.37 % @ −14 dBFS, 10.5 % @ 0 dBFS,
  20.4 % @ +6 dBFS.  With `Drive=0.8` the low-level THD is lower (5.7e-4 %
  @ −74 dBFS) but high-level THD stays ~9–23 %.  Report these to the DSP owner;
  they are not judged here (no TANK golden exists).

---

## 6. Known pitfalls

1. **Analyzer WAVs are IEEE float32 (fmt tag 3)** — Python's `wave` module
   rejects them.  Always use `read_wav()` here.
2. **THD needs a settled sine.** Spring RT60 ≈ 2.6 s plus a slow build-up, so
   analyse the last second of a 20 s render.  A 4 s render is dominated by the
   onset transient.
3. **The THD+N floor of ~0.0045 % (−87 dB) is not arithmetic noise**: it is
   slow modal sidebands of the spring (e.g. 12 Hz offset, spur at 1012.3 Hz for
   a 1 kHz drive).  The true THD is −129 dB.  Do not "improve" the measurement
   by chasing this floor.
4. **Low-frequency 1/3-octave bands decay with two slopes.**  Below ~500 Hz the
   Schroeder curve plateaus and T30 blows up to 8–15 s with r² 0.68–0.75 and
   `truncated: true`.  Quote T20 there, or quote the broadband RT60 (well
   behaved: 2.565 s, r² ≈ 1.000).  The flag/r² are there so this cannot be
   silently trusted.
5. **`truncated`** means the render ended before the envelope fell 60 dB below
   the IR peak — the tail was cut, so T30 is unreliable.  Either render longer
   or trust T20/EDT.
6. **SPRING onset**: ~20 ms waveguide propagation delay, an initial silent gap
   (10–15 ms) and a dispersive chirp — the spectral centroid of the IR is only
   ~700 Hz and HF>8 kHz is 0.02 %.  This is expected, not a bug.
7. **Event detector is for hits, not noise.**  Defaults were tuned so that a
   stationary noise render gives 0 events; hits closer than 0.5 s cannot be
   separated.  Raising `--floor-db`/lowering `--refractory` changes what counts
   as a hit — always state the detector settings with the numbers.
8. **`Mod` is inert for SPRING** (PURE does not connect it): `spring_mod100` is
   byte-identical to `alg_SPRING_wet` in the golden set.  Don't use it as a
   SPRING variable.
9. **Choice parameters**: prefer the text form (`--set Algorithm=SPRING`).
   A numeric `--set` is the *normalised* value, not the enum index.
10. **Model/Drive isolation (current build)**: `Spring Model` and `Drive` have
    no effect on ROOM/PLATE/HALL/SHIMMER/CLOUD (bit-identical) and `Drive` has
    no effect in PURE.  `Spring Model=PURE` explicit == default PURE bit-exact.
11. **Binary provenance hashes change on relink.**  The current analyzer is
    `372b8a83…` while `build/golden/provenance.txt` records `8a7b09af…`, yet
    all 31 golden renders still reproduce bit-exactly.  Compare *renders*, not
    tool hashes; record both if provenance matters.
12. **Levels above 0 dBFS are legal** (`--level 2.0` = +6 dBFS in): the WAV is
    float and the wet path can exceed 0 dBFS (the sweep test reached +4.3 dBFS
    from a −6 dBFS chirp input).  Clipping, if any, happens outside this host.
13. Unknown parameter → `ERROR: parameter not found: X`, exit code 2.
14. `--set` values containing spaces need shell quoting; in Python pass
    `args=["--set", "Spring Model=TANK"]`.

## TANK-specific helpers (added with the 0.2.0 spring work)

- `tank_regimes.py` — the four-state level scan (1 kHz sine + noise, levels
  −74…+6 dBFS) whose numbers are quoted in `docs/REPORT.md` §8.0.1.
- `gate_scan.py` — sweeps a 1 kHz sine in level and prints the 1.5–4.5 kHz /
  fundamental ratio, which is how the impact gate threshold is located
  (−11 dBFS at Drive 0.5, −28 dBFS at Drive 1.0).
- `burst_tail.py` — renders a loud/quiet noise burst and measures the T30 of the
  tail that follows, i.e. the level dependent damping (2.35 s vs 2.59 s).
- `capture_tank_golden.py` — captures the 12-case TANK golden set
  (`build/golden/manifest_tank.tsv`); `regress_golden.py` verifies it with
  `--manifest build/golden/manifest_tank.tsv`.

Note: `regress_golden.py` parses manifest args with `shlex.split`, so quoted
`--set "Spring Model=TANK"` entries work.
