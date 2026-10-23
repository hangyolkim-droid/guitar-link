#!/usr/bin/env python3
"""Latency modes and a green/yellow/red live-latency indicator.

Modelled on DAW behaviour: an I/O buffer ladder plus a Low Latency Mode that
bypasses the stages which actually cost time, and an indicator driven by
*measured* latency rather than the configured buffer.

The rule that matters: the buffer is not the latency. Total is

    2 * buffer            input fill + output fill
  + driver overhead       OS/class-compliant stack
  + DSP delay             oversampling, IR partition, lookahead
  + link                  wireless hop, if any

Run `python3 latency_mode.py` for the table, `--selftest` for assertions.
"""

from collections import deque
import statistics

ORDER = ("green", "yellow", "red")
BOUNDS = (10.0, 20.0)  # ms: transparent, audible-but-playable, unplayable

RATES = (44100, 48000, 96000)

# buffer sizes offered, in samples
BUFFER_LADDER = (32, 64, 128, 256, 512)

DRIVER_OVERHEAD_MS = {
    "usb": 1.5,       # class-compliant USB interface
    "onboard": 1.0,   # built-in codec
    "thunderbolt": 0.8,
    "android_usb": 12.0,  # the reason not to host the amp sim on a phone
}

OVERSPAMPLE_DELAY_MS = {1: 0.0, 2: 0.3, 4: 0.6, 8: 1.2}


def estimate(sr=48000, buffer=64, oversample=4, low_latency_mode=False,
             lookahead_ms=1.5, link_ms=0.0, platform="usb"):
    """Return the round-trip latency breakdown in milliseconds."""
    buf_ms = buffer * 1000.0 / sr
    io = 2.0 * buf_ms
    driver = DRIVER_OVERHEAD_MS[platform]
    os_delay = OVERSPAMPLE_DELAY_MS[oversample]

    if low_latency_mode:
        # short direct-form cab filter, no lookahead limiter, 2x oversampling
        ir = 0.0
        lookahead = 0.0
        os_delay = OVERSPAMPLE_DELAY_MS[2]
    else:
        # uniform-partitioned convolution costs one full block
        ir = buf_ms
        lookahead = lookahead_ms

    dsp = os_delay + ir + lookahead
    parts = {
        "io": io,
        "driver": driver,
        "oversample": os_delay,
        "ir": ir,
        "lookahead": lookahead,
        "link": link_ms,
    }
    return {"total": sum(parts.values()), "buffer_ms": buf_ms, "parts": parts}


def classify(ms, bounds=BOUNDS):
    if ms <= bounds[0]:
        return "green"
    if ms <= bounds[1]:
        return "yellow"
    return "red"


def advice(state):
    return {
        "green": "live-safe: feels like a cable",
        "yellow": "playable, but you hear the attack late",
        "red": "not live-safe: reduce buffer or bypass stages",
    }[state]


class Indicator:
    """Traffic light with median smoothing and hysteresis, so it stops flickering
    when latency sits on a threshold."""

    def __init__(self, window=9, hysteresis_ms=0.5, bounds=BOUNDS):
        self.window = deque(maxlen=window)
        self.hyst = hysteresis_ms
        self.bounds = bounds
        self.state = None

    def push(self, measured_ms):
        self.window.append(measured_ms)
        med = statistics.median(self.window)
        raw = classify(med, self.bounds)

        if self.state is None:
            self.state = raw
            return self.state, med

        i, j = ORDER.index(self.state), ORDER.index(raw)
        if j > i and med > self.bounds[i] + self.hyst:
            self.state = ORDER[i + 1]
        elif j < i and med < self.bounds[i - 1] - self.hyst:
            self.state = ORDER[i - 1]
        return self.state, med

    def reset(self):
        self.window.clear()
        self.state = None


def selftest():
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print(f"  {'ok  ' if cond else 'FAIL'} {name} {detail}")

    t32 = estimate(48000, 32)["total"]
    t64 = estimate(48000, 64)["total"]
    t256 = estimate(48000, 256)["total"]
    t512 = estimate(48000, 512)["total"]

    check("32/48k is green", classify(t32) == "green", f"{t32:.2f} ms")
    check("64/48k is green", classify(t64) == "green", f"{t64:.2f} ms")
    check("256/48k is yellow", classify(t256) == "yellow", f"{t256:.2f} ms")
    check("512/48k is red", classify(t512) == "red", f"{t512:.2f} ms")

    llm = estimate(48000, 128, low_latency_mode=True)["total"]
    norm = estimate(48000, 128)["total"]
    check("low latency mode saves time", llm < norm, f"{llm:.2f} vs {norm:.2f} ms")

    bt = estimate(48000, 64, link_ms=40.0)["total"]
    check("40 ms link forces red", classify(bt) == "red", f"{bt:.2f} ms")

    # monotonic in buffer size
    series = [estimate(48000, b)["total"] for b in BUFFER_LADDER]
    check("latency increases with buffer", all(a < b for a, b in zip(series, series[1:])))

    # hysteresis: straddling 10 ms must not oscillate
    ind = Indicator(hysteresis_ms=0.5)
    for _ in range(10):
        ind.push(9.8)
    check("settles green below boundary", ind.state == "green", ind.state)
    transitions = 0
    prev = ind.state
    for i in range(40):
        ind.push(10.2 if i % 2 == 0 else 9.9)
        if ind.state != prev:
            transitions += 1
            prev = ind.state
    check("no flicker on a straddling signal", transitions <= 1, f"{transitions} transitions")

    # a sustained move must eventually change state
    for _ in range(20):
        ind.push(25.0)
    check("sustained 25 ms reaches red", ind.state == "red", ind.state)

    print("\nall assertions passed" if ok else "\nFAILURES above")
    return 0 if ok else 1


def table():
    print(f"{'mode':10} {'buf':>4} {'LLM':>4} {'48k ms':>8} {'light':>7}  advice")
    print("-" * 78)
    for buf, name in zip(BUFFER_LADDER, ("ultra", "low", "balanced", "safe", "huge")):
        for llm in (False, True):
            r = estimate(48000, buf, low_latency_mode=llm)
            st = classify(r["total"])
            print(f"{name:10} {buf:4} {str(llm):>4} {r['total']:8.2f} {st:>7}  {advice(st)}")


if __name__ == "__main__":
    import sys
    sys.exit(selftest() if "--selftest" in sys.argv else (table() or 0))
