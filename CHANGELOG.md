# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

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

[0.1.0]: https://github.com/WTdusk/rivage-reverb/releases/tag/v0.1.0
