#!/usr/bin/env python3
"""Guitar link latency budget.

Every number is end-to-end guitar -> amplifier speaker input, in milliseconds.
"link" is the vendor-spec radio figure; converters are the ADC/DAC stages that
the spec usually omits, and "amp" is the amplifier's own input->power-amp delay.
No dependencies.
"""

CHAINS = [
    # name, link(min, typ, max), converters, amp, note
    ("Cable (reference)",              (0.0,   0.0,   0.0),   0.3, 0.5, "passive copper"),
    ("2.4 GHz digital wireless",       (2.3,   4.0,   5.5),   0.6, 0.5, "Xvive U2 / Boss WL / Line 6 class"),
    ("BT LE Audio, LC3 unicast",       (20.0,  26.0,  35.0),  3.0, 0.5, "BT 5.2+ both ends, rare as analog-in dongles"),
    ("BT Classic, aptX Low Latency",   (32.0,  36.0,  40.0),  3.0, 0.5, "codec retired by Qualcomm; stock is finite"),
    ("BT Classic, aptX Adaptive",      (50.0,  65.0,  80.0),  3.0, 0.5, "~40 ms only on Snapdragon Sound pairs"),
    ("BT Classic, SBC (A2DP default)", (100.0, 170.0, 300.0), 3.0, 0.5, "what you get when codec negotiation fails"),
]

BANDS = [
    (10.0, "feels like a cable"),
    (20.0, "playable, but you hear the delay"),
    (30.0, "timing suffers, practice only"),
    (1e9,  "not usable as an instrument amplifier"),
]


def verdict(ms):
    for limit, text in BANDS:
        if ms <= limit:
            return text
    return BANDS[-1][1]


def main():
    hdr = f"{'chain':34} {'min':>6} {'typ':>6} {'max':>6}  {'verdict (typ)':38} note"
    print(hdr)
    print("-" * len(hdr))
    rows = []
    for name, (lo, ty, hi), conv, amp, note in CHAINS:
        lo_t, ty_t, hi_t = lo + conv + amp, ty + conv + amp, hi + conv + amp
        rows.append((name, ty_t))
        print(f"{name:34} {lo_t:6.1f} {ty_t:6.1f} {hi_t:6.1f}  {verdict(ty_t):38} {note}")

    print()
    ref = rows[0][1]
    for name, ty in rows[1:]:
        print(f"{name:34} {ty / ref:6.1f}x a cable")

    print()
    print("Thresholds used (monitoring your own playing):")
    print("  <=10 ms  transparent | 10-20 ms audible | 20-30 ms fight the instrument | >30 ms unusable")
    print("  <10 ms is the target for a cable replacement; 40 ms is roughly a 13 m walk away from the cab.")


if __name__ == "__main__":
    main()
