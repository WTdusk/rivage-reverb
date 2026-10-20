"""Level dependent damping: render a loud/quiet noise burst, measure the decay
of the tail that follows it.

The TANK shortens its loop RT60 while the programme level is high, so the tail
right after a loud burst should decay faster than the tail after a quiet one.
"""
import struct, subprocess, sys, os, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
A = ROOT + "/build/tools/analyzer/Analyzer_artefacts/Release/Analyzer.exe"
P = ROOT + "/build/Source/RivageReverb_artefacts/Release/VST3/Rivage Reverb.vst3"
OUT = ROOT + "/build/measures/probe"
os.makedirs(OUT, exist_ok=True)
SR = 48000


def make_material(amplitude, seconds=2.0, path=None):
    rng = np.random.default_rng(7)
    n = int(seconds * SR)
    x = rng.standard_normal(n) * amplitude
    x = np.clip(x, -1.0, 1.0)
    pcm = (x * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    return path


def read_wav(path):
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


def t30(x, sr, start):
    """Schroeder T30 of x[start:], in seconds."""
    seg = x[start:]
    # skip the predelay gap: find where the tail actually starts
    peak = np.max(np.abs(seg))
    first = int(np.argmax(np.abs(seg) > peak * 0.1))
    seg = seg[first:]
    energy = np.cumsum((seg ** 2)[::-1])[::-1]
    energy = 10 * np.log10(energy / max(energy[0], 1e-30) + 1e-30)
    t = np.arange(len(seg)) / sr
    try:
        i5 = int(np.argmax(energy <= -5))
        i35 = int(np.argmax(energy <= -35))
    except ValueError:
        return float("nan")
    if i35 <= i5:
        return float("nan")
    slope = np.polyfit(t[i5:i35], energy[i5:i35], 1)[0]
    return -60.0 / slope


print("material | model | tail T30")
for amp in (0.03, 0.5):
    mat = make_material(amp, path=f"{OUT}/mat_{amp}.wav")
    for model in ("PURE", "TANK"):
        out = f"{OUT}/tail_{model}_{amp}.wav"
        subprocess.run([A, "render", P, "--in", mat, "--out", out, "--sr", str(SR),
                        "--channels", "LR", "--sec", "9", "--set", "Algorithm=SPRING",
                        "--set", "Mix=1.0", "--set", "Pre-Delay=0",
                        "--set", "Spring Model=" + model], capture_output=True)
        sr, x = read_wav(out)
        print(f"  {amp:5.2f}   | {model:4s} | {t30(x, sr, int(2.0*sr)):6.3f} s")
