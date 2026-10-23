#!/usr/bin/env python3
"""Per-rig latency calibration store.

Problem: unlike a DAW, we do not get the round-trip figure from the driver. It
has to be measured once per rig, then trusted — and re-checked when the rig
changes.

Design decisions:

* Fingerprint = (host, device, sample rate, link). Those change the latency
  unpredictably, so any change invalidates the calibration.
* Buffer size is NOT part of the fingerprint. Latency is linear in buffer size,
  so a stored point extrapolates exactly; changing the buffer must never force
  another chirp measurement.
* One point per buffer, so two points recover the true slope empirically instead
  of trusting the model's coefficient.
* A calibration is rejected unless the correlation peak is strong and the takes
  agree. A confident number from a bad take is worse than no number.
* Re-verification compares a fresh measurement against the prediction and
  distinguishes "stable" from "drift" rather than silently overwriting.

Store is JSON, one file, human-readable, safe to diff.
"""

import argparse
import hashlib
import json
import os
import statistics
import sys
import time

import numpy as np

from measure_latency import find_delay, read_wav, stimulus, write_wav

SCHEMA = 1
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_STORE = os.path.join(HERE, "profiles.json")

# Fallback slope when only one buffer point exists: total = 3 * buffer_ms + const
# (two I/O fills + one partitioned-IR block).
DEFAULT_MS_PER_SAMPLE = 3.0 / 48000.0 * 1000.0  # at 48 kHz

MIN_CONFIDENCE = 20.0
MAX_JITTER_MS = 1.5
# Median+MAD is robust to noise but blind to a bimodal split: takes of
# 7.6 / 19.0 / 7.6 give MAD = 0, because the majority cluster wins. Peak-to-peak
# spread catches that. Both gates must pass.
MAX_SPREAD_MS = 3.0
FRESH_DAYS = 30


# ---------------------------------------------------------------- fingerprint

FIELDS = ("host", "device", "sr", "link")


def normalise(rig):
    out = {}
    for f in FIELDS:
        v = rig.get(f)
        if f == "host" and not v:
            import socket
            v = socket.gethostname()
        if f == "sr":
            v = int(v)
        out[f] = v
    return out


