#!/usr/bin/env python3
"""Measure real end-to-end latency of any analog link with a loopback recording.

Vendor datasheets lie in both directions: "aptX LL" dongles silently fall back to
SBC, and "5 ms" systems are 5 ms at the radio plus 2 ms of converters. This
measures the thing that actually matters.

Usage
-----
1. Generate the stimulus (a click train, 1 s of lead-in silence):

    python3 measure_latency.py --make-stimulus click.wav --sr 48000

2. Play click.wav into the transmitter, record the receiver output on a second
   interface, keeping the original file's timing (i.e. record the same duration
   starting at the same moment, or just capture the whole take).

3. Compare:

    python3 measure_latency.py --ref click.wav --rec take.wav

The reported number is receiver-output-minus-input, i.e. the full chain delay
including both converters. Repeat 5x and take the median; jitter is the number
that actually ruins feel, not the mean.

Self-test: python3 measure_latency.py --selftest
"""

import argparse
import sys
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        nch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        sys.exit(f"unsupported sample width: {sw} bytes")
    if nch > 1:
        x = x.reshape(-1, nch).mean(axis=1)
    return x, sr


def write_wav(path, x, sr):
    pcm = np.clip(x, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def chirp(sr, dur_ms=20.0, f0=500.0, f1=8000.0):
    """Hann-windowed linear sweep. Its autocorrelation has one sharp peak, unlike a
    click train, whose repeated clicks produce several equally strong peaks and let
    the peak picker lock onto the wrong one."""
    n = int(dur_ms * 1e-3 * sr)
    t = np.arange(n) / sr
    T = n / sr
    k = (f1 - f0) / T
    phase = 2.0 * np.pi * (f0 * t + 0.5 * k * t * t)
    return np.hanning(n) * np.sin(phase) * 0.9


def stimulus(sr, lead_s=1.0, reps=1, gap_s=1.0, dur_ms=20.0):
    """Silence, then one 300 Hz-8 kHz sweep.

    One sweep, not a train. Repeats of any stimulus that contains the same
    frequency content produce a nearly equal correlation peak at +/- the repeat
    gap, so the peak picker can pick the wrong one and the confidence number
    becomes meaningless. Repeat *takes* instead of repeats within a take.
    """
    total = int((lead_s + reps * gap_s + 0.1) * sr)
    x = np.zeros(total)
    one = chirp(sr, dur_ms=dur_ms)
    for i in range(reps):
        start = int((lead_s + i * gap_s) * sr)
        x[start:start + len(one)] += one
    return x


def find_delay(ref, rec, sr):
    """Samples by which `rec` lags `ref`. Positive = rec is late."""
    n = 1
    while n < len(ref) + len(rec):
        n <<= 1
    spec = np.fft.rfft(rec, n) * np.conj(np.fft.rfft(ref, n))
    cc = np.fft.irfft(spec, n)
    k = int(np.argmax(np.abs(cc)))
    lag = k if k < n // 2 else k - n
    peak = float(np.abs(cc[k]))
    # mask the mainlobe, not just one bin, or the peak's own neighbours count as
    # a second peak and confidence is meaningless
    w = max(1, int(0.005 * sr))
    cc[np.arange(k - w, k + w + 1) % n] = 0.0
    second = float(np.max(np.abs(cc)))
    confidence = peak / second if second > 0 else float("inf")
    return lag, sr, confidence


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref")
    ap.add_argument("--rec")
    ap.add_argument("--make-stimulus")
    ap.add_argument("--sr", type=int, default=48000)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.make_stimulus:
        write_wav(a.make_stimulus, stimulus(a.sr), a.sr)
        print(f"wrote {a.make_stimulus} ({a.sr} Hz, lead-in 1 s then one 300-8000 Hz chirp)")
        return

    if a.selftest:
        sr = 48000
        rng = np.random.default_rng(7)
        for delay_ms in (0.0, 2.5, 26.0, 171.0, 300.0):
            d = int(round(delay_ms * 1e-3 * sr))
            ref = stimulus(sr)
            rec = np.concatenate([np.zeros(d), ref])[: len(ref) + d]
            # -34 dBFS of hiss plus a 6 dB pad, like a real receiver in a quiet room
            rec = 0.5 * rec + rng.normal(0, 0.02, len(rec))
            lag, _, conf = find_delay(ref, rec, sr)
            got = lag * 1000.0 / sr
            ok = abs(got - delay_ms) < 0.05 and conf > 3.0
            print(f"  delay {delay_ms:6.2f} ms -> {got:7.2f} ms  conf {conf:6.1f}  {'ok' if ok else 'FAIL'}")
        return

    if not (a.ref and a.rec):
        ap.error("need --ref and --rec (or --make-stimulus / --selftest)")

    ref, sr1 = read_wav(a.ref)
    rec, sr2 = read_wav(a.rec)
    if sr1 != sr2:
        sys.exit(f"sample rates differ ({sr1} vs {sr2}); record at the stimulus rate")

    lag, sr, conf = find_delay(ref, rec, sr1)
    ms = lag * 1000.0 / sr
    print(f"latency: {ms:.2f} ms  ({lag} samples at {sr} Hz, correlation confidence {conf:.1f})")
    if conf < 3.0:
        print("warning: weak peak - clicks may be clipped, drowned in noise, or dropped entirely")
    print("median of 5 takes is your real number; spread is your jitter.")


if __name__ == "__main__":
    main()
