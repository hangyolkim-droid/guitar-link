# Build: guitar → any powered speaker, ~6 ms plus the speaker's own DSP

A purchasable-parts build with no soldering. The radio is 2.4 GHz, not Bluetooth,
because Bluetooth's floor is ~40 ms and its default is ~170 ms. The 6 ms figure
covers the radio and the modeller only; the speaker adds an unpublished amount of
its own DSP, so the finished chain has to be measured.

## Chain

    guitar ──[1/4" TX]~~~2.4 GHz~~~[RX 1/4"]──▶ [modeller] ──3.5 mm TRS──▶ [speaker AUX]

The transmitter is the only thing on the guitar. Everything else sits on the
floor.

## Parts

| part | requirement | example | note |
|---|---|---|---|
| radio | TX plugs into the guitar, RX has a 1/4" plug, ~5 ms | Xvive U2 | 5 ms and 1/4" connections are on the spec sheet |
| amp sim | 1/4" instrument input, **3.5 mm headphone output**, cab IR | Valeton GP-100 | 1/8" (3.5 mm) headphone output is stated by Valeton |
| cable | 3.5 mm TRS **stereo**, 1–2 m | any | stereo, not a mono TS patch cable |
| power | 9 V DC for the modeller | supplied with the GP-100 | or a USB bank with a 9 V trigger cable |
| speaker | aux input | anything | nearly every powered speaker has one |

Approximate cost: 300–350 USD. Cheaper radio (NUX B-2 Plus, ~90) lands near 270.
Prices move; check before buying.

## Assembly

1. Charge the transmitter and receiver.
2. Power both, set them to the same channel.
3. Plug the transmitter into the guitar.
4. Plug the receiver into the modeller's instrument input. If it dangles, use a
   short 1/4" patch cable and velcro the receiver to the board.
5. Run the 3.5 mm TRS cable from the modeller's headphone output to the
   speaker's aux input.
6. Power the modeller. Use the same power strip as the speaker if both are
   mains-powered; a ground loop across two outlets hums.
7. On the speaker: select aux, and **turn its Bluetooth off** so a phone cannot
   take it mid-session.

## Settings

- Modeller: one amp model and one cab IR. Start with output around half.
- Speaker: about 70 percent, bass boost off, any "surround" or "party" mode off.
- Radio: pick a channel away from the room's WiFi access point.

## Latency budget

| stage | ms | basis |
|---|---|---|
| ADC + encode + radio + decode + DAC | ~5 | manufacturer figure for the radio |
| modeller | 1–3 | estimate |
| cable | 0 | copper |
| speaker DSP | **0–20, unknown** | not published by any speaker maker |
| total | **6–28** | dominated by the unknown term |

The speaker is the wildcard. Consumer speakers run their own DSP — crossover,
limiter, bass shaping — after the analogue input, and it is never in the spec
sheet. A speaker with a heavy "party" mode can add more delay than the radio.

## Measure it rather than trusting the table

The harness in this repo measures the finished chain end to end:

    python3 measure_latency.py --make-stimulus click.wav --sr 48000

Play `click.wav` into the transmitter and record the speaker with a microphone,
or better, take the speaker's line/headphone output if it has one. Then:

    python3 measure_latency.py --ref click.wav --rec take.wav

Take the median of five. If it reads above 20 ms, suspect the speaker's DSP
before you suspect the radio.

## Failure modes

- **Speaker auto-connects to a phone.** Bluetooth off, aux selected.
- **Ground loop hum.** Modeller and speaker on different outlets. Same strip, or
  a ground-loop isolator.
- **Overdriving the aux input.** A headphone output is hotter than line level.
  Start at half and raise until it clips, then back off.
- **Mono cable into a stereo aux.** Use TRS stereo; a TS cable shorts a channel.
- **2.4 GHz congestion.** Dropouts mean interference, not a faulty unit. Change
  channel, move the receiver off the router.
- **Receiver snapping off.** It is a 1/4" plug hanging out of a jack. Patch cable
  and velcro.

## Why not Bluetooth

A2DP negotiates the worst codec both ends support: SBC at 150–300 ms, aptX LL at
~40 ms (retired by Qualcomm), LE Audio at 20–30 ms if both ends support it, which
consumer speakers rarely do. All of those are at or past the point where you hear
your own attack late. The 2.4 GHz link is a dedicated isochronous radio with
fixed small buffers, which is why it lands at 5 ms.
