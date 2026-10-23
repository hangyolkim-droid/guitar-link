# Product: guitar → Bluetooth speaker dongle

A 1/4" plug that goes into the guitar, runs an amp sim, and streams to a
low-latency Bluetooth speaker. The value is not the radio — that is a commodity
module — it is the measured compatibility data and the codec diagnostics.

## Positioning

**It is:** a way to play through the speaker you already own, with no receiver
box and no cable.

**It is not:** a stage or gigging solution. A $90 2.4 GHz system is 5 ms with no
caveats. Anyone who needs determinism should buy that instead, and the listing
should say so.

## The three caveat strings

Print the same idea at three levels of detail; never an adjective alone.

- **Box:** Low-latency Bluetooth speaker required
- **Listing:** Works with LE Audio, aptX Low Latency, or aptX Adaptive speakers.
  SBC-only speakers will lag.
- **Manual:** the line above, plus the measured per-speaker table.

Do not write "LE3". The standard is LE Audio, negotiated on Bluetooth 5.2 or
newer, using LC3. aptX LL is the only one of the three that genuinely suits a
guitar; aptX Adaptive is mostly 50–80 ms and should not be presented as equal.

## Claims we are allowed to make

Only measured statements:

- "Verified N ms on these speakers" — from `measure_latency.py`, median of five.
- "Adds X ms before the speaker" — from the DSP budget, if measured on hardware.
- Never "low latency" unqualified, and never a number the speaker's DSP controls
  unless it was measured with that speaker.

## The differentiator: codec diagnosis

On connect, read the negotiated codec.

| negotiated | behaviour |
|---|---|
| LC3 / aptX LL | connect, show measured latency estimate |
| aptX Adaptive | connect, warn that timing feels late |
| SBC | do not silently stream; show "this speaker will lag ~170 ms" |

A product that tells the buyer their speaker is the problem converts a return
into a review. No competitor in this category does this.

## Compatibility table format

One row per speaker, measured, dated:

| speaker | profile | measured ms | verdict |
|---|---|---|---|
| (model) | LC3 7.5 ms | (number) | green / amber / red |

Verdict thresholds are the ones in `latency_mode.py`: ≤10 green, 10–20 amber,
>20 red. Expect most rows to be amber. Publishing that honestly is the moat.

## Economics, order of magnitude

BOM at 1k units: BT module $8–15, codec/ADC $3–5, battery and charging $4–6,
jack and enclosure $3–5, assembly $3–5 → **$25–35**. Retail $79–99. Certification
(SIG, FCC, CE) is mid-four to low-five figures before tooling, so build on a
module rather than a custom PCB until volume justifies it.

## Risks

- **The claim is hostage to the sink.** Presentation delay and speaker DSP are
  not ours. Mitigation: publish measurements, never a blanket latency claim.
- **Returns.** A caveat buyers cannot evaluate drives returns. Mitigation: the
  codec check above.
- **Copyable hardware.** Mitigation: the measured corpus and firmware quality.
- **Market size.** The buyer must own an LE Audio speaker and want wireless
  guitar. That intersection is small. Price and volume should assume small.

## Validate before building

Buy an FMA121, one JBL and one Sony LE Audio speaker (about $300), and measure
the finished chain. If 25–35 ms is unacceptable while playing, stop — that is the
whole product. If it is acceptable, those are the first two rows of the table.