def fingerprint(rig):
    canon = json.dumps(normalise(rig), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


# ---------------------------------------------------------------------- store


def load_store(path):
    if not os.path.exists(path):
        return {"schema": SCHEMA, "rigs": {}}
    with open(path) as f:
        data = json.load(f)
    if data.get("schema") != SCHEMA:
        raise SystemExit(f"store schema {data.get('schema')} != {SCHEMA}; refusing to guess")
    return data


def save_store(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


# ------------------------------------------------------------------ measuring


def measure_takes(paths, sr, ref_path=None):
    """Median, jitter (MAD) and minimum confidence across N recorded takes."""
    ref = read_wav(ref_path)[0] if ref_path else stimulus(sr)
    vals, confs = [], []
    for p in paths:
        rec, rsr = read_wav(p)
        if rsr != sr:
            raise SystemExit(f"{p}: {rsr} Hz but rig says {sr} Hz")
        lag, _, conf = find_delay(ref, rec, sr)
        vals.append(lag * 1000.0 / sr)
        confs.append(conf)
    med = statistics.median(vals)
    jitter = statistics.median([abs(v - med) for v in vals])
    return {
        "ms": round(med, 3),
        "jitter_ms": round(jitter, 3),
        "spread_ms": round(max(vals) - min(vals), 3),
        "confidence": round(min(confs), 1),
        "takes": [round(v, 3) for v in vals],
        "n": len(vals),
    }


# ----------------------------------------------------------------- prediction


def predict(data, rig, buffer):
    """Return dict(ms, status, detail) for this rig at this buffer size."""
    fp = fingerprint(rig)
    entry = data["rigs"].get(fp)
    if not entry:
        return {"ms": None, "status": "uncalibrated", "detail": "no calibration for this rig"}

    sr = normalise(rig)["sr"]
    points = {int(b): p for b, p in entry["points"].items()}

    if buffer in points:
        p = points[buffer]
        age = (time.time() - p["ts"]) / 86400.0
        fresh = age <= FRESH_DAYS
        return {
            "ms": p["ms"],
            "status": "exact" if fresh else "stale",
            "detail": f"measured at {buffer} samples, {age:.0f} days ago",
        }

    if len(points) >= 2:
        lo, hi = sorted(points, key=lambda b: abs(b - buffer))[:2]
        x1, x2, y1, y2 = lo, hi, points[lo]["ms"], points[hi]["ms"]
        slope = (y2 - y1) / (x2 - x1)
        ms = y1 + slope * (buffer - x1)
        return {
            "ms": round(ms, 3),
            "status": "extrapolated",
            "detail": f"slope {slope * 1000:.3f} ms/ksample from {lo}/{hi}-sample points",
        }

    if len(points) == 1:
        b0, p0 = next(iter(points.items()))
        ms = p0["ms"] + DEFAULT_MS_PER_SAMPLE * (buffer - b0)
        return {
            "ms": round(ms, 3),
            "status": "extrapolated",
            "detail": f"one point at {b0}; model slope (measure a second buffer to pin it)",
        }

    return {"ms": None, "status": "uncalibrated", "detail": "empty profile"}


# ------------------------------------------------------------------ calibrate


def calibrate(data, rig, buffer, result, ref_note=""):
    if result["confidence"] < MIN_CONFIDENCE:
        return False, (f"rejected: correlation confidence {result['confidence']} "
                       f"< {MIN_CONFIDENCE} (clipped, noisy, or no signal)")
    if result["jitter_ms"] > MAX_JITTER_MS:
        return False, (f"rejected: jitter {result['jitter_ms']} ms > {MAX_JITTER_MS} ms; "
                       "takes disagree, so there is no stable number to store")
    if result.get("spread_ms", 0.0) > MAX_SPREAD_MS:
        return False, (f"rejected: peak-to-peak spread {result['spread_ms']} ms > "
                       f"{MAX_SPREAD_MS} ms (takes {result['takes']}); "
                       "the chain is not delivering consistent latency")

    fp = fingerprint(rig)
    entry = data["rigs"].setdefault(fp, {"rig": normalise(rig), "points": {}})
    entry["points"][str(buffer)] = {
        "ms": result["ms"],
        "jitter_ms": result["jitter_ms"],
        "spread_ms": result.get("spread_ms", 0.0),
        "confidence": result["confidence"],
        "n": result["n"],
        "ts": time.time(),
        "note": ref_note,
    }
    return True, f"stored {result['ms']:.2f} ms at {buffer} samples ({result['n']} takes)"


def reverify(data, rig, buffer, result):
    """Compare a fresh measurement against the prediction."""
    pred = predict(data, rig, buffer)
    if pred["ms"] is None:
        return "no_baseline", pred, 0.0
    delta = result["ms"] - pred["ms"]
    tol = max(2.0, 0.12 * abs(pred["ms"]))
    verdict = "stable" if abs(delta) <= tol else "drift"
    return verdict, pred, delta


# -------------------------------------------------------------------- selftest


def selftest():
    import tempfile

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print(f"  {'ok  ' if cond else 'FAIL'} {name} {detail}")

    sr = 48000
    ref = stimulus(sr)
    tmp = tempfile.mkdtemp()
    rng = np.random.default_rng(11)

    def make_take(ms, name):
        d = int(round(ms * 1e-3 * sr))
        y = np.concatenate([np.zeros(d), 0.5 * ref])
        y = y + rng.normal(0, 0.01, len(y))
        p = os.path.join(tmp, name)
        write_wav(p, y, sr)
        return p

    store = os.path.join(tmp, "profiles.json")
    data = load_store(store)
    rig = {"host": "testbox", "device": "Scarlett Solo", "sr": sr, "link": "none"}

    # two buffer points, three takes each
    r64 = measure_takes([make_take(7.6, f"a{i}.wav") for i in range(3)], sr)
    r128 = measure_takes([make_take(11.6, f"b{i}.wav") for i in range(3)], sr)

    got, msg = calibrate(data, rig, 64, r64)
    check("calibrate 64 samples", got, msg)
    got, msg = calibrate(data, rig, 128, r128)
    check("calibrate 128 samples", got, msg)
    save_store(store, data)

    check("exact hit at 64", abs(predict(data, rig, 64)["ms"] - 7.6) < 0.05)
    check("exact hit at 128", abs(predict(data, rig, 128)["ms"] - 11.6) < 0.05)

    p256 = predict(data, rig, 256)
    check("extrapolates 256 to 19.6", abs(p256["ms"] - 19.6) < 0.1, f"{p256['ms']} ms")
    check("extrapolation is labelled", p256["status"] == "extrapolated")

    # buffer change must not invalidate
    check("buffer change keeps calibration", predict(data, rig, 512)["status"] == "extrapolated")

    # device change must invalidate
    other = dict(rig, device="Built-in Audio")
    check("device change invalidates", predict(data, other, 64)["status"] == "uncalibrated")

    # link change must invalidate
    bt = dict(rig, link="bt")
    check("link change invalidates", predict(data, bt, 64)["status"] == "uncalibrated")

    # sample-rate change must invalidate
    check("rate change invalidates", predict(data, dict(rig, sr=96000), 64)["status"] == "uncalibrated")

    # reverify: stable inside tolerance
    good = measure_takes([make_take(8.4, f"c{i}.wav") for i in range(3)], sr)
    v, _, d = reverify(data, rig, 64, good)
    check("within tolerance reads stable", v == "stable", f"delta {d:+.2f} ms")

    # reverify: drift outside tolerance
    bad = measure_takes([make_take(14.0, f"d{i}.wav") for i in range(3)], sr)
    v, _, d = reverify(data, rig, 64, bad)
    check("outside tolerance reads drift", v == "drift", f"delta {d:+.2f} ms")

    # noisy takes must be refused, not stored
    mixed = [make_take(7.6, "f0.wav"), make_take(19.0, "f1.wav"), make_take(7.6, "f2.wav")]
    rr = measure_takes(mixed, sr)
    got, msg = calibrate(data, dict(rig, device="Inconsistent"), 64, rr)
    check("bimodal takes rejected", not got, msg)

    # ordinary jitter must still be accepted
    small = [make_take(7.3, "g0.wav"), make_take(7.9, "g1.wav"), make_take(7.6, "g2.wav")]
    got, msg = calibrate(data, dict(rig, device="Consistent"), 64, measure_takes(small, sr))
    check("normal jitter accepted", got, msg)

    # persistence round-trip
    reloaded = load_store(store)
    check("store round-trips", abs(predict(reloaded, rig, 64)["ms"] - 7.6) < 0.05)

    print("\nall assertions passed" if ok else "\nFAILURES above")
    return 0 if ok else 1


# ------------------------------------------------------------------------ cli


def fmt(rig):
    r = normalise(rig)
    return f"{r['device']} @ {r['sr']} Hz, link={r['link']}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--store", default=DEFAULT_STORE)
    ap.add_argument("--device", help="audio device name")
    ap.add_argument("--sr", type=int, default=48000)
    ap.add_argument("--link", default="none", help="none | rf | bt | bt:<codec>")
    ap.add_argument("--buffer", type=int, help="I/O buffer in samples")
    ap.add_argument("--takes", nargs="*", help="recorded take wavs")
    ap.add_argument("--ref", help="stimulus wav (default: regenerate)")
    ap.add_argument("--simulate", type=float, metavar="MS",
                    help="synthesise takes at this latency instead of recording")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    data = load_store(a.store)

    if a.list:
        if not data["rigs"]:
            print("no calibrations stored")
        for fp, e in sorted(data["rigs"].items()):
            print(f"{fp}  {fmt(e['rig'])}")
            for b, p in sorted(e["points"].items(), key=lambda kv: int(kv[0])):
                age = (time.time() - p["ts"]) / 86400.0
                print(f"    {b:>4} samples: {p['ms']:8.2f} ms  "
                      f"jitter {p['jitter_ms']:.2f}  spread {p.get('spread_ms', 0):.2f}  "
                      f"conf {p['confidence']:5.1f}  {age:.0f}d ago")
        return 0

    rig = {"device": a.device, "sr": a.sr, "link": a.link}

    if a.buffer:
        pred = predict(data, rig, a.buffer)
        print(f"predicted: {pred['ms'] if pred['ms'] is None else round(pred['ms'], 2)} ms  "
              f"[{pred['status']}] {pred['detail']}")

    calibrating = bool(a.takes or a.simulate is not None)
    if not calibrating:
        return 0
    if not a.buffer:
        ap.error("--buffer is required when calibrating")
    if not a.device:
        ap.error("--device is required when calibrating")

    if a.simulate is not None:
        import tempfile
        ref = stimulus(a.sr)
        paths = []
        for i in range(3):
            d = int(round(a.simulate * 1e-3 * a.sr))
            y = np.concatenate([np.zeros(d), 0.5 * ref])
            y += np.random.default_rng(i).normal(0, 0.01, len(y))
            p = os.path.join(tempfile.mkdtemp(), f"sim{i}.wav")
            write_wav(p, y, a.sr)
            paths.append(p)
        print(f"(simulating {a.simulate} ms chain, 3 takes)")
    else:
        paths = a.takes

    result = measure_takes(paths, a.sr, a.ref)
    print(f"measured: {result['ms']:.2f} ms  jitter {result['jitter_ms']:.2f}  "
          f"spread {result['spread_ms']:.2f}  conf {result['confidence']:.1f}  "
          f"takes {result['takes']}")

    verdict, pred, delta = reverify(data, rig, a.buffer, result)
    if verdict in ("stable", "drift"):
        print(f"re-verify: {verdict} (delta {delta:+.2f} ms vs prediction {pred['ms']:.2f})")

    ok, msg = calibrate(data, rig, a.buffer, result)
    print(msg)
    if ok:
        save_store(a.store, data)
        print(f"saved to {a.store}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
