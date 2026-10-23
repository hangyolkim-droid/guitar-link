# Bluetooth at its floor

The thesis: LE Audio is the fastest Bluetooth that exists for this job. This is
the latency budget, the four knobs that control it, and the purchasable hardware
that exposes them.

Conclusion up front: the floor is about **17–20 ms before the speaker**, which is
amber on the indicator in `latency_mode.py`, never green. Reaching even that
requires controlling the presentation delay, which is a sink-side policy rather
than a codec property.

## The stack that is actually fastest

| choice | fastest option | why |
|---|---|---|
| profile | LE Audio (BT 5.2+) | only path with isochronous transport |
| codec | LC3 | mandatory for LE Audio, 7.5 or 10 ms frames |
| frame interval | **7.5 ms** | 10 ms adds 2.5 ms of framing delay |
| topology | **unicast (CIS)** | feedback and a negotiable presentation delay |
| presentation delay | as low as the link tolerates | the dominant term, and it is policy |
| sink DSP | none | crossover/limiter adds 0–20 ms after the radio |

The counter-intuitive part: **Auracast is newer and slower.** It is a broadcast
topology with no per-sink feedback, so sources set a conservative presentation
delay. One measured report put a consumer Auracast transmitter at 80–100 ms.
"Newest" and "fastest" are different axes.

## Budget

| stage | ms | basis |
|---|---|---|
| ADC | ~1 | converter |
| LC3 encode + decode | ~10 | 12.5 ms algorithmic delay at 10 ms frames; ~10 at 7.5 |
| transport, one CIS interval | ~7.5 | frame-aligned |
| presentation / jitter buffer | 5–15 | **configurable, the lever** |
| DAC | ~1 | converter |
| sink DSP | 0–20 | unpublished |
| **total** | **~17–20 best case, 25–50 with a consumer speaker** | |

Compare: aptX LL ~40 ms, SBC 150–300 ms, a 2.4 GHz radio ~5 ms, a cable ~0.8 ms.

## Hardware that exposes the knobs

**FlooGoo FMA121** — Bluetooth 5.4, Class 1, dual-mode. Has both USB-C and
**3.5 mm inputs**, supports **unicast and Auracast**, and runs as transmitter or
receiver depending on firmware. Its FlooCast app **displays the active codec**
during streaming and configures low-latency modes, including "Auracast with
configurable latency". Officially Auracast-qualified.

Codec visibility is the point. Without it you cannot tell whether you got LC3 at
7.5 ms or fell back to something slower.

### Build A — fewest parts

    guitar -> modeller -> 3.5 mm -> FMA121 (unicast TX) ~~~ LE Audio
           -> LE Audio speaker (aux not used)

Speakers documented as Auracast/LE Audio capable: JBL Partybox Stage 320,
JBL Xtreme 5, JBL Go 4, Sony ULT Tower 7. Their presentation delay and DSP are
fixed and unpublished, so treat the speaker as the unknown.

### Build B — maximum control, any speaker

    guitar -> modeller -> 3.5 mm -> FMA121 (unicast TX) ~~~ LE Audio
           -> FMA121 (receiver firmware, USB audio out)
           -> USB-C DAC -> 3.5 mm -> any powered speaker

The receiver presents as a USB audio device, so a DAC is required to get back to
analogue. That adds a hop, but it makes both ends configurable and both ends
visible, and the speaker stops mattering.

## What it takes, in order

1. Force **unicast**, not broadcast.
2. Set the frame interval to **7.5 ms** if the firmware exposes it.
3. Minimise **presentation delay** and accept the occasional dropout; that trade
   is the whole point.
4. Verify the negotiated codec in FlooCast before believing any number.
5. Measure the finished chain with `measure_latency.py`. Vendor claims do not
   survive contact with a speaker's DSP.

## Verdict

Best Bluetooth lands around 17–20 ms before the speaker. That is roughly double
the "feels like a cable" threshold, and a consumer speaker can double it again.

So the honest finding is not "Bluetooth cannot do this". It is: **the best
Bluetooth reaches amber, and getting there costs codec visibility, unicast
topology, and control of a delay parameter that almost no consumer product
exposes.**
