#!/usr/bin/env python3
"""rvx_measure.py - measurement toolkit for Rivage Reverb renders.

Reads the 32-bit-float WAV files written by RvxAnalyzer (RIFF parser, no
`wave` module) and offers the metrics we use to accept a build:

  info      sample rate / frames / channels / peak / RMS / DC
  thd       THD, THD+N and largest non-harmonic spur of a sine render
  rt60      Schroeder T20/T30/EDT, broadband and per 1/3-octave band
  centroid  spectral centroid, 100 ms trajectory, high-frequency energy share
  events    onset/impact detection on a 4-8 kHz band (events per second)
  compare   sample-aligned comparison of two WAVs (bit-exactness)
  sweep     render a level x test matrix through RvxAnalyzer and write CSV/MD

All commands print human-readable text by default; pass --json for a
machine-readable object on stdout (the regression script does this).

Timing convention: time-domain metrics are measured from the impulse-response
onset (first sample above -60 dBFS relative to the peak), not from the file
start, so the 1 s --warmup silence and any pre-delay do not distort them.

The module is importable: `compare_files`, `measure_rt60`, `detect_events`,
`measure_thd`, `measure_centroid` and `read_wav` are used by regress_golden.py.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import subprocess
import sys

import numpy as np

# ------------------------------------------------------------------ paths ---

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ANALYZER = os.path.join(
    ROOT, "build", "tools", "analyzer", "Analyzer_artefacts", "Release", "Analyzer.exe")
DEFAULT_PLUGIN = os.path.join(
    ROOT, "build", "Source", "RivageReverb_artefacts", "Release", "VST3", "Rivage Reverb.vst3")
DEFAULT_OUT_DIR = os.path.join(ROOT, "build", "measures")

EPS = 1.0e-30
FLOOR_DB = -999.0

# 1/3-octave centre frequencies from 125 Hz to 8 kHz (19 bands).
THIRD_OCTAVE = [125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000]

# Lead's level-sweep matrix: -74 dBFS .. +6 dBFS (linear amplitudes).
SWEEP_LEVELS = ["0.0002", "0.0006", "0.002", "0.006", "0.02",
                "0.063", "0.2", "0.63", "1.0", "2.0"]


# -------------------------------------------------------------------- WAV ---

def _u16(b, o):
    return int.from_bytes(b[o:o + 2], "little")


def _u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def read_wav(path):
    """Parse a RIFF/WAVE file.

    Returns dict with keys: data (channels, frames) float64, sr, channels,
    frames, bits, format_tag, data_bytes, fmt_key.
    Supports IEEE float 32/64 and integer PCM 8/16/24/32, plain or
    WAVE_FORMAT_EXTENSIBLE, mono or stereo.
    """
    with open(path, "rb") as fh:
        raw = fh.read()

    if len(raw) < 12 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"not a RIFF/WAVE file: {path}")

    pos = 12
    fmt = None
    data_chunks = []

    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        size = _u32(raw, pos + 4)
        body = raw[pos + 8:pos + 8 + size]

        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            data_chunks.append(body)

        pos += 8 + size + (size & 1)

    if fmt is None or not data_chunks:
        raise ValueError(f"missing fmt/data chunk: {path}")

    tag = _u16(fmt, 0)
    channels = _u16(fmt, 2)
    sr = _u32(fmt, 4)
    bits = _u16(fmt, 14)

    if tag == 0xFFFE and len(fmt) >= 26:          # WAVE_FORMAT_EXTENSIBLE
        tag = _u16(fmt, 24)                       # first 2 bytes of the subformat GUID

    data_bytes = b"".join(data_chunks)

    if tag == 3:
        if bits == 32:
            arr = np.frombuffer(data_bytes, "<f4").astype(np.float64)
        elif bits == 64:
            arr = np.frombuffer(data_bytes, "<f8").astype(np.float64)
        else:
            raise ValueError(f"unsupported float width {bits} in {path}")
    elif tag == 1:
        if bits == 8:
            arr = (np.frombuffer(data_bytes, "u1").astype(np.float64) - 128.0) / 128.0
        elif bits == 16:
            arr = np.frombuffer(data_bytes, "<i2").astype(np.float64) / 32768.0
        elif bits == 24:
            b = np.frombuffer(data_bytes, "u1")
            usable = (len(b) // 3) * 3
            b = b[:usable].reshape(-1, 3).astype(np.uint32)
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            v = v.astype(np.int64)
            v = np.where(v & 0x800000, v - 0x1000000, v)
            arr = v.astype(np.float64) / 8388608.0
        elif bits == 32:
            arr = np.frombuffer(data_bytes, "<i4").astype(np.float64) / 2147483648.0
        else:
            raise ValueError(f"unsupported PCM width {bits} in {path}")
    else:
        raise ValueError(f"unsupported WAV format tag {tag} in {path}")

    frames = len(arr) // channels
    arr = arr[:frames * channels].reshape(frames, channels).T

    return {
        "path": path,
        "data": arr,
        "sr": sr,
        "channels": channels,
        "frames": frames,
        "bits": bits,
        "format_tag": tag,
        "data_bytes": data_bytes,
        "fmt_key": (tag, bits, channels, sr),
    }


def _as_mono(data, channel):
    """channel: 'L', 'R', 'mix' (or an integer index)."""
    if isinstance(channel, int):
        return data[min(channel, data.shape[0] - 1)].copy()
    key = str(channel).strip().upper()
    if key == "L":
        return data[0].copy()
    if key == "R":
        return data[min(1, data.shape[0] - 1)].copy()
    return data.mean(axis=0)


def _select(x, sr, start=None, dur=None, onset=None):
    """Slice x by onset-relative or absolute time; returns (segment, t0)."""
    n = len(x)
    if start is None:
        start = onset if onset is not None else 0.0
    i0 = max(0, min(n, int(round(float(start) * sr))))
    if dur is None:
        i1 = n
    else:
        i1 = max(i0, min(n, i0 + int(round(float(dur) * sr))))
    return x[i0:i1], i0 / float(sr)


# ----------------------------------------------------------------- levels ---

def amp_to_db(amp):
    a = float(amp)
    if a <= 0.0 or not math.isfinite(a):
        return FLOOR_DB
    return 20.0 * math.log10(a)


def level_to_db(level):
    return amp_to_db(float(level))


def rms(x):
    if len(x) == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(x.astype(np.float64)))))


def peak(x):
    if len(x) == 0:
        return 0.0
    return float(np.max(np.abs(x)))


def _rms_db(x):
    return amp_to_db(rms(x))


def _peak_db(x):
    return amp_to_db(peak(x))


# -------------------------------------------------------------- filtering ---

def _band_mask(freqs, lo, hi, taper_oct=0.15):
    """Tapered band mask, edges expressed in octaves around lo/hi."""
    half = taper_oct * 0.5
    lo_edge = lo * 2.0 ** (-half)
    hi_edge = hi * 2.0 ** (half)
    mask = np.zeros_like(freqs)

    inside = (freqs >= lo) & (freqs <= hi)
    mask[inside] = 1.0

    rise = (freqs >= lo_edge) & (freqs < lo)
    if np.any(rise):
        t = (np.log2(np.maximum(freqs[rise], 1e-9)) - math.log2(lo_edge)) / (2 * half)
        mask[rise] = 0.5 - 0.5 * np.cos(np.pi * np.clip(t, 0.0, 1.0))

    fall = (freqs > hi) & (freqs <= hi_edge)
    if np.any(fall):
        t = (math.log2(hi_edge) - np.log2(np.maximum(freqs[fall], 1e-9))) / (2 * half)
        mask[fall] = 0.5 - 0.5 * np.cos(np.pi * np.clip(t, 0.0, 1.0))

    return mask


def fft_band(x, sr, lo, hi, taper_oct=0.15):
    """Zero-phase FFT band filter (fast, no ringing tails of an IIR)."""
    n = len(x)
    if n == 0:
        return x.copy()
    nfft = 1 << int(math.ceil(math.log2(max(2, n))))
    X = np.fft.rfft(x, nfft)
    f = np.fft.rfftfreq(nfft, 1.0 / sr)
    return np.fft.irfft(X * _band_mask(f, lo, hi, taper_oct), nfft)[:n]


# ------------------------------------------------------------------ onset ---

def detect_onset(x, sr, threshold_db=-60.0, search=None):
    """First sample above threshold_db relative to the peak.

    Returns (onset_sample, onset_time_s).  The 1 s --warmup silence means the
    first threshold crossing is the impulse / signal start.
    """
    n = len(x)
    if n == 0:
        return 0, 0.0
    p = int(np.argmax(np.abs(x)))
    if search is not None:
        lo = max(0, int(search[0] * sr))
        hi = min(n, int(search[1] * sr))
    else:
        lo, hi = 0, n

    peak_abs = abs(float(x[p])) if n else 0.0
    if peak_abs <= 0.0:
        return p, p / float(sr)

    thr = peak_abs * 10.0 ** (threshold_db / 20.0)
    above = np.nonzero(np.abs(x[lo:hi]) >= thr)[0]
    if above.size == 0:
        return p, p / float(sr)
    onset = lo + int(above[0])
    return onset, onset / float(sr)


# -------------------------------------------------------------- schroeder ---

def _moving_rms_db(x, sr, win_ms=5.0, hop_ms=1.0):
    n = len(x)
    win = max(1, int(sr * win_ms / 1000.0))
    hop = max(1, int(sr * hop_ms / 1000.0))
    if n < win:
        return np.array([_rms_db(x)])
    cs = np.concatenate(([0.0], np.cumsum(np.square(x.astype(np.float64)))))
    idx = np.arange(0, n - win + 1, hop)
    e = (cs[idx + win] - cs[idx]) / win
    return 10.0 * np.log10(np.maximum(e, EPS))


def _fit_decay(t, level_db, lo_db, hi_db):
    """Least-squares RT from the Schroeder curve between lo_db and hi_db."""
    m = (level_db <= lo_db) & (level_db >= hi_db)
    if int(np.sum(m)) < 32:
        return None
    tt = t[m]
    ll = level_db[m]
    slope, intercept = np.polyfit(tt, ll, 1)
    if slope >= -1.0e-9:
        return None
    rt = -60.0 / float(slope)
    pred = slope * tt + intercept
    ss_res = float(np.sum((ll - pred) ** 2))
    ss_tot = float(np.sum((ll - np.mean(ll)) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    span = lo_db - hi_db
    if abs(slope) * span <= 0.0:
        return None
    return {"rt60_s": rt, "slope_db_per_s": float(slope), "r2": float(r2),
            "span_db": float(span), "points": int(np.sum(m))}


def decay_curve(x, sr, trunc_db=60.0):
    """Schroeder backwards-integrated decay curve for x (starting at onset).

    The IR is truncated at the last 1 ms envelope frame above -trunc_db dB
    relative to the envelope peak; `truncated` is True when the file ended
    before that point.
    """
    n = len(x)
    if n < 16:
        return None
    env = _moving_rms_db(x, sr)
    env_peak = float(np.max(env))
    above = np.nonzero(env >= env_peak - trunc_db)[0]
    truncated = True
    end = n
    if above.size:
        last = int(above[-1])
        hop = max(1, int(sr * 0.001))
        end = min(n, (last + 2) * hop + hop)
        truncated = last >= (len(env) - 2)

    seg = x[:end].astype(np.float64)
    energy = np.cumsum(np.square(seg)[::-1])[::-1]
    if energy[0] <= 0.0:
        return None
    level_db = 10.0 * np.log10(np.maximum(energy / energy[0], EPS))
    t = np.arange(end, dtype=np.float64) / sr
    return {"t": t, "level_db": level_db, "truncated": bool(truncated),
            "end_sample": int(end)}


def _rt60_from_curve(curve):
    t = curve["t"]
    L = curve["level_db"]
    out = {
        "edt": _fit_decay(t, L, 0.0, -10.0),
        "t20": _fit_decay(t, L, -5.0, -25.0),
        "t30": _fit_decay(t, L, -5.0, -35.0),
    }
    flat = {"truncated": curve["truncated"], "end_s": float(t[-1]) if len(t) else 0.0}
    for key in ("edt", "t20", "t30"):
        fit = out[key]
        flat[key + "_s"] = None if fit is None else fit["rt60_s"]
        flat[key + "_r2"] = None if fit is None else fit["r2"]
    return flat, out


def measure_rt60(path_or_data, sr=None, channel="both", bands=None,
                 onset_db=-60.0, trunc_db=60.0):
    """Broadband + per-band Schroeder RT60.

    Returns {"broadband": {"mean": {...}, "L": {...}, "R": {...}},
             "bands": {"250": {"mean": {...}, ...}, ...}}
    """
    if isinstance(path_or_data, str):
        w = read_wav(path_or_data)
        data, sr = w["data"], w["sr"]
        path = path_or_data
    else:
        data, path = path_or_data, None

    onset_sample, onset_s = detect_onset(_as_mono(data, "mix"), sr, onset_db)
    mono = _as_mono(data, "mix")[onset_sample:]
    curve = decay_curve(mono, sr, trunc_db)

    channels = {}
    keys = ["L", "R"] if data.shape[0] > 1 else ["L"]
    if str(channel).upper() in ("L", "R"):
        keys = [str(channel).upper()]

    for key in keys:
        seg = _as_mono(data, key)[onset_sample:]
        cur = decay_curve(seg, sr, trunc_db)
        if cur is None:
            channels[key] = {"truncated": True, "edt_s": None, "t20_s": None, "t30_s": None}
        else:
            flat, _detail = _rt60_from_curve(cur)
            channels[key] = flat

    def mean_flat(per_ch):
        out = {"truncated": any(v.get("truncated", False) for v in per_ch.values())}
        for metric in ("edt_s", "t20_s", "t30_s"):
            vals = [v[metric] for v in per_ch.values() if v.get(metric) is not None]
            out[metric] = float(np.mean(vals)) if vals else None
        for metric in ("edt_r2", "t20_r2", "t30_r2"):
            vals = [v[metric] for v in per_ch.values() if v.get(metric) is not None]
            out[metric] = float(np.mean(vals)) if vals else None
        return out

    result = {"broadband": dict(mean=mean_flat(channels), **channels),
              "onset_sample": int(onset_sample), "onset_s": float(onset_s),
              "duration_s": float(len(mono) / sr) if sr else 0.0}

    if bands:
        band_results = {}
        for centre in bands:
            lo, hi = centre * 2.0 ** (-1.0 / 6.0), centre * 2.0 ** (1.0 / 6.0)
            band_chans = {}
            for key in keys:
                seg = fft_band(_as_mono(data, key)[onset_sample:], sr, lo, hi)
                cur = decay_curve(seg, sr, trunc_db)
                if cur is None:
                    band_chans[key] = {"truncated": True, "t20_s": None, "t30_s": None, "edt_s": None}
                    continue
                flat, _ = _rt60_from_curve(cur)
                band_chans[key] = flat
            band_results[str(centre)] = mean_flat(band_chans)
        result["bands"] = band_results

    if path:
        result["file"] = path
        result["sr"] = sr
    return result


def rt60_summary(result):
    """Flatten measure_rt60() into the few numbers a report shows."""
    broadband = result["broadband"]
    mean = broadband["mean"]
    return {"rt60_t30_s": mean.get("t30_s"), "rt60_t20_s": mean.get("t20_s"),
            "edt_s": mean.get("edt_s"), "truncated": mean.get("truncated")}


# ---------------------------------------------------------------- spectrum ---

def _hann(n):
    if n < 2:
        return np.ones(max(1, n))
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / (n - 1))


def _parabolic_peak(mag, idx):
    """Parabolic interpolation of |X| around bin idx -> (fractional bin, amp)."""
    if idx <= 0 or idx >= len(mag) - 1:
        return float(idx), float(mag[idx])
    a, b, c = abs(mag[idx - 1]), abs(mag[idx]), abs(mag[idx + 1])
    la, lb, lc = math.log(a + 1e-300), math.log(b + 1e-300), math.log(c + 1e-300)
    denom = la - 2.0 * lb + lc
    delta = 0.5 * (la - lc) / denom if abs(denom) > 1e-12 else 0.0
    delta = float(np.clip(delta, -0.5, 0.5))
    amp = math.exp(lb - 0.25 * (la - lc) * delta) if abs(denom) > 1e-12 else b
    return float(idx) + delta, float(amp)


def _ls_sinusoid_fit(x, t, freq):
    """Least-squares fit of x ~ a*cos(wt) + b*sin(wt) at freq.

    Returns (amplitude, a, b); the residual is x - a*cos - b*sin.
    """
    w = 2.0 * np.pi * freq * t
    c = np.cos(w)
    s = np.sin(w)
    # 2x2 normal equations, no big matrices.
    cc = float(np.dot(c, c))
    ss = float(np.dot(s, s))
    cs = float(np.dot(c, s))
    xc = float(np.dot(x, c))
    xs = float(np.dot(x, s))
    det = cc * ss - cs * cs
    if abs(det) <= 1e-12:
        return 0.0, 0.0, 0.0
    a = (xc * ss - xs * cs) / det
    b = (xs * cc - xc * cs) / det
    return float(math.hypot(a, b)), float(a), float(b)


def measure_thd(path_or_data, sr=None, channel="L", start=1.5, dur=1.0,
                freq=None, max_harmonics=20, hp=20.0, spur_channel=None,
                spur_guard_bins=5.0):
    """THD / THD+N / largest non-harmonic spur of a steady sine render.

    Fundamental and harmonics are measured with a least-squares sinusoid fit at
    exact multiples of f0, which avoids window-leakage bias.  The residual after
    removing them is windowed to find the largest non-harmonic spur.

    ``spur_guard_bins`` blanks +-this many *analysis* bins (``sr / n``, i.e. the
    DFT resolution of the fitted segment) around every harmonic k*f0 below
    Nyquist, including harmonics beyond ``max_harmonics``.  A "spur" is
    therefore non-harmonic by construction: it can never be leftover harmonic
    energy.
    """
    if isinstance(path_or_data, str):
        w = read_wav(path_or_data)
        data, sr = w["data"], w["sr"]
        path = path_or_data
    else:
        data, path = path_or_data, None

    x = _as_mono(data, spur_channel if spur_channel is not None else channel)
    x, t0 = _select(x, sr, start=start, dur=dur)
    n = len(x)
    if n < 256:
        raise ValueError("thd: analysis segment too short")
    x = x - float(np.mean(x))
    t = np.arange(n, dtype=np.float64) / sr

    # --- fundamental frequency -------------------------------------------
    if freq is not None:
        f0 = float(freq)
    else:
        w = _hann(n)
        nfft = 1 << int(math.ceil(math.log2(max(4096, n * 16))))
        X = np.fft.rfft(x * w, nfft)
        freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
        keep = freqs >= max(hp, 10.0)
        mag = np.abs(X)
        cand = np.where(keep, mag, 0.0)
        k = int(np.argmax(cand))
        frac, _amp = _parabolic_peak(mag, k)
        f0 = frac * (sr / nfft)

    if not (0.0 < f0 < sr * 0.5):
        raise ValueError(f"thd: fundamental {f0} Hz out of range")

    # --- harmonics -------------------------------------------------------
    nyquist = sr * 0.5
    kmax = int(math.floor((nyquist * 0.999) / f0))
    k_max_use = min(kmax, int(max_harmonics))
    harmonics = []
    residual = x.copy()
    for k in range(1, k_max_use + 1):
        fk = f0 * k
        if fk >= nyquist:
            break
        amp_k, a_k, b_k = _ls_sinusoid_fit(x, t, fk)
        harmonics.append({"k": k, "freq_hz": fk, "amp": amp_k,
                          "dbfs": amp_to_db(amp_k)})
        # remove this component (DC already removed)
        residual -= (a_k * np.cos(2.0 * np.pi * fk * t)
                     + b_k * np.sin(2.0 * np.pi * fk * t))

    if not harmonics:
        raise ValueError("thd: no harmonics fitted")
    base = harmonics[0]["amp"]
    if base <= 0.0:
        raise ValueError("thd: zero fundamental amplitude")

    harm_energy = sum(h["amp"] ** 2 for h in harmonics[1:])
    thd = math.sqrt(harm_energy) / base
    thd_db = 20.0 * math.log10(max(thd, 1e-30))

    # --- THD+N: residual RMS relative to the fundamental RMS ------------
    # Band-limit to [hp, 0.98*Nyquist] first: sub-audio drift/DC is not
    # distortion and would otherwise dominate THD+N.
    resid_band = fft_band(residual, sr, hp, sr * 0.49)
    resid_rms = rms(resid_band)
    fund_rms = base / math.sqrt(2.0)
    thdn = resid_rms / fund_rms if fund_rms > 0 else float("inf")
    thdn_db = 20.0 * math.log10(max(thdn, 1e-30))

    # --- largest non-harmonic spur in the residual ----------------------
    w = _hann(n)
    nfft = 1 << int(math.ceil(math.log2(max(4096, n * 4))))
    R = np.fft.rfft(residual * w, nfft)
    rf = np.fft.rfftfreq(nfft, 1.0 / sr)
    mag = np.abs(R)
    coherent = float(np.sum(w))
    amp_scale = 2.0 / coherent if coherent > 0 else 0.0
    mask = (rf >= max(hp, 20.0)) & (rf <= nyquist * 0.98)
    # Blank +-spur_guard_bins analysis bins around *every* harmonic below
    # Nyquist (not only the harmonics fitted above), so a reported spur is
    # non-harmonic by construction.
    guard_hz = max(2.0, float(spur_guard_bins) * sr / n)
    k_blank = int(math.floor(nyquist * 0.98 / f0))
    for k in range(1, k_blank + 1):
        mask &= np.abs(rf - f0 * k) > guard_hz
    cand = np.where(mask, mag, -1.0)
    spur_idx = int(np.argmax(cand))
    spur_frac, spur_mag = _parabolic_peak(mag, spur_idx)
    spur_freq = spur_frac * (sr / nfft)
    spur_amp = spur_mag * amp_scale

    # noise floor: median residual spectral amplitude
    noise_amp = float(np.median(cand[mask])) * amp_scale

    result = {
        "channel": str(spur_channel if spur_channel is not None else channel),
        "start_s": float(t0), "dur_s": float(n / sr),
        "f0_hz": float(f0),
        "fundamental_dbfs": amp_to_db(base),
        "thd_pct": float(thd * 100.0),
        "thd_db": float(thd_db),
        "thdn_pct": float(thdn * 100.0),
        "thdn_db": float(thdn_db),
        "spur_dbfs": float(amp_to_db(spur_amp)),
        "spur_hz": float(spur_freq),
        "noise_floor_dbfs": float(amp_to_db(noise_amp)),
        "hp_hz": float(hp),
        "spur_guard_hz": float(guard_hz),
        "spur_guard_bins": float(spur_guard_bins),
        "harmonics": harmonics,
    }
    if path:
        result["file"] = path
        result["sr"] = sr
    return result


def measure_centroid(path_or_data, sr=None, channel="mix", start=None, dur=None,
                     onset_db=-60.0, frame=2048, hop_s=0.1, hf_hz=8000.0,
                     min_db=60.0):
    """Spectral centroid, 100 ms trajectory and HF energy share.

    Trajectory frames more than min_db below the loudest frame are marked
    invalid (they are numerical tail noise) and excluded from the percentile
    summary.
    """
    if isinstance(path_or_data, str):
        w = read_wav(path_or_data)
        data, sr = w["data"], w["sr"]
        path = path_or_data
    else:
        data, path = path_or_data, None

    xfull = _as_mono(data, channel)
    onset_sample, onset_s = detect_onset(_as_mono(data, "mix"), sr, onset_db)
    x, t0 = _select(xfull, sr, start=start, dur=dur, onset=onset_s)
    if len(x) < 16:
        raise ValueError("centroid: segment too short")
    x = x - float(np.mean(x))

    w = _hann(len(x))
    X = np.fft.rfft(x * w)
    freqs = np.fft.rfftfreq(len(x), 1.0 / sr)
    power = np.abs(X) ** 2
    total = float(np.sum(power))
    centroid = float(np.sum(freqs * power) / total) if total > 0 else 0.0
    hf = float(np.sum(power[freqs >= hf_hz]) / total) if total > 0 else 0.0

    # --- trajectory ------------------------------------------------------
    hop = max(1, int(round(hop_s * sr)))
    frame = int(frame)
    if frame > len(x):
        frame = len(x)
    win = _hann(frame)
    nfft = 1 << int(math.ceil(math.log2(max(1024, frame * 2))))
    bins = np.fft.rfftfreq(nfft, 1.0 / sr)
    traj = []
    starts = list(range(0, max(1, len(x) - frame + 1), hop))
    for i0 in starts:
        seg = x[i0:i0 + frame]
        if len(seg) < frame:
            seg = np.pad(seg, (0, frame - len(seg)))
        P = np.abs(np.fft.rfft(seg * win, nfft)) ** 2
        tot = float(np.sum(P))
        c = float(np.sum(bins * P) / tot) if tot > 0 else 0.0
        h = float(np.sum(P[bins >= hf_hz]) / tot) if tot > 0 else 0.0
        traj.append({"t_s": float(i0 / sr), "t_abs_s": float(t0 + i0 / sr),
                     "centroid_hz": c, "hf_ratio": h,
                     "rms_dbfs": _rms_db(seg)})

    if traj:
        frame_db = np.array([p["rms_dbfs"] for p in traj])
        valid = frame_db >= (float(np.max(frame_db)) - min_db)
        for p, ok in zip(traj, valid):
            p["valid"] = bool(ok)
        cents = np.array([p["centroid_hz"] for p, ok in zip(traj, valid) if ok])
        if cents.size == 0:
            cents = np.array([p["centroid_hz"] for p in traj])
    else:
        valid = np.zeros(0, dtype=bool)
        cents = np.array([centroid])

    result = {
        "channel": str(channel),
        "start_s": float(t0), "dur_s": float(len(x) / sr),
        "centroid_hz": centroid,
        "hf_hz": float(hf_hz), "hf_ratio": hf,
        "trajectory": traj,
        "frames_total": len(traj),
        "frames_valid": int(np.sum(valid)) if len(valid) else 0,
        "min_db": float(min_db),
        "centroid_median_hz": float(np.median(cents)),
        "centroid_p10_hz": float(np.percentile(cents, 10)),
        "centroid_p90_hz": float(np.percentile(cents, 90)),
        "trajectory_hz": float(1.0 / hop_s),
    }
    if path:
        result["file"] = path
        result["sr"] = sr
    return result


# ----------------------------------------------------------------- events ---

def detect_events(path_or_data, sr=None, channel="mix", band=None,
                  env_ms=2.0, floor_db=6.0, refractory_s=0.5,
                  median_ms=500.0, margin_db=6.0, rise_ms=100.0, rise_db=16.0,
                  onset_db=-60.0):
    """Impact / event detection.

    Method: envelope = env_ms RMS at a 1 ms hop (optionally on a band-passed
    copy, `band=(lo, hi)`).  An event is a local maximum of that envelope that
    (a) lies within floor_db of the loudest envelope frame, (b) exceeds the
    local median by margin_db, (c) has risen at least rise_db over the previous
    rise_ms (i.e. it comes after a quieter stretch), and (d) is at least
    refractory_s after the previous event.  Defaults (16 dB / 100 ms) were
    tuned so that stationary noise (SPRING noise test) yields 0 events while a
    single physical hit still yields exactly 1 for all six algorithms.  One physical hit -> one event for
    every algorithm; a stationary tone or noise yields none; three hits spaced
    2 s yield three.

    The event time is the envelope-peak time; `peak_dbfs` is the absolute
    sample peak of the broadband signal within +-10 ms of it.  For the
    dispersion micro-structure of one spring hit pass a band (e.g.
    `band=(4000, 8000)`) and a lower refractory.
    """
    if isinstance(path_or_data, str):
        w = read_wav(path_or_data)
        data, sr = w["data"], w["sr"]
        path = path_or_data
    else:
        data, path = path_or_data, None

    x = _as_mono(data, channel)
    y = x if band is None else fft_band(x, sr, float(band[0]), float(band[1]))
    n = len(y)
    onset_sample, onset_s = detect_onset(_as_mono(data, "mix"), sr, onset_db)

    win = max(1, int(round(sr * env_ms / 1000.0)))
    hop = max(1, int(round(sr * 0.001)))
    if n < win:
        env_db = np.array([_rms_db(y)])
    else:
        cs = np.concatenate(([0.0], np.cumsum(np.square(y.astype(np.float64)))))
        idx = np.arange(0, n - win + 1, hop)
        e = (cs[idx + win] - cs[idx]) / win
        env_db = 10.0 * np.log10(np.maximum(e, EPS))

    nframes = len(env_db)
    if nframes == 0:
        env_db = np.array([_rms_db(y)])
        nframes = 1

    peak_db = float(np.max(env_db))
    if peak_db <= -200.0 or not np.any(np.isfinite(env_db)):
        # digital silence (e.g. freeze render): no events to report
        cand = []
        gate_db = peak_db
    else:
        gate_db = peak_db - float(floor_db)

    # Local baseline: median of the envelope over +-median_ms/2.  A hit must
    # stand margin_db above it, which rejects stationary tones and noise.
    half = max(1, int(round(median_ms / 2.0)))       # 1 ms frames
    padded = np.pad(env_db, (half, half), mode="edge")
    baseline = np.median(np.lib.stride_tricks.sliding_window_view(padded, 2 * half + 1),
                         axis=1)

    # lowest envelope level in the previous rise_ms (does the max rise out of
    # a quieter stretch, or is it just a ripple on a steady signal?)
    k = max(1, int(round(rise_ms)))
    padded_hist = np.pad(env_db, (k, 0), mode="edge")
    pre_min = np.min(np.lib.stride_tricks.sliding_window_view(padded_hist, k + 1)[:nframes],
                     axis=1)

    # local maxima of the envelope that clear all gates
    if nframes >= 3:
        prev, cur, nxt = env_db[:-2], env_db[1:-1], env_db[2:]
        loc = np.nonzero((cur >= prev) & (cur > nxt) & (cur >= gate_db)
                         & (cur >= baseline[1:-1] + margin_db)
                         & (cur - pre_min[1:-1] >= rise_db))[0] + 1
        cand = list(loc)
        if (env_db[0] >= gate_db and env_db[0] >= baseline[0] + margin_db
                and env_db[0] - pre_min[0] >= rise_db):
            cand.insert(0, 0)
        if (env_db[-1] > env_db[-2] and env_db[-1] >= gate_db
                and env_db[-1] >= baseline[-1] + margin_db
                and env_db[-1] - pre_min[-1] >= rise_db):
            cand.append(nframes - 1)
    else:
        cand = [0] if env_db[0] >= gate_db else []

    refractory_frames = max(1, int(round(refractory_s * 1000.0)))
    events = []
    last_frame = -10 ** 9
    for i in cand:
        if i - last_frame < refractory_frames:
            # same hit: keep the stronger peak of the pair
            if events and env_db[i] > events[-1]["env_db"]:
                events.pop()
            else:
                continue
        t = i / 1000.0                      # 1 ms hop
        p0 = max(0, int(round((t - 0.01) * sr)))
        p1 = min(n, int(round((t + 0.01) * sr)))
        seg = x[p0:p1]
        events.append({"t_s": float(t), "t_abs_s": float(t),
                       "onset_frame": int(i),
                       "env_db": float(env_db[i]),
                       "peak_dbfs": _peak_db(seg) if len(seg) else FLOOR_DB})
        last_frame = i

    duration = float(n / sr) if sr else 0.0
    gaps = [events[i + 1]["t_s"] - events[i]["t_s"] for i in range(len(events) - 1)]
    band_hz = [0.0, sr / 2.0] if band is None else [float(band[0]), float(band[1])]
    result = {
        "channel": str(channel),
        "band_hz": band_hz,
        "onset_s": float(onset_s),
        "duration_s": duration,
        "event_count": len(events),
        "events_per_sec": (len(events) / duration) if duration > 0 else 0.0,
        "mean_interval_s": float(np.mean(gaps)) if gaps else None,
        "median_interval_s": float(np.median(gaps)) if gaps else None,
        "min_interval_s": float(np.min(gaps)) if gaps else None,
        "max_peak_dbfs": max((e["peak_dbfs"] for e in events), default=FLOOR_DB),
        "events": events,
        "method": {"band_hz": band_hz, "env_ms": float(env_ms),
                   "floor_db": float(floor_db),
                   "refractory_s": float(refractory_s),
                   "median_ms": float(median_ms),
                   "margin_db": float(margin_db),
                   "rise_ms": float(rise_ms), "rise_db": float(rise_db),
                   "peak_env_db": peak_db},
    }
    if path:
        result["file"] = path
        result["sr"] = sr
    return result


# ---------------------------------------------------------------- compare ---

def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def compare_files(path_a, path_b):
    """Sample-aligned comparison; reference is path_a.

    Returns dict including max_abs_diff, rel_peak_error, first_diff_sample,
    bitwise_identical (identical sample bits and identical format).
    """
    a = read_wav(path_a)
    b = read_wav(path_b)
    res = {
        "a": path_a, "b": path_b,
        "sr_a": a["sr"], "sr_b": b["sr"],
        "channels_a": a["channels"], "channels_b": b["channels"],
        "frames_a": a["frames"], "frames_b": b["frames"],
        "bits_a": a["bits"], "bits_b": b["bits"],
        "sha256_a": _sha256_file(path_a), "sha256_b": _sha256_file(path_b),
        "length_mismatch": a["frames"] != b["frames"] or a["channels"] != b["channels"],
    }
    res["file_sha256_equal"] = res["sha256_a"] == res["sha256_b"]
    same_fmt = a["fmt_key"] == b["fmt_key"]
    res["same_format"] = bool(same_fmt)

    n = min(a["frames"], b["frames"])
    if n > 0 and a["channels"] == b["channels"]:
        da = a["data"][:, :n]
        db = b["data"][:, :n]
        diff = np.abs(da - db)
        res["max_abs_diff"] = float(np.max(diff))
        res["max_abs_diff_dbfs"] = amp_to_db(float(np.max(diff)))
        pa = float(np.max(np.abs(da))) if da.size else 0.0
        res["ref_peak"] = pa
        res["rel_peak_error"] = float(res["max_abs_diff"] / pa) if pa > 0 else (
            0.0 if res["max_abs_diff"] == 0 else float("inf"))
        res["n_diff_samples"] = int(np.sum(diff > 0.0))
        flat_a = np.ravel(da, order="F")
        flat_b = np.ravel(db, order="F")
        nz = np.nonzero(diff.ravel(order="F") > 0.0)[0]
        first = int(nz[0]) if nz.size else -1
        res["first_diff_sample"] = first
        res["first_diff_channel"] = int(first % a["channels"]) if first >= 0 else -1
        res["first_diff_time_s"] = (first // a["channels"]) / float(a["sr"]) if first >= 0 else -1.0
        res["rms_a_dbfs"] = _rms_db(flat_a)
        res["rms_b_dbfs"] = _rms_db(flat_b)
        if nz.size:
            res["rms_diff_dbfs"] = _rms_db(flat_a - flat_b)
        else:
            res["rms_diff_dbfs"] = FLOOR_DB
    else:
        res["max_abs_diff"] = float("inf")
        res["max_abs_diff_dbfs"] = FLOOR_DB
        res["rel_peak_error"] = float("inf")
        res["n_diff_samples"] = -1
        res["first_diff_sample"] = -1
        res["first_diff_channel"] = -1
        res["first_diff_time_s"] = -1.0
        res["rms_a_dbfs"] = _rms_db(a["data"])
        res["rms_b_dbfs"] = _rms_db(b["data"])
        res["rms_diff_dbfs"] = FLOOR_DB

    # Bit-exactness: identical fmt + identical raw data bytes.
    res["bitwise_identical"] = bool(same_fmt and a["data_bytes"] == b["data_bytes"])
    if not res["bitwise_identical"] and same_fmt and a["bits"] == 32 and a["format_tag"] == 3:
        # Distinguish -0.0 vs 0.0 and NaN payload differences.
        ua = np.frombuffer(a["data_bytes"], "<u4", count=n * a["channels"])
        ub = np.frombuffer(b["data_bytes"], "<u4", count=n * b["channels"])
        differing = np.nonzero(ua != ub)[0]
        if differing.size:
            res["first_bit_diff_sample"] = int(differing[0] // a["channels"])
            res["n_bit_diff_samples"] = int(differing.size)
        else:
            res["first_bit_diff_sample"] = -1
            res["n_bit_diff_samples"] = 0
    return res


# ---------------------------------------------------------------- reports ---

def _json(obj):
    print(json.dumps(obj, indent=2, ensure_ascii=False, default=float))


def _fmt(value, digits=2, suffix=""):
    if value is None:
        return "-"
    if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
        return "-"
    return f"{value:.{digits}f}{suffix}"


def print_info(res):
    print(f"file      : {res['file']}")
    print(f"sr        : {res['sr']} Hz")
    print(f"frames    : {res['frames']}  ({res['duration_s']:.3f} s)")
    print(f"channels  : {res['channels']}   bits: {res['bits']}   format: {res['format_tag']}")
    print(f"peak      : {res['peak_dbfs']:.3f} dBFS")
    print(f"rms       : {res['rms_dbfs']:.3f} dBFS")
    print(f"crest     : {res['crest_db']:.3f} dB")
    print(f"dc        : {', '.join(f'{v:+.3e}' for v in res['dc'])}")
    nf = res.get("nonfinite_samples", 0)
    print(f"nonfinite : {nf} samples" + ("" if nf else "   (all finite)"))
    for i, ch in enumerate(res["per_channel"]):
        name = "L" if i == 0 else ("R" if i == 1 else f"ch{i}")
        print(f"  {name}: peak {ch['peak_dbfs']:.3f} dBFS  rms {ch['rms_dbfs']:.3f} dBFS  "
              f"dc {ch['dc']:+.3e}")


# -------------------------------------------------------------------- CLI ---

def cmd_info(args):
    w = read_wav(args.wav)
    data = w["data"]
    onset_sample, onset_s = detect_onset(_as_mono(data, "mix"), w["sr"])
    x = data[:, onset_sample:]
    per_channel = []
    for ch in range(data.shape[0]):
        seg = x[ch]
        per_channel.append({"peak_dbfs": _peak_db(seg), "rms_dbfs": _rms_db(seg),
                            "dc": float(np.mean(seg)) if len(seg) else 0.0})
    res = {
        "file": args.wav, "sr": w["sr"], "frames": w["frames"],
        "duration_s": w["frames"] / w["sr"], "channels": w["channels"],
        "bits": w["bits"], "format_tag": w["format_tag"],
        "onset_s": onset_s,
        "peak_dbfs": _peak_db(x), "rms_dbfs": _rms_db(x),
        "crest_db": _peak_db(x) - _rms_db(x),
        "dc": [float(v) for v in np.mean(x, axis=1)] if len(x) else [0.0] * w["channels"],
        "per_channel": per_channel,
        "nonfinite_samples": int(data.size - int(np.count_nonzero(np.isfinite(data)))),
        "all_finite": bool(np.all(np.isfinite(data))),
    }
    if args.json:
        _json(res)
    else:
        print_info(res)


def cmd_thd(args):
    res = measure_thd(args.wav, channel=args.channel, start=args.start,
                      dur=args.dur, freq=args.freq,
                      max_harmonics=args.max_harmonics, hp=args.hp,
                      spur_guard_bins=args.spur_guard_bins)
    if args.json:
        _json(res)
        return
    print(f"file        : {res['file']}")
    print(f"segment     : {res['start_s']:.3f} .. {res['start_s'] + res['dur_s']:.3f} s  (ch {res['channel']})")
    print(f"fundamental : {res['f0_hz']:.3f} Hz  {res['fundamental_dbfs']:.2f} dBFS")
    print(f"THD         : {res['thd_pct']:.6f} %   ({res['thd_db']:.1f} dB)")
    print(f"THD+N       : {res['thdn_pct']:.6f} %   ({res['thdn_db']:.1f} dB)")
    print(f"largest spur: {res['spur_dbfs']:.1f} dBFS @ {res['spur_hz']:.1f} Hz"
          f"  (harmonics +-{res['spur_guard_hz']:.1f} Hz / {res['spur_guard_bins']:g} bins blanked)")
    print(f"noise floor : {res['noise_floor_dbfs']:.1f} dBFS (median residual bin)")
    print("harmonics   :")
    for h in res["harmonics"]:
        print(f"  h{h['k']:<2d} {h['freq_hz']:9.2f} Hz  {h['dbfs']:9.2f} dBFS")


def cmd_rt60(args):
    bands = None
    if not args.no_bands:
        bands = THIRD_OCTAVE
    res = measure_rt60(args.wav, channel=args.channel, bands=bands,
                       onset_db=args.onset_db, trunc_db=args.trunc_db)
    if args.json:
        _json(res)
        return
    print(f"file       : {res['file']}")
    print(f"onset      : sample {res['onset_sample']} ({res['onset_s']:.4f} s)  "
          f"IR span {res['duration_s']:.2f} s")
    bb = res["broadband"]
    for key in (["L", "R"] if "R" in bb else ["L"]):
        ch = bb[key]
        print(f"  {key}: EDT {_fmt(ch['edt_s'], 3)} s   T20 {_fmt(ch['t20_s'], 3)} s   "
              f"T30 {_fmt(ch['t30_s'], 3)} s   truncated={ch['truncated']}")
    m = bb["mean"]
    print(f"broadband  : EDT {_fmt(m['edt_s'], 3)} s   T20 {_fmt(m['t20_s'], 3)} s   "
          f"T30 {_fmt(m['t30_s'], 3)} s")
    if "bands" in res:
        print("bands      : centre  T20(s)  T30(s)")
        for centre, b in res["bands"].items():
            print(f"  {centre:>5s} Hz  {_fmt(b['t20_s'], 3):>7s} {_fmt(b['t30_s'], 3):>8s}")


def cmd_centroid(args):
    res = measure_centroid(args.wav, channel=args.channel, start=args.start,
                           dur=args.dur, onset_db=args.onset_db,
                           frame=args.frame, hop_s=args.hop, hf_hz=args.hf,
                           min_db=args.min_db)
    if args.json:
        _json(res)
        return
    print(f"file      : {res['file']}")
    print(f"segment   : {res['start_s']:.3f} .. {res['start_s'] + res['dur_s']:.3f} s  (ch {res['channel']})")
    print(f"centroid  : {res['centroid_hz']:.1f} Hz   median {res['centroid_median_hz']:.1f} Hz "
          f"(p10 {res['centroid_p10_hz']:.1f} / p90 {res['centroid_p90_hz']:.1f})")
    print(f"HF share  : {100.0 * res['hf_ratio']:.2f} % above {res['hf_hz']:.0f} Hz")
    print(f"trajectory: {res['frames_valid']}/{res['frames_total']} frames valid "
          f"(< -{res['min_db']:.0f} dB rel. loudest frame dropped)")
    for p in res["trajectory"][: args.max_traj]:
        flag = "" if p.get("valid", True) else "  [weak]"
        print(f"  t={p['t_abs_s']:7.3f} s  centroid {p['centroid_hz']:8.1f} Hz  "
              f"hf {100.0 * p['hf_ratio']:6.2f} %  rms {p['rms_dbfs']:7.1f} dBFS{flag}")
    if len(res["trajectory"]) > args.max_traj:
        print(f"  ... {len(res['trajectory']) - args.max_traj} more frames")


def cmd_events(args):
    band = (args.band_lo, args.band_hi) if args.band_lo and args.band_lo > 0 else None
    res = detect_events(args.wav, channel=args.channel, band=band,
                        env_ms=args.env_ms, floor_db=args.floor_db,
                        refractory_s=args.refractory, median_ms=args.median_ms,
                        margin_db=args.margin_db, rise_ms=args.rise_ms,
                        rise_db=args.rise_db)
    if args.json:
        _json(res)
        return
    print(f"file      : {res['file']}")
    lo, hi = res["band_hz"]
    band_txt = "full band" if lo <= 0 else f"{lo:.0f}-{hi:.0f} Hz"
    print(f"band      : {band_txt}   duration {res['duration_s']:.3f} s")
    m = res["method"]
    print(f"detector  : env {m['env_ms']:.0f} ms maxima, gate peak-{m['floor_db']:.0f} dB, "
          f"median+{m['margin_db']:.0f} dB, rise +{m['rise_db']:.0f} dB / "
          f"{m['rise_ms']:.0f} ms, refractory {m['refractory_s'] * 1000:.0f} ms "
          f"(peak env {m['peak_env_db']:.1f} dBFS)")
    print(f"events    : {res['event_count']}   rate {res['events_per_sec']:.3f} /s")
    print(f"intervals : mean {_fmt(res['mean_interval_s'], 3)} s  median "
          f"{_fmt(res['median_interval_s'], 3)} s  min {_fmt(res['min_interval_s'], 3)} s")
    print(f"peak      : {res['max_peak_dbfs']:.2f} dBFS (loudest event)")
    for e in res["events"][: args.max_events]:
        print(f"  t={e['t_s']:7.3f} s  peak {e['peak_dbfs']:7.2f} dBFS  "
              f"env {e['env_db']:7.1f} dBFS")
    if len(res["events"]) > args.max_events:
        print(f"  ... {len(res['events']) - args.max_events} more events")


def cmd_compare(args):
    res = compare_files(args.a, args.b)
    if args.json:
        _json(res)
    else:
        print(f"a          : {res['a']}")
        print(f"b          : {res['b']}")
        print(f"bit-exact  : {res['bitwise_identical']}")
        print(f"sha256 same: {res['file_sha256_equal']}")
        print(f"format     : {res['bits_a']}-bit / {res['bits_b']}-bit  "
              f"same={res['same_format']}")
        print(f"shape      : {res['frames_a']} x {res['channels_a']}  vs  "
              f"{res['frames_b']} x {res['channels_b']}  mismatch={res['length_mismatch']}")
        print(f"max|diff|  : {res['max_abs_diff']:.6e}   ({res['max_abs_diff_dbfs']:.1f} dBFS)")
        print(f"rel peak   : {res['rel_peak_error']:.6e}")
        print(f"diff specs : {res['n_diff_samples']}")
        print(f"first diff : sample {res['first_diff_sample']} "
              f"(ch {res['first_diff_channel']}) at {res['first_diff_time_s']:.6f} s")
    if args.fail_on_diff and not res["bitwise_identical"]:
        sys.exit(1)


# ------------------------------------------------------------------ sweep ---

TEST_SECONDS = {"impulse": 6.0, "sine": 20.0, "noise": 4.0, "sweep": 6.0}

CSV_COLUMNS = [
    "test", "index", "level", "level_dbfs", "sr", "sec", "file",
    "peak_dbfs", "rms_dbfs", "crest_db", "peak_gain_db",
    "rt60_t30_s", "rt60_t20_s", "edt_s", "rt60_truncated", "rt60_t30_r2",
    "f0_hz", "thd_pct", "thd_db", "thdn_pct", "thdn_db", "spur_dbfs", "spur_hz",
    "centroid_hz", "hf_ratio", "event_count", "events_per_sec",
    "mean_event_interval_s", "error",
]


def _run_render(analyzer, plugin, out_path, extra_args, quiet=True):
    cmd = [analyzer, "render", plugin, "--out", out_path] + list(extra_args)
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    summary = None
    if proc.returncode == 0:
        for line in reversed(proc.stdout.strip().splitlines()):
            line = line.strip()
            if line.startswith("{"):
                try:
                    summary = json.loads(line)
                except json.JSONDecodeError:
                    summary = None
                break
    if proc.returncode != 0 and not quiet:
        sys.stderr.write(proc.stderr)
    return proc.returncode, summary, proc.stderr.strip()


def _level_tag(i, level):
    txt = float(level)
    return f"{i:02d}_{txt:g}"


def _measure_one(test, wav, level, args):
    row = {"test": test, "peak_dbfs": None, "rms_dbfs": None,
           "crest_db": None, "peak_gain_db": None,
           "rt60_t30_s": None, "rt60_t20_s": None, "edt_s": None,
           "rt60_truncated": None, "rt60_t30_r2": None,
           "f0_hz": None, "thd_pct": None, "thd_db": None,
           "thdn_pct": None, "thdn_db": None,
           "spur_dbfs": None, "spur_hz": None,
           "centroid_hz": None, "hf_ratio": None,
           "event_count": None, "events_per_sec": None,
           "mean_event_interval_s": None, "error": ""}

    w = read_wav(wav)
    data, sr = w["data"], w["sr"]
    mono = _as_mono(data, "mix")
    onset_sample, onset_s = detect_onset(mono, sr)
    active = mono[onset_sample:]

    row["sr"] = sr
    row["peak_dbfs"] = _peak_db(active)
    row["rms_dbfs"] = _rms_db(active)
    row["crest_db"] = row["peak_dbfs"] - row["rms_dbfs"]
    row["peak_gain_db"] = row["peak_dbfs"] - level_to_db(level) if level > 0 else None

    try:
        if test == "impulse":
            rt = measure_rt60(data, sr, bands=None)
            s = rt60_summary(rt)
            row["rt60_t30_s"] = s["rt60_t30_s"]
            row["rt60_t20_s"] = s["rt60_t20_s"]
            row["edt_s"] = s["edt_s"]
            row["rt60_truncated"] = s["truncated"]
            row["rt60_t30_r2"] = rt["broadband"]["mean"].get("t30_r2")
            c = measure_centroid(data, sr)
            row["centroid_hz"] = c["centroid_hz"]
            row["hf_ratio"] = c["hf_ratio"]
            ev = detect_events(data, sr)
            row["event_count"] = ev["event_count"]
            row["events_per_sec"] = ev["events_per_sec"]
        elif test == "sine":
            t = measure_thd(data, sr, start=args.sine_start, dur=args.sine_dur,
                            freq=args.sine_hz or None,
                            max_harmonics=args.max_harmonics)
            row["f0_hz"] = t["f0_hz"]
            row["thd_pct"] = t["thd_pct"]
            row["thd_db"] = t["thd_db"]
            row["thdn_pct"] = t["thdn_pct"]
            row["thdn_db"] = t["thdn_db"]
            row["spur_dbfs"] = t["spur_dbfs"]
            row["spur_hz"] = t["spur_hz"]
        elif test == "noise":
            c = measure_centroid(data, sr)
            row["centroid_hz"] = c["centroid_hz"]
            row["hf_ratio"] = c["hf_ratio"]
            ev = detect_events(data, sr)
            row["event_count"] = ev["event_count"]
            row["events_per_sec"] = ev["events_per_sec"]
            row["mean_event_interval_s"] = ev["mean_interval_s"]
        elif test == "sweep":
            c = measure_centroid(data, sr)
            row["centroid_hz"] = c["centroid_hz"]
            row["hf_ratio"] = c["hf_ratio"]
    except Exception as exc:                    # keep the matrix alive on failure
        row["error"] = f"{type(exc).__name__}: {exc}"

    row["file"] = os.path.basename(wav)
    return row


def _markdown_report(rows, args, csv_path):
    lines = []
    lines.append(f"# Level sweep: {os.path.basename(args.plugin)}")
    lines.append("")
    lines.append(f"- levels: {', '.join(f'{float(v):g}' for v in rows[0]['_levels'])}"
                 if rows and rows[0].get("_levels") else "")
    lines.append(f"- sr: {args.sr} Hz, warmup {args.warmup} s, channels {args.channels}")
    if args.sets:
        lines.append(f"- params: `{'`, `'.join(args.sets)}`")
    if args.note:
        lines.append(f"- note: {args.note}")
    lines.append(f"- CSV: `{os.path.relpath(csv_path, ROOT)}`" if os.path.isabs(csv_path)
                 else f"- CSV: `{csv_path}`")
    lines.append("")

    tests = []
    for r in rows:
        if r["test"] not in tests:
            tests.append(r["test"])

    for test in tests:
        sub = [r for r in rows if r["test"] == test]
        lines.append(f"## {test}")
        lines.append("")
        if test == "impulse":
            lines.append("| level | dBFS | peak dBFS | RMS dBFS | peak gain dB | T30 s | T20 s | EDT s | centroid Hz | HF>8k | events |")
            lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
            for r in sub:
                fit = "" if not r["rt60_truncated"] else " (trunc)"
                lines.append(
                    f"| {r['level']} | {r['level_dbfs']:.1f} | {_fmt(r['peak_dbfs'], 2)} | "
                    f"{_fmt(r['rms_dbfs'], 2)} | {_fmt(r['peak_gain_db'], 2)} | "
                    f"{_fmt(r['rt60_t30_s'], 3)}{fit} | {_fmt(r['rt60_t20_s'], 3)} | "
                    f"{_fmt(r['edt_s'], 3)} | {_fmt(r['centroid_hz'], 0)} | "
                    f"{_fmt(100.0 * r['hf_ratio'] if r['hf_ratio'] is not None else None, 2)} % | "
                    f"{r['event_count']} |")
        elif test == "sine":
            lines.append("| level | dBFS | peak dBFS | THD % | THD dB | THD+N % | spur dBFS | spur Hz |")
            lines.append("|---|---|---|---|---|---|---|---|")
            for r in sub:
                lines.append(
                    f"| {r['level']} | {r['level_dbfs']:.1f} | {_fmt(r['peak_dbfs'], 2)} | "
                    f"{_fmt(r['thd_pct'], 6)} | {_fmt(r['thd_db'], 1)} | "
                    f"{_fmt(r['thdn_pct'], 4)} | {_fmt(r['spur_dbfs'], 1)} | "
                    f"{_fmt(r['spur_hz'], 1)} |")
        elif test == "noise":
            lines.append("| level | dBFS | peak dBFS | RMS dBFS | crest dB | centroid Hz | HF>8k | events | events/s | mean gap s |")
            lines.append("|---|---|---|---|---|---|---|---|---|---|")
            for r in sub:
                lines.append(
                    f"| {r['level']} | {r['level_dbfs']:.1f} | {_fmt(r['peak_dbfs'], 2)} | "
                    f"{_fmt(r['rms_dbfs'], 2)} | {_fmt(r['crest_db'], 2)} | "
                    f"{_fmt(r['centroid_hz'], 0)} | "
                    f"{_fmt(100.0 * r['hf_ratio'] if r['hf_ratio'] is not None else None, 2)} % | "
                    f"{r['event_count']} | {_fmt(r['events_per_sec'], 3)} | "
                    f"{_fmt(r['mean_event_interval_s'], 3)} |")
        else:
            lines.append("| level | dBFS | peak dBFS | RMS dBFS | centroid Hz | HF>8k |")
            lines.append("|---|---|---|---|---|---|")
            for r in sub:
                lines.append(
                    f"| {r['level']} | {r['level_dbfs']:.1f} | {_fmt(r['peak_dbfs'], 2)} | "
                    f"{_fmt(r['rms_dbfs'], 2)} | {_fmt(r['centroid_hz'], 0)} | "
                    f"{_fmt(100.0 * r['hf_ratio'] if r['hf_ratio'] is not None else None, 2)} % |")
        errors = [r for r in sub if r["error"]]
        if errors:
            lines.append("")
            for r in errors:
                lines.append(f"- ERROR at {r['level']}: {r['error']}")
        lines.append("")

    lines.append("## Notes")
    lines.append("")
    lines.append("- `peak gain dB` = output peak dBFS - input level dBFS; a linear, "
                 "gain-normalised render keeps this constant across the sweep.")
    lines.append("- RT60 is Schroeder T30 (5..-35 dB fit) from the IR onset; `trunc` means "
                 "the render ended before -35 dB.")
    lines.append("- THD uses least-squares fits at exact harmonic multiples of the "
                 "rendered sine frequency; THD+N is residual RMS / fundamental RMS.")
    lines.append("- THD is measured in a late window (default: last 1 s of a 20 s sine "
                 "render) so the spring onset transient does not dominate it.")
    lines.append("- events = envelope local maxima within 6 dB of the loudest envelope "
                 "frame that rise +16 dB out of the preceding 100 ms and sit +6 dB "
                 "above a 500 ms local median, 500 ms refractory (2 ms RMS envelope at "
                 "a 1 ms hop); one physical hit gives one event, a stationary tone or "
                 "noise gives none, `--band-lo 4000` exposes the spring's dispersion "
                 "micro-arrivals.")
    return "\n".join(lines)


def cmd_sweep(args):
    analyzer = args.analyzer
    plugin = args.plugin
    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)

    levels = [float(v) for v in args.levels]
    tests = [t.strip().lower() for t in args.tests.split(",") if t.strip()]
    for t in tests:
        if t not in TEST_SECONDS:
            raise SystemExit(f"unknown test: {t}")

    base = ["--sr", str(args.sr), "--channels", args.channels,
            "--warmup", str(args.warmup)]
    for s in args.sets:
        base += ["--set", s]

    rows = []
    for test in tests:
        sec = args.sec if args.sec is not None else TEST_SECONDS[test]
        if test == "sine":
            # Measure THD only once the onset transient has decayed: the spring
            # response needs several seconds to settle (RT60 ~2.6 s), so the
            # default analysis window is the last second of the render.
            if args.sine_start is None:
                args.sine_start = max(1.5, float(sec) - 1.5)
            if args.sine_start + args.sine_dur > sec:
                args.sine_dur = max(0.25, float(sec) - args.sine_start - 0.1)
        for i, level in enumerate(levels):
            tag = _level_tag(i, level)
            wav = os.path.join(out_dir, f"{args.prefix}_{test}_{tag}.wav")
            extra = list(base) + ["--test", test, "--sec", f"{sec:g}",
                                  "--level", f"{level:g}"]
            if test == "sine" and args.sine_hz:
                extra += ["--sine", f"{args.sine_hz:g}"]
            if test == "sweep" and args.sweep_end:
                extra += ["--sweep-end", f"{args.sweep_end:g}"]

            if os.path.exists(wav) and not args.force:
                pass
            else:
                rc, _summary, err = _run_render(analyzer, plugin, wav, extra)
                if rc != 0:
                    row = {"test": test, "index": i, "level": level,
                           "level_dbfs": level_to_db(level), "sr": args.sr,
                           "sec": sec, "file": os.path.basename(wav),
                           "error": f"render rc={rc}: {err}"}
                    for col in CSV_COLUMNS:
                        row.setdefault(col, None)
                    rows.append(row)
                    print(f"[{test}] level {level:g}: RENDER FAILED rc={rc}", file=sys.stderr)
                    continue

            row = _measure_one(test, wav, level, args)
            row["test"] = test
            row["index"] = i
            row["level"] = level
            row["level_dbfs"] = level_to_db(level)
            row["sec"] = sec
            rows.append(row)
            print(f"[{test}] level {level:g} ({level_to_db(level):+.1f} dBFS): "
                  f"peak {_fmt(row['peak_dbfs'], 2)} dBFS"
                  + (f"  THD {row['thd_pct']:.3g} %" if test == "sine" else "")
                  + (f"  T30 {_fmt(row['rt60_t30_s'], 2)} s" if test == "impulse" else ""),
                  file=sys.stderr)

    # --- CSV ------------------------------------------------------------
    csv_path = args.csv_out or os.path.join(DEFAULT_OUT_DIR, f"level_sweep_{args.prefix}.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        for r in rows:
            writer.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in CSV_COLUMNS})

    # `_levels` is only used by the markdown header.
    if rows:
        rows[0]["_levels"] = [float(v) for v in args.levels]

    md = _markdown_report(rows, args, csv_path)
    if args.md_out:
        with open(args.md_out, "w", encoding="utf-8") as fh:
            fh.write(md + "\n")
        print(f"wrote {csv_path} and {args.md_out}", file=sys.stderr)
    else:
        print(md)
    return 0


# ------------------------------------------------------------------ main ----

def build_parser():
    p = argparse.ArgumentParser(
        description="Measurement toolkit for Rivage Reverb renders (float32 WAV).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    # info
    s = sub.add_parser("info", help="file shape, peak, RMS, DC")
    s.add_argument("wav")
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_info)

    # thd
    s = sub.add_parser("thd", help="THD / THD+N / largest non-harmonic spur")
    s.add_argument("wav")
    s.add_argument("--channel", default="L", help="L, R or mix")
    s.add_argument("--start", type=float, default=1.5, help="segment start, seconds")
    s.add_argument("--dur", type=float, default=1.0, help="segment length, seconds")
    s.add_argument("--freq", type=float, default=None,
                   help="fundamental; default: estimate from the spectrum")
    s.add_argument("--max-harmonics", type=int, default=20)
    s.add_argument("--hp", type=float, default=20.0, help="ignore bins below this Hz")
    s.add_argument("--spur-guard-bins", type=float, default=5.0,
                   help="blank +-N analysis bins (sr/segment) around every harmonic "
                        "before the spur search (default 5)")
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_thd)

    # rt60
    s = sub.add_parser("rt60", help="Schroeder T20/T30/EDT, broadband and per band")
    s.add_argument("wav")
    s.add_argument("--channel", default="both", help="both, L or R")
    s.add_argument("--no-bands", action="store_true")
    s.add_argument("--onset-db", type=float, default=-60.0)
    s.add_argument("--trunc-db", type=float, default=60.0)
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_rt60)

    # centroid
    s = sub.add_parser("centroid", help="spectral centroid, trajectory, HF share")
    s.add_argument("wav")
    s.add_argument("--channel", default="mix")
    s.add_argument("--start", type=float, default=None)
    s.add_argument("--dur", type=float, default=None)
    s.add_argument("--onset-db", type=float, default=-60.0)
    s.add_argument("--frame", type=int, default=2048)
    s.add_argument("--hop", type=float, default=0.1)
    s.add_argument("--hf", type=float, default=8000.0)
    s.add_argument("--min-db", type=float, default=60.0,
                   help="trajectory frames this far below the loudest frame are marked invalid")
    s.add_argument("--max-traj", type=int, default=20)
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_centroid)

    # events
    s = sub.add_parser("events",
                       help="impact/onset detection (rise-based; optional band-pass)")
    s.add_argument("wav")
    s.add_argument("--channel", default="mix")
    s.add_argument("--band-lo", type=float, default=0.0,
                   help="band-pass low edge in Hz; 0 = broadband (default)")
    s.add_argument("--band-hi", type=float, default=8000.0,
                   help="band-pass high edge in Hz, used when --band-lo > 0")
    s.add_argument("--env-ms", type=float, default=2.0)
    s.add_argument("--floor-db", type=float, default=6.0,
                   help="an event peak must be within this of the loudest envelope frame")
    s.add_argument("--refractory", type=float, default=0.5,
                   help="minimum spacing between events, seconds")
    s.add_argument("--median-ms", type=float, default=500.0,
                   help="local-median baseline window")
    s.add_argument("--margin-db", type=float, default=6.0,
                   help="event must exceed the local median by this much")
    s.add_argument("--rise-ms", type=float, default=100.0,
                   help="look-back window for the rise-from-quiet test")
    s.add_argument("--rise-db", type=float, default=16.0,
                   help="event must rise this far above the quietest point of the look-back")
    s.add_argument("--max-events", type=int, default=20)
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_events)

    # compare
    s = sub.add_parser("compare", help="sample-aligned comparison of two WAVs")
    s.add_argument("a")
    s.add_argument("b")
    s.add_argument("--fail-on-diff", action="store_true")
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_compare)

    # sweep
    s = sub.add_parser("sweep", help="render a level x test matrix and write CSV/MD")
    s.add_argument("plugin", nargs="?", default=DEFAULT_PLUGIN)
    s.add_argument("--analyzer", default=DEFAULT_ANALYZER)
    s.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    s.add_argument("--prefix", default="sweep")
    s.add_argument("--levels", nargs="+", default=SWEEP_LEVELS,
                   help="linear amplitudes; dBFS = 20*log10(level)")
    s.add_argument("--tests", default="impulse,sine,noise,sweep")
    s.add_argument("--sets", nargs="*", default=[],
                   help='parameter assignments, e.g. --sets Algorithm=SPRING Mix=1.0')
    s.add_argument("--sr", type=int, default=48000)
    s.add_argument("--channels", default="LR")
    s.add_argument("--warmup", type=float, default=1.0)
    s.add_argument("--sec", type=float, default=None,
                   help="render length override for every test")
    s.add_argument("--sine-hz", type=float, default=1000.0)
    s.add_argument("--sweep-end", type=float, default=None)
    s.add_argument("--sine-start", type=float, default=None,
                   help="THD analysis window start (default: last second of the render)")
    s.add_argument("--sine-dur", type=float, default=1.0)
    s.add_argument("--max-harmonics", type=int, default=20)
    s.add_argument("--csv-out", default=None)
    s.add_argument("--md-out", default=None)
    s.add_argument("--note", default="")
    s.add_argument("--force", action="store_true",
                   help="re-render even when the wav already exists")
    s.set_defaults(func=cmd_sweep)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args) or 0
    except BrokenPipeError:
        return 0
    except (ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
