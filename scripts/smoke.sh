#!/usr/bin/env bash
# Smoke matrix: every algorithm, sample rate, block size, plus repeat runs and a
# zero-length render. Fails loudly on the first non-zero exit code.
set -u

EXE="build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe"
PLUGIN="build/Source/RivageReverb_artefacts/Release/VST3/Rivage Reverb.vst3"
OUT="build/smoke"
mkdir -p "$OUT"

fails=0
n=0

run() {
    local label="$1"; shift
    n=$((n + 1))
    "$EXE" render "$PLUGIN" "$@" >/dev/null 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        fails=$((fails + 1))
        printf 'FAIL %-18s rc=%s\n' "$label" "$rc"
    fi
}

for m in ROOM PLATE HALL SPRING SHIMMER CLOUD; do
    run "mode:$m" --out "$OUT/mode_$m.wav" --sec 2 --set "param0=$m"
done

for sr in 44100 96000; do
    run "sr:$sr" --out "$OUT/sr_$sr.wav" --sec 2 --sr $sr
done

for bs in 1 32 64 512 1024; do
    run "bs:$bs" --out "$OUT/bs_$bs.wav" --sec 2 --bs $bs
done

for i in $(seq 1 12); do
    run "repeat:$i" --out "$OUT/rep.wav" --sec 1
done

run "sec0" --out "$OUT/zero.wav" --sec 0

printf 'SMOKE: %d/%d passed\n' $((n - fails)) "$n"
[ $fails -eq 0 ]
