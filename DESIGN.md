# Guitar → wireless → amplifier

Goal: replace the guitar cable with a wireless link, with latency low enough that
the result behaves like an amplifier rather than a delayed recording of one.

## 1. The verdict up front

**Bluetooth cannot be the guitar link.** Not "needs tuning" — cannot. The best
Bluetooth path that exists (LE Audio / LC3, both ends certified, low-latency
configuration) lands around 25–30 ms end to end. Every commodity path is 40 ms
or worse, and the default fallback is ~170 ms.

A guitar amplifier is a monitoring path: you hear your own attack. Above roughly
10 ms the note arrives after your hand has already moved on, and players start
compensating by playing early. That is not a preference, it is what the ear does.

Run `python3 latency_budget.py` for the full table. Summary (typical, end to end
including converters and the amp's own input stage):

| Link                              | typ      | vs cable | usable as amp input |
|-----------------------------------|----------|----------|---------------------|
| Cable                             | 0.8 ms   | 1x       | yes, reference      |
| 2.4 GHz digital wireless          | 5.1 ms   | 6x       | **yes**             |
| BT LE Audio (LC3 unicast)         | 29.5 ms  | 37x      | marginal            |
| BT Classic aptX Low Latency       | 39.5 ms  | 49x      | no                  |
| BT Classic aptX Adaptive          | 68.5 ms  | 86x      | no                  |
| BT Classic SBC (default A2DP)     | 173.5 ms | 217x     | no                  |

The 2.4 GHz systems are **not Bluetooth**. They are dedicated isochronous radio
links (TI CC85xx-class silicon) with fixed, tiny buffers — that is exactly why
they hit 2–5 ms and Bluetooth cannot.

Also relevant: aptX Low Latency has been retired by Qualcomm in favour of aptX
Adaptive, so LL dongles are dwindling stock, and adaptive is *slower* unless both
ends are Snapdragon Sound. Apple still does not ship LE Audio on iPhone. There is
no forward-looking Bluetooth path to 10 ms.

## 2. Three architectures

### A. 2.4 GHz wireless cable replacement — recommended

Transmitter in the guitar jack, receiver into the amp. Nothing to build, nothing
to solder, 2–5 ms.

- Xvive U2 (~$150): 4 channels, rechargeable, ~5 ms claimed, ~5 h
- Line 6 Relay G10S (~$230): ~2.4 ms claimed, auto-pair, base receiver
- Boss WL-20L: ~2.3 ms claimed
- NUX B-2 / B-3 (~$90): budget option, ~5 ms claimed

All output instrument level and plug straight into a guitar input. This is the
answer to the question as asked.

### B. Bluetooth, if it is non-negotiable

Only sensible if the destination is a Bluetooth speaker or a Bluetooth-equipped
amp, i.e. practice and backing tracks, not riffing.

Chain: analog-in BT transmitter → matching BT receiver → 3.5 mm to 1/4" TS →
amp **FX return** (not the instrument input; see §4).

- TX: 1Mii ML300, Avantree TC419/Oasis Plus, or equivalent that explicitly lists
  aptX LL on the *transmit* side
- RX: matching unit from the same family that lists aptX LL on the *receive* side

Expect 35–45 ms. Verify it — do not trust the box. `measure_latency.py` exists
because the failure mode is silent: if the codec does not negotiate, you get SBC
at ~170 ms and it looks like "Bluetooth is just like that."

Roughly $70–120. Add a 1/4" TS mono adapter; a TRS plug is pointless here.

### C. Software amplifier on a host — where your code actually goes

Wired (or 2.4 GHz) input, amp sim in software, wired output to a powered FRFR
cab or the amp's FX return. This is the version where "write the software" is a
real project rather than a configuration exercise.

Latency budget: USB interface at 64 samples / 48 kHz ≈ 1.3 ms per buffer; a
well-behaved driver round trip is 5–8 ms; plus amp 0.5 ms. **6–10 ms total.**

Hardware, no soldering: any class-compliant interface — Focusrite Scarlett Solo
4th gen, M-Audio M-Track Solo, iRig HD 2. Laptop, not phone: Android's USB audio
path is typically 20–40 ms and will eat the entire budget. If it must be Android,
use the built-in headset/USB-C path with Oboe's MMAP/fast track and verify on the
actual device.

## 3. Software design (architecture C)

Target: 48 kHz, 64-sample blocks, one audio callback, no allocation.

Signal chain:

    input trim -> noise gate -> pre-EQ -> preamp (4x oversampled) ->
    tone stack -> cabinet IR -> limiter -> output trim

Rules that decide whether it sounds like an amp or like a plugin demo:

1. **Oversample the nonlinearity 4x minimum.** Clipping at 48 kHz folds
   harmonics back down as aliasing; that fizz is the single most common reason
   DIY amp sims sound cheap. Linear stages stay at base rate; only the
   waveshaper and its surrounding filters oversample.
2. **Real-time safety.** No malloc, locks, logging, file I/O, or unbounded
   branching in the callback. Preallocate everything; lock-free SPSC ring for
   parameter changes; ramp every parameter over ~10 ms to avoid zipper noise.
3. **Cabinet via partitioned convolution** at 48 kHz with a normalised IR.
   Sample-rate mismatch is a silent tone bug.
4. **Gain staging.** Aim for peaks at −12 dBFS. Instrument level is ~100 mV to
   1 V into 1 MΩ; line level is 1–2 V into ~10 kΩ. Getting this wrong is §4.2.
5. **Flush denormals** (or add a −120 dBFS noise floor) in feedback paths.
6. **Display measured latency**, not the configured buffer size. They differ.

If the transport is Bluetooth, software cannot fix the 40 ms — but it can delay
the backing track by the measured amount so at least the *music* stays aligned,
and it can refuse to start until the codec is confirmed low-latency.

## 4. Failure modes that eat these projects

1. **Silent codec fallback.** The box says aptX LL; the pair negotiated SBC.
   Indistinguishable from a bad design without measurement. Measure every pair.
2. **Line level into an instrument input.** A BT receiver outputs ~1–2 V into a
   low-impedance output; a guitar input expects ~100 mV into 1 MΩ. Result is
   hard clipping of the amp's first stage, which reads as "harsh Bluetooth," not
   as a level error. Use the FX return, or pad −12 to −20 dB.
3. **TRS confusion.** A guitar is unbalanced mono: TS. A TRS plug in a mono jack
   shorts ring to sleeve — harmless but buys nothing. Never feed phantom power
   into anything in this chain.
4. **2.4 GHz coexistence.** WiFi routers sit in the same band; dropouts are
   congestion, not a broken unit. Change channel, keep the receiver off the
   router.
5. **Ground loop.** USB-powered receiver on a laptop fed from a different outlet
   than the amp hums. Same power strip, or a ground-loop isolator.
6. **Buffer underrun.** At 64 samples you have 1.3 ms per callback. One missed
   deadline is an audible click; profile with the real chain loaded.

## 5. Tools in this folder

- `latency_budget.py` — the table above, no dependencies.
- `measure_latency.py` — real end-to-end latency by chirp cross-correlation.
  Generates the stimulus, measures, reports correlation confidence.

      python3 measure_latency.py --make-stimulus click.wav --sr 48000
      python3 measure_latency.py --ref click.wav --rec take.wav

  Self-test (`--selftest`) recovers 0, 2.5, 26, 171 and 300 ms exactly under
  −34 dBFS noise with confidence ≈ 70. Sample-accurate to 0.02 ms at 48 kHz.
  Take the median of five takes; the spread is your jitter, which is what your
  hands actually notice.

## 6. Decision: destination is a consumer Bluetooth speaker

Requirement: work with the broadest set of consumer speakers.
Consequence: **the two goals are anti-correlated.** A2DP picks the
*highest-priority codec the sink also advertises*, i.e. the worst common
denominator. Designing a transmitter for maximum compatibility means designing
it to fall back to SBC — the slowest option on the list.

Codec support across consumer speakers, order of magnitude:

| Codec            | Mandatory in A2DP | Consumer speaker support | Typical latency |
|------------------|-------------------|--------------------------|-----------------|
| SBC              | **yes**           | ~100%                    | 150–300 ms      |
| AAC              | no                | ~70–90%                  | 150–250 ms      |
| aptX             | no                | ~20–30%                  | 70–100 ms       |
| aptX HD          | no                | ~10%                     | 150–200 ms      |
| aptX Adaptive    | no                | ~15%, recent             | 50–80 ms        |
| aptX Low Latency | no                | ~5%, almost never in speakers | 35–45 ms   |
| LC3 / LE Audio   | no                | ~10%, recent             | 25–35 ms        |

So "most compatible Bluetooth format" = SBC = 150–300 ms = not an amplifier.
There is no codec choice that fixes this, because the choice is made by the
speaker, not by you.

**The most compatible interface on a consumer speaker is the 3.5 mm aux input.**
It is present on essentially every one of them and costs 0 ms. Design against
that and the codec question disappears.

### The rig

    guitar -> [2.4 GHz TX] ~~~ [RX] -> amp sim -> 3.5 mm cable -> speaker AUX
                  ~5 ms                           0 ms

Total ~6–10 ms including the amp sim. Untethered feel, zero Bluetooth in the
audio path, works with any speaker that has an aux jack.

Off-the-shelf, no soldering:

- 2.4 GHz guitar link (optional, removes the last cable): Xvive U2 ~$150
- Amp sim, pick one:
  - Fender Mustang Micro Plus ~$130 — guitar in, amp + IR, 3.5 mm out, battery
  - NUX Mighty Plug Pro ~$100 — same idea, more tweakable
  - laptop + audio interface + your own software (architecture C)
- 3.5 mm TRS–TRS stereo cable ~$8
- Speaker: any with an aux input

Speaker settings that matter: aux mode, **Bluetooth off** (so a phone cannot
steal the connection mid-session), volume ~70%, and any "bass boost" or
"surround" DSP defeated — those add their own latency and wreck guitar tone.
Set the amp sim output so the speaker never clips; start at 50%.

Laptop caveat: many laptops route a 3.5 mm combo headset jack through impedance
sensing and misdetect a line cable, giving you mono, tinny, or silent output. Use
the interface's own output or a cheap USB-C DAC instead of trusting the combo
jack.

If the speaker is Bluetooth-only with no aux jack, it cannot be used for live
playing at all. Use it for the backing track and nothing else.

If you insist on Bluetooth audio anyway: look for aptX LL explicitly in the
speaker's spec sheet (rare outside soundbars), otherwise accept 40 ms floor and
~170 ms reality, and confirm with `measure_latency.py` before trusting it.

## 7. Recommended path

1. Aux-in rig above. Play this week; ~$110 if you use the Mighty Plug, ~$260
   with the wireless guitar link.
2. Build architecture C on a laptop as the actual product — that is where your
   software effort compounds, and it is 6–10 ms.
3. Keep Bluetooth for what it is good at: backing track into the mix, control
   surface, preset sync. Not the guitar.

## 8. Latency modes and the live indicator

Two independent controls, matching how DAWs actually behave:

1. **I/O buffer ladder** — 32 / 64 / 128 / 256 / 512 samples. This is the only
   control that trades CPU for time.
2. **Low Latency Mode** — bypasses the stages that cost time rather than CPU:
   uniform-partitioned cab convolution drops to a short direct-form filter, the
   lookahead limiter is disabled, oversampling falls from 4x to 2x. Logic's
   equivalent; it is what you engage when the light goes amber and you cannot
   drop the buffer further.

`latency_mode.py` models both. At 48 kHz, USB, driver overhead 1.5 ms:

| mode     | buffer | LLM off | LMM on | light (off) |
|----------|--------|---------|--------|-------------|
| ultra    | 32     | 5.6 ms  | 3.1 ms | green       |
| low      | 64     | 7.6 ms  | 4.5 ms | green       |
| balanced | 128    | 11.6 ms | 7.1 ms | yellow      |
| safe     | 256    | 19.6 ms | 12.5 ms| yellow      |
| huge     | 512    | 35.6 ms | 23.1 ms| red         |

Note what the table shows: Low Latency Mode buys roughly one buffer step, which
is why it is a rescue switch and not a substitute for the ladder.

### The indicator contract

- **Feed it measured latency, never the configured buffer.** `measure_latency.py`
  gives the real number; the buffer setting is an input to the estimate, not the
  answer.
- **Median over a 9-sample window**, then classify. A single dropped buffer must
  not repaint the UI.
- **Hysteresis of 0.5 ms** at each boundary. Without it, a chain sitting at
  10.0 ms flickers green/yellow forever.
- **Thresholds:** green ≤ 10 ms, yellow 10–20 ms, red > 20 ms.
- **Show the delta.** If measured exceeds the estimate by more than ~3 ms,
  something invisible is in the path — OS buffer, a codec fallback, a hidden
  plugin. Surface that as its own warning; it is the single most useful
  diagnostic in the whole app.
- **Make red actionable.** A red light that only changes colour is decoration.
  It should offer the fix: switch to Low Latency Mode, drop one buffer step, or
  raise the buffer if the symptom is actually dropouts rather than delay.

`Indicator` in `latency_mode.py` implements the median + hysteresis logic; it is
~30 lines and ports directly to C++/Rust/JS.

## 9. Per-rig calibration

A DAW gets round-trip latency from the driver. This rig does not, so the number
must be measured once and then trusted. `calibrate.py` owns that.

### What a calibration is bound to

Fingerprint = `(host, device, sample rate, link)`, hashed to 16 hex chars.

Those four change latency unpredictably, so changing any of them invalidates the
profile and the app must show `uncalibrated` rather than a stale number.

**Buffer size is deliberately excluded.** Latency is linear in buffer size, so a
stored point extrapolates exactly; forcing a chirp re-measure on every buffer
change would make the feature unusable. Points are stored per buffer, so two
points recover the real slope empirically instead of trusting the model:

    64 samples  -> 7.60 ms
    128 samples -> 11.60 ms
    slope = 4.00 / 64 = 62.5 ms per 1000 samples
    256 -> 19.60 ms     512 -> 35.60 ms     (both exact, no re-measure)

The model fallback before a second point exists is 3 * buffer_ms + constant,
flagged `extrapolated` with a note to measure a second buffer and pin it.

### Acceptance gates

A confident number from a bad take is worse than no number. A calibration is
stored only if **all three** pass:

| gate       | limit   | catches                                  |
|------------|---------|------------------------------------------|
| confidence | >= 20   | clipped, noisy, or missing signal        |
| jitter     | <= 1.5  | broad instability across takes           |
| spread     | <= 3.0  | bimodal takes, e.g. 7.6 / 19.0 / 7.6     |

The spread gate exists because median + MAD is blind to a bimodal split: three
takes of 7.6 / 19.0 / 7.6 give MAD = 0, since the majority cluster wins and the
outlier vanishes. Peak-to-peak is what catches it. This was found by a failing
assertion, not by inspection.

### Re-verification

`reverify()` compares a fresh measurement against the prediction and reports
`stable` or `drift`. Tolerance is `max(2.0 ms, 12%)`. Drift is reported *and*
the new point stored, so the history shows when a rig changed. Points older than
30 days read `stale` — cables, firmware, and USB hubs change.

### CLI

    python3 calibrate.py --device "Scarlett Solo" --buffer 64 \
        --takes take1.wav take2.wav take3.wav

    python3 calibrate.py --device "Scarlett Solo" --buffer 256      # predict only
    python3 calibrate.py --list
    python3 calibrate.py --selftest                                 # 15 assertions

`--simulate 7.6` synthesises takes at a known latency, so the whole pipeline can
be proven end to end without hardware.

### Wiring into the app

    pred = calibrate.predict(store, rig, buffer)
    if pred["ms"] is None:            # uncalibrated -> prompt for a chirp run
    indicator.push(pred["ms"])        # median + hysteresis -> green/yellow/red

Status drives the UI, not just the number: `exact` (measured), `extrapolated`
(trusted model), `stale` (re-verify), `uncalibrated` (no estimate — do not show
a colour, ask for calibration).

## 10. The amp sim (src/)

Dependency-free C++17. `g++ -std=c++17 -O2 -o amp_sim src/main.cpp`, no JUCE.
1307 lines across `dsp.h`, `amp.h`, `main.cpp`.

### Signal chain

    input HP 80 Hz -> gain -> [4x OS: stage1 -> interstage HP -> stage2]
      -> tone stack (low shelf / mid / high shelf / presence)
      -> cabinet IR -> reverb -> delay -> softclip -> DC block -> clamp

Oversampling wraps *only* the nonlinear stages. Linear filters (tone stack) run
at base rate and cannot alias, so charging them 4x is waste.

Added dry latency: **0.667 ms** (32 base samples, the up+decimate FIR pair).
The cab is direct-form FIR, so it adds none; reverb and delay are wet-only.

### Voicings, honestly

| preset   | character                    | reverb        |
|----------|------------------------------|---------------|
| fender   | soft, asymmetric, scooped mid | spring 0.32  |
| marshall | harder knee, mid-forward      | **none**     |
| mesa     | two-stage cascade, tight low  | spring 0.24  |

Marshall heads ship with no reverb and none of the three has built-in delay.
Inventing a "Marshall delay" would have been a lie; the delay is a tape echo
available on every preset.

### Reverb

- **Spring** — a real spring is a dispersive line: highs travel faster, which is
  the "drip". Five series allpasses (1.1–2.9 ms, g 0.62–0.71) reproduce the chirp
  far better than any comb or plate model, then a 58 ms loop with damping.
- **Plate** — 8-line FDN, Hadamard mixing (normalised 1/sqrt8), per-line damping.
  Stable by construction for decay < 1.

### Delay

Tape: fractional delay line, 0.6 Hz wow + 6.5 Hz flutter, lowpass in the loop and
`tanh` saturation so repeats decay into mush rather than staying bright.

### Measured (selftest, 12 assertions, all passing)

| check | result |
|---|---|
| oversampler latency | 32.000 base samples (matches 0.667 ms) |
| 4x alias suppression | **19.54 dB** less energy at the 13 kHz alias bin |
| silence in -> silence out | peak 0.0 for all three presets |
| finite and bounded | peak <= 1.0, rms 0.36–0.87 |
| DC offset | -0.00025 |
| tail decay | early 0.0028 -> late 0.0000 |
| render speed | **6.7x real time**, single thread, this ARM box |

### Four bugs the tests caught

1. **Reconstruction filter normalisation.** `designLowpass` takes fc relative to
   the *sample rate*, not Nyquist. Using Nyquist put the filter at 38 kHz instead
   of 19 kHz and passed the 35 kHz harmonic straight through: alias suppression
   measured 0.83 dB.
2. **Block-rate smoothing.** The one-pole coefficient is per-sample but was
   advanced once per 64-sample block, making every parameter ramp 64x too slow.
   At 2 ms the drive was still ~0.006, so impulses were almost muted.
3. **Cab IR normalisation.** `Fir::setTaps` DC-normalises, which is right for an
   interpolation filter and wrong for a resonant cab IR. It buried the output
   ~12x (22 dB). Now energy-normalised with `normalizeDc=false`.
4. **DC blocker placement.** Blocking before the clipper cannot work: clipping an
   asymmetric wave re-creates the offset. Blocking after it is correct, but the
   highpass then overshoots on sharp edges (peak 1.4), so the order is
   clip -> block -> clamp, with Q=0.5 to stop the overshoot feeding the clamp.

### Limitations, stated plainly

- The cab is a **procedural placeholder**, not a measured impulse. It proves the
  path; it will not sound like a 4x12. Load a real IR with `--ir cab.wav`.
- No real-time host adapter yet: `Engine::processBlock` is ready, but there is no
  ALSA/JACK/JUCE wrapper driving it from a device clock.
- 129-tap FIR at 4x costs ~1032 MAC per sample. Fine here; on a phone, use `--os 2`.
