"""Gate behaviour of the TANK impact layer.

A 1 kHz sine is swept in level; the energy in the 1.5-4.5 kHz window (the hit
band) relative to the fundamental is printed. The impact layer should make that
ratio jump as the input crosses the gate threshold.
"""
import struct, subprocess, sys, os
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
A = ROOT + "/build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe"
P = ROOT + "/build/Source/RivageReverb_artefacts/Release/VST3/Rivage Reverb.vst3"
OUT = ROOT + "/build/measures/probe"
os.makedirs(OUT, exist_ok=True)


def render(name, model="TANK", level=0.5, drive=None, sec=6, freq=1000):
    path = f"{OUT}/{name}.wav"
    args = [A, "render", P, "--out", path, "--sr", "48000", "--channels", "LR",
            "--warmup", "1.0", "--test", "sine", "--sine", str(freq),
            "--sec", str(sec), "--level", str(level),
            "--set", "Algorithm=SPRING", "--set", "Mix=1.0", "--set", "Pre-Delay=0",
            "--set", "Spring Model=" + model]
    if drive is not None:
        args += ["--set", "Drive=" + str(drive)]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path):
        print("FAIL", name, r.stdout[-200:]); sys.exit(1)
    return path


def read_mono(path):
    data = open(path, "rb").read()
    pos, fmt, frames = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos+4]
        size = struct.unpack("<I", data[pos+4:pos+8])[0]
        body = data[pos+8:pos+8+size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    nch, sr = fmt[1], fmt[2]
    n = len(frames) // (4 * nch)
    x = np.frombuffer(frames[:n*4*nch], dtype="<f4").astype(np.float64)
    return sr, x.reshape(n, nch)[:, 0]


def band_ratio(path, f0=1000.0):
    sr, x = read_mono(path)
    x = x[int(3.0*sr):int(5.0*sr)]
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x*w)) / (w.sum()/2.0)
    freqs = np.fft.rfftfreq(len(x), 1.0/sr)
    fund = spec[np.argmin(np.abs(freqs - f0))]

    mask = np.zeros(len(freqs), dtype=bool)
    for h in range(1, 9):
        c = h*f0
        if c > sr/2:
            break
        mask |= np.abs(freqs - c) <= 10.0
    band = (freqs >= 1500) & (freqs <= 4500) & ~mask
    e = np.sqrt(np.sum(spec[band]**2))
    return 20*np.log10(e/fund + 1e-12), 20*np.log10(fund + 1e-12)


print("level dBFS | TANK band/fund dB | fund dBFS | PURE band/fund dB")
for db in (-20, -16, -14, -13, -12, -11, -10, -6, 0):
    level = 10**(db/20.0)
    t = band_ratio(render(f"gate_t_{db}", "TANK", level))
    p = band_ratio(render(f"gate_p_{db}", "PURE", level))
    print(f"  {db:6d}    | {t[0]:8.1f}          | {t[1]:7.1f}   | {p[0]:8.1f}")
