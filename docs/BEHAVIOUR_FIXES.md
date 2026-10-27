# Freeze and Cloud correctness fixes — 2026-10-26

These changes address implementation defects without redesigning the existing
voicings or introducing a GPU backend.

## Freeze

All seven engines now close their excitation paths and approach unity feedback
with the existing parameter ramp. Room also closes its early-reflection and
modal injection paths. Spring Tank's level sensor no longer responds to new
input while frozen. Cloud closes its input rather than its wet output.

At full Freeze, feedback delays become stationary integer delays. Modulation
fades out with the Freeze ramp, avoiding the cumulative energy loss from cubic
fractional interpolation. Room/Plate/Hall bypass loop absorption at full Freeze.
Cloud additionally bypasses its feedback low-pass and fades the contracting
stereo cross-mix to independent loops. Its allpass delays also become integers.
Unfreezing restores the existing delay modulation, absorption and decay settings.

Freeze is a sustained tail, rather than an exact repeated recording. The
transition and subsequent tail can evolve as stored energy redistributes.

## Cloud delay capacity

Previously each allpass buffer held its base delay plus 64 samples, but Size
requested up to 1.5 times the base delay, with up to another 8 ms of modulation.
The delay reader silently clamped the excess. Buffers now include the full
requested range and interpolation guards, allocated in `prepare()`.

No feedback/filter/voicing constants were retuned. Removing the clamp changes
Cloud's sound at formerly clipped settings, including its default Size. The
existing Cloud golden files are retained as evidence of this expected change.
Debug builds assert that requested Cloud diffuser delays fit their allocations.

## Verification

- Release plugin, Analyzer and preview build successfully.
- `freezecheck`: 42 checks across seven engines at 44.1/48/96 kHz. Excite for
  0.5 seconds, freeze for six seconds at minimum Decay, then release for another
  six seconds. Compare frozen output under silence versus new input, and
  compare energy in seconds 1–2 versus seconds 5–6. All frozen-input comparisons
  are sample-identical; measured energy drift stays within 0.49 dB. Release
  reduces final-window energy to less than 1% of the first release window.
- `cloudcheck`: 45 combinations of Size/Mod at 0/0.5/1 and sample rates of
  32/44.1/48/96/192 kHz, at maximum Decay. Release checks finite bounded output;
  Debug additionally exercises the actual allocation assertions.
- Existing `dspcheck` and hosted VST3 `layouttest` pass.
- Golden regression: all 40 non-Cloud cases remain bit-exact (28 original
  cases plus all 12 Spring Tank cases). Three old Cloud cases differ as
  expected: `alg_CLOUD_wet.wav`, `alg_CLOUD_mix35.wav`, `cloud_noise.wav`.
  Original manifests and WAVs are not overwritten.

Run from the repository root in PowerShell:

```powershell
scripts/build.cmd all build Release
$analyzer = 'build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe'
& $analyzer freezecheck
& $analyzer cloudcheck
scripts/build.cmd Analyzer build-debug Debug
& 'build-debug/tools/analyzer/Analyzer_artefacts/Debug/Analyzer.exe' cloudcheck
```

The new Release checks run in Windows CI. Golden comparison against the
unchanged old manifest deliberately reports the three Cloud differences;
inspect the named cases rather than treating that command as an all-pass gate.

Room's fixed modal frequencies and the Plate/Spring/Shimmer voicings remain
unchanged. Proposed acoustic-model or timbral redesigns were excluded to respect
the existing tuning.
