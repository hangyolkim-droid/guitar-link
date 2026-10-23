# Testing the link

Anyone with an LE Audio transmitter and an LE Audio speaker can produce a
useful data point in about ten minutes.

## You need

- A transmitter with an analogue input: FlooGoo FMA120/FMA121 or equivalent.
- A speaker that supports LE Audio (Bluetooth 5.2+, LC3). JBL Partybox Stage 320,
  JBL Xtreme 5, JBL Go 4 and Sony ULT Tower 7 are known to advertise it.
- A way to record the speaker: a microphone at a fixed distance, or the speaker's
  own line output if it has one.
- Python 3 with numpy.

## Protocol

1. Pair the transmitter and speaker. Force **unicast**, not Auracast broadcast.
   Broadcast implementations are consistently slower.
2. Generate the stimulus:

       python3 measure_latency.py --make-stimulus click.wav --sr 48000

3. Play `click.wav` into the transmitter and record the speaker at the same
   time, keeping the file's original timing.
4. Measure:

       python3 measure_latency.py --ref click.wav --rec take.wav

5. Repeat five times. Report the **median**, not the best take.

## What to send back

| field | example |
|---|---|
| speaker | JBL Xtreme 5 |
| profile | LC3, unicast, 7.5 ms if known |
| median ms | 31.4 |
| felt late? | slightly / no / yes |
| notes | auto-connected to phone, had to disable |

## Rules

- Report the median and the spread. The spread is jitter, and jitter is what
  players actually feel.
- If the speaker falls back to SBC you will see 150–300 ms. Say so rather than
  reporting it as a failure of the link.
- Note the speaker's own DSP setting if it has a "party" or "bass boost" mode,
  and whether turning it off changed anything.
- Do not report a number from a phone-to-speaker test as a guitar number; the
  phone's audio stack is in the path.
