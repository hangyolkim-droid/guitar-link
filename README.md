# guitar-link

A latency-first wireless guitar rig, and a dependency-free guitar amp sim.

Built around one measurement: **a guitar amplifier is a monitoring path.** You
hear your own attack, so anything past ~10 ms changes how you play. That single
constraint decides the whole design.

## The verdict on Bluetooth

It cannot replace a guitar cable. Best case (LE Audio / LC3, both ends
certified) is ~25–30 ms end to end; every commodity path is 40 ms or worse, and
the silent default fallback is ~170 ms.

End-to-end, including converters and the amp's input stage:

| Link | typical | vs cable |
|---|---|---|
| Cable | 0.8 ms | 1x |
| 2.4 GHz digital wireless | 5.1 ms | 6x |
| BT LE Audio (LC3) | 29.5 ms | 37x |
| BT aptX Low Latency | 39.5 ms | 49x |
| BT SBC (A2DP default) | 173.5 ms | 217x |

Two things follow, and both are in `DESIGN.md`:

- The 2.4 GHz systems are not Bluetooth — they are dedicated isochronous links,
  which is exactly why they hit 2–5 ms.
- The most compatible interface on a consumer Bluetooth speaker is its **3.5 mm
  aux jack**: present on essentially every one, and 0 ms.

## Quick start

    make
    ./amp_sim --selftest
    ./amp_sim --demo demo
    ./amp_sim --in demo/di.wav --out out.wav --preset mesa

Render your own DI:

    ./amp_sim --in di.wav --out out.wav --preset marshall --gain 0.7

Only flags you pass override the preset, so `--bass 0` really means zero.

    --preset fender|marshall|mesa   --gain 0..1
    --bass / --mid / --treble / --presence  dB
    --os 1|2|4                      --trim linear
    --reverb spring|plate|off       --rex / --rdecay
    --delay MS                      --dfb / --dmix
    --ir cab.wav                    --no-cab

## Signal chain

    input HP 80 Hz -> gain -> [4x OS: stage1 -> interstage HP -> stage2]
      -> tone stack -> cab IR -> reverb -> delay -> softclip -> DC block -> clamp

Oversampling wraps only the nonlinear stages; the tone stack is linear and
cannot alias, so it runs at base rate. Added dry latency is **0.667 ms** (32 base
samples). The cab is direct-form FIR, so it adds none; reverb and delay are
wet-only.

## Measured, not assumed

`./amp_sim --selftest` — 12 assertions:

| check | result |
|---|---|
| 4x alias suppression | 19.54 dB less energy at the 13 kHz alias bin |
| silence in -> silence out | peak 0.0, all presets |
| finite and bounded | peak <= 1.0 |
| DC offset | -0.00025 |
| tail decay | 0.0028 -> 0.0000 |
| render speed | 6.7x real time, single thread, ARM |

## Tooling (Python)

| file | purpose |
|---|---|
| `latency_budget.py` | the table above, no dependencies |
| `measure_latency.py` | real end-to-end latency by chirp cross-correlation |
| `latency_mode.py` | DAW-style buffer ladder + Low Latency Mode + indicator |
| `calibrate.py` | per-rig calibration store (host/device/rate/link) |

`measure_latency.py` and `calibrate.py` need numpy; the rest are stdlib only.

    python3 measure_latency.py --make-stimulus click.wav --sr 48000
    python3 measure_latency.py --ref click.wav --rec take.wav
    python3 calibrate.py --device "Scarlett Solo" --buffer 64 --takes t1.wav t2.wav t3.wav

Calibration is bound to `(host, device, sample rate, link)` and **not** to buffer
size, because latency is linear in buffer and extrapolates exactly. Two measured
points pin the real slope.

## Building on Termux

No CMake, so the usual Termux CMake/`PREFIX` failure cannot occur here.

    pkg install clang make python
    make && make test

## Limitations, stated plainly

- The cabinet is a **procedural placeholder**, not a measured impulse. It proves
  the signal path; it will not sound like a 4x12. Load a real IR with `--ir`.
- There is no real-time host adapter yet. `Engine::processBlock` is ready, but
  nothing drives it from a device clock (ALSA/JACK/JUCE).
- No license file yet — that is a deliberate open decision, not an oversight.
