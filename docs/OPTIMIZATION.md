# DSP optimization — 2026-10-26

This records the initial performance pass. The subsequent Freeze/Cloud
correctness fixes and their expected audio differences are documented in
`BEHAVIOUR_FIXES.md`; the golden results below precede those fixes.

Live parameter edits now advance their smoothing ramps. Previously the tank
controls read only the last snapped value, so changing Decay, Size, Mod, Freeze
or Tank Drive while playing could leave the sound unchanged. The ramps now
advance at the existing 32-sample control rate (50 ms normally, 60 ms for Cloud,
30 ms for Tank Drive). Tone now uses its 50 ms ramp as well.

Delay reads retain the existing buffer lengths and cubic interpolation, with
bounded wrap operations replacing floating modulo and integer remainders. A
rounding edge at the buffer boundary is also handled. Static tank and Tone EQ
coefficients are recalculated only while controls change. Cloud still updates
its drifting delays, and Spring Tank still follows the input envelope.

Mono processing now runs directly through the engine and averages both wet
channels. This removes the callback's temporary stereo buffer allocation and
copies. Mono input is duplicated for stereo output. Supported layouts are
mono/mono, mono/stereo and stereo/stereo. Preparing or resetting the engine now
also clears shared EQ history and resets its control counter.

## Local CPU comparison

Windows x64, MSVC Release; 48 kHz, 64-sample buffer, eight seconds of audio per
mode. Three alternating baseline/optimized runs, median CPU below. Each run
uses the Analyzer `bench` impulse-and-tail workload, including output scanning.
These are local engine timings, rather than a DAW's CPU-meter readings. They
exclude preparation and parameter switching and do not measure automation cost.

| Mode | Before | After | Relative reduction |
| --- | ---: | ---: | ---: |
| ROOM | 10.09% | 8.90% | 11.8% |
| PLATE | 5.59% | 4.94% | 11.6% |
| HALL | 3.99% | 3.08% | 22.8% |
| SPRING PURE | 7.36% | 6.79% | 7.7% |
| SHIMMER | 3.18% | 2.27% | 28.6% |
| CLOUD | 3.83% | 2.45% | 36.0% |

Raw local output: `build/measures/optimization-bench.txt`. The saved baseline
Analyzer is `build/measures/optimization-baseline-analyzer.exe`.

## Verification

- Release VST3, Analyzer and preview build completed successfully.
- All 31 original golden renders and all 12 Spring Tank golden renders match
  SHA-256 and every audio sample exactly. This covers fixed parameter settings;
  live edits intentionally change because automation now works.
- `dspcheck`: 120,000 delay-reader comparisons including wrap and non-finite
  requests; control-ramp completion; all applicable live tank controls across
  seven engines; Tone edit timing; mono fold-down with changing block lengths;
  reset clears tails and EQ state.
- `layouttest`: actual VST3 mono/mono, mono/stereo and stereo/stereo processing,
  and rejection of stereo/mono; 44.1/48/96 kHz, buffers of 0/1/32/64/257/1024
  samples within the declared maximum of 1024; Mix=0 preserves dry routing.
- 27 hosted rendering smoke cases passed: six modes plus Tank, 44.1/96 kHz,
  1/32/64/512/1024-sample blocks, twelve repeated loads and a zero-length render.
- Existing `switchtest` completed: switch delta peak 0.05394, surrounding tail
  delta 0.04000, ratio +2.6 dB. This diagnostic reports continuity measurements;
  it has no automated pass/fail threshold.

Run from the repository root in PowerShell:

```powershell
scripts/build.cmd all build Release
$analyzer = 'build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe'
$plugin = 'build/Source/RivageReverb_artefacts/Release/VST3/Rivage Reverb.vst3'
& $analyzer dspcheck
& $analyzer layouttest $plugin
& $analyzer bench --sr 48000 --bs 64 --sec 8
python tools/analysis/regress_golden.py
python tools/analysis/regress_golden.py --manifest build/golden/manifest_tank.tsv
```

`dspcheck` and `layouttest` are also part of Windows CI. Golden WAVs and raw
measurement files remain local build artifacts.

Mode changes still construct and prepare the incoming algorithm in the audio
callback. This optimization removes mono scratch allocation; it does not yet
remove allocation during mode changes.
