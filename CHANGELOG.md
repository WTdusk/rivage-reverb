# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Make Freeze sustain the existing tail and reject new input in all seven
  engines. Remove damping/interpolation loss from fully frozen loops, restore
  normal decay on release, and prevent Cloud Freeze from muting the output.
- Allocate Cloud diffusers for the full Size and Mod range before processing;
  formerly clipped settings now use their requested delays. Cloud renders at
  those settings intentionally differ from the old release.
- Advance Decay, Size, Mod, Freeze and Spring Tank Drive ramps during playback;
  live edits previously left these controls at their last snapped value.
- Smooth Tone over 50 ms instead of jumping straight to the target EQ.
- Fold both reverb channels into mono, duplicate mono input for stereo output,
  and advertise only supported mono/stereo bus layouts.
- Clear shared tone-filter history and the control counter on prepare/reset.
- Handle a fractional-delay wrap that can round to the end of the buffer.

### Performance

- Replace per-read delay-line floating-point modulo and integer remainders with
  bounded wraps, retaining the cubic interpolation and existing buffer lengths.
- Recompute static tank/EQ coefficients only during parameter ramps. Cloud's
  modulation and Spring Tank's input-dependent working point still update.
- Process mono directly without allocating or copying a stereo scratch buffer.

### Added

- Analyzer `dspcheck` and `layouttest` commands, also run in Windows CI, covering
  delay interpolation, live controls, mono fold-down, resets and VST3 layouts.
- Analyzer `freezecheck` and `cloudcheck` commands for sustained-tail behavior,
  rejection of new input, release decay and Cloud parameter boundaries.
- Local optimization measurements and verification notes in
  `docs/OPTIMIZATION.md`.

## [0.2.0] — 2026-10-20

SPRING grows a second machine. The old, strictly linear waveguide is kept
bit-for-bit as `PURE`; the new `TANK` puts the physical chain around it, so the
spring now answers the input level with different behaviour instead of being
one fixed filter.

### Added

- **`SPRING PURE` / `SPRING TANK`**, selected by a new `Spring Model` switch in
  the FREEZE row while SPRING is the active machine. PURE is the 0.1.0 signal
  path, sample for sample (31-render golden set, all bit-exact); it stays the
  default so existing sessions sound identical. TANK has its own 12-render
  golden set (`build/golden/manifest_tank.tsv`) so its timbre is pinned too.
- **`Drive`** knob (±18 dB around its noon detent, 70 % of the gain change
  compensated). While SPRING is selected it replaces the MOD knob, which does
  nothing on a spring. `Drive` is a new parameter; it is neutral on every other
  machine.
- TANK's signal chain, all inside `if constexpr` so PURE is untouched:
  - a drive transducer (tanh around a 0.9 knee with a small static bias, run 2×
    oversampled so the harmonics do not alias, then DC-blocked);
  - a level dependent working point: the loop damping shortens RT60 by up to
    25 % and the output tone follows the material down, both driven by a slow
    envelope follower (10 ms attack / 400 ms release);
  - an **impact layer**: when the input level after the Drive gain crosses
    −12 dBFS (off again below −15 dBFS, 400 ms release) short noise bursts are
    injected into the coils, 0.5 events/s at the threshold rising 1.8× per dB,
    capped at 10 events/s, with a 15 % chance of a quieter second tick 6–12 ms
    later;
  - a saturating pickup (`tanh` with an 0.8 knee) and the ROOM soft-clip curve
    on the output instead of the hard clamp.
- `tools/analysis/`: the measurement toolkit — `rvx_measure.py` (`info`, `thd`,
  `rt60`, `centroid`, `events`, `compare`, `sweep`), `regress_golden.py` (the
  bit-exact regression runner), `run_baseline.py`, plus `gate_scan.py`,
  `burst_tail.py`, `tank_regimes.py` and `capture_tank_golden.py` for the spring
  work. `tools/preview` gained `--set id=value`.
- Golden capture sets (kept local under `build/golden/`, regenerated with
  `tools/analysis/capture_tank_golden.py` and the original capture script): 31
  PURE renders and 12 TANK renders, compared by SHA-256 plus `max|diff| == 0`.

### Changed

- `docs/REPORT.md` §5.4 now documents the two spring machines, §7.1 lists the
  two new parameters, and §8.0 has the TANK level-state measurements.
- Version 0.2.0; editor status bar reads `RIVAGE 0.2`.

### Fixed

- The machine tabs at the bottom of the editor never showed which machine was
  active. `juce::ParameterAttachment` hands back the *denormalised* value, so the
  old callback multiplied an item index by five and always landed out of range.

### Known limitations

- TANK is deliberately non-linear: it fails null tests and its THD rises with
  level (1 kHz sine: 0.02 % at −74 dBFS, 0.08 % at −44, 2.2 % at −14, 11 % at
  0 dBFS). PURE is the numerically linear machine.
- The impact layer has no user control — its threshold, rate and level are fixed
  by design, and it is switched off while Freeze is engaged.

## [0.1.0] — 2026-10-19

First release. Windows x64 VST3, as a source tree, a portable archive and a Windows installer. The binaries are not code-signed.

### Added

- Six algorithms, each with its own signal path:
  - `ROOM` — 14 early-reflection taps, five low-frequency modal resonators, an
    8-line tank with a damped low end.
  - `PLATE` — five cascaded allpass diffusers into a short 8-line tank, no early
    reflections, a fixed 3.8 kHz sheen shelf.
  - `HALL` — four diffuser stages that fade into a 16-line FDN one after another,
    so the tail thickens over its first ~70 ms.
  - `SPRING` — six coil delays, each with 24 first-order allpass stages inside the
    feedback loop for dispersion.
  - `SHIMMER` — the hall tank with an octave-up granular pitch shifter feeding the
    input, so each pass climbs another octave.
  - `CLOUD` — three allpass chains with their own feedback delays, slowly
    modulated; no tank, no early reflections.
- Seven knobs and one switch: Mix, Decay, Pre-Delay, Tone, Mod, Size, Width,
  Freeze.
- Cross-faded algorithm switching: the outgoing machine keeps playing and keeps
  receiving input while it fades out over 0.6 s, so switching does not click or
  cut the tail.
- Low end below 120 Hz is folded to the centre in the wet signal, so the tail does
  not go hollow in mono.
- Zero added latency.
- `tools/analyzer` (headless host: `scan`, `render`, `bench`, `switchtest`),
  `tools/preview` (editor window with `--screenshot`), `scripts/smoke.sh`
  (26 renders), `scripts/build.cmd`, `scripts/install-user.cmd`.
- Editor artwork embedded as binary data.
- Windows installer (`installer/`, built with Inno Setup) that installs to the
  standard VST3 folder, records an uninstaller and ships the licence and notices
  inside the bundle; plus a portable archive built by `scripts/make_zip.py`.

### Known limitations

- Windows x64 only; the bundle is unsigned, so hosts may warn the first time it is
  scanned.
- `ROOM` has a mode gap around 55 Hz, and single-tone wet gain still varies by
  about 12 dB across the 40–170 Hz range (PLATE about 21 dB). This is inherent to
  short tank delays rather than a bug.
- No VST2, AU or AAX builds.

[0.2.0]: https://github.com/WTdusk/rivage-reverb/releases/tag/v0.2.0
[0.1.0]: https://github.com/WTdusk/rivage-reverb/releases/tag/v0.1.0
