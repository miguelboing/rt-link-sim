#!/usr/bin/env python3
"""Write the four channels the simulator ships with.

These are *designed*, not drawn at random -- each one is a named propagation
regime the schedulers are meant to be compared across, and the numbers below
are derived from the regime rather than tuned. `generate_channels.py` is the
other half of the pair: it synthesises arbitrary random chains for sensitivity
work, and nothing here depends on it except the archetype curve table and the
figure, which are shared so the two cannot drift.

    cyclic_fast_6     6 states, 3 curves, mean dwell 50.  The original
                      `channel_20m`, renamed: the band it was named after
                      stopped being part of the model when the noise floor
                      went dead, and its distinguishing feature is the cycle.
    cyclic_slow_8     8 states, 4 curves, mean dwell 100. The same mirrored
                      ring one rung taller and half the speed.
    good_moderate_2   2 states, oscillating Good/Moderate, 60% in Good.
    rare_excellent_3  3 states, mostly Poor and Moderate, with rare but long
                      Excellent openings.

Every channel is written as the two files `SigmoidChannel` reads:

    <name>/transition_matrix.kov   the chain, one row per state
    <name>/fsmc_states.json        one sigmoid decode curve per state
    <name>/fsmc_diagram.{png,pdf}  the drawing, unless --no-figure

The state count differs per channel on purpose, which is why
`SigmoidChannel` resolves it at run time from the files rather than from a
compile-time constant.

Usage:
    python build_channels.py                # write all four
    python build_channels.py --dry-run      # print, write nothing
    python build_channels.py --only rare_excellent_3
"""

import argparse
import json
import os
import sys

from generate_channels import (ARCHETYPES, CHANNEL_DIR, FIGURE_BASENAME,
                               draw_fsmc, stationary)

CURVES = {name: (slope, snr_50, sat) for name, slope, snr_50, sat in ARCHETYPES}


def one_way_ring(curve_names, p_self):
    """A one-way ring: state i holds with `p_self`, else steps to i+1 mod n.

    Mean dwell is 1/(1 - p_self) and a full lap is n times that. The ring is a
    circulant, so its stationary distribution is exactly uniform -- the time
    spent on a curve is the number of slots carrying it, which is what makes
    the mirrored layout below a statement about the *schedule* of conditions
    rather than about their frequency.
    """
    n = len(curve_names)
    matrix = [[0.0] * n for _ in range(n)]
    for i in range(n):
        matrix[i][i] = p_self
        matrix[i][(i + 1) % n] = 1.0 - p_self
    return matrix


def two_state(pi_first, dwell_first):
    """A two-state oscillator with a given split and dwell in the first state.

    Any two-state chain is reversible, so detailed balance fixes the return
    rate exactly: pi_0 p_01 = pi_1 p_10. Asking for 60% in the first state
    with a dwell of 50 there therefore pins the dwell in the second at
    1/p_10 = 33.3 -- the split and the two dwells are not independent, and
    only two of the three may be chosen.
    """
    p_01 = 1.0 / dwell_first
    p_10 = p_01 * pi_first / (1.0 - pi_first)
    return [[1.0 - p_01, p_01],
            [p_10, 1.0 - p_10]]


def birth_death(pi, dwell_0, dwell_1):
    """A 3-state birth-death chain: 0 <-> 1 <-> 2, no 0 <-> 2 hop.

    Reversible like the two-state case, so detailed balance (pi_i p_ij =
    pi_j p_ji) determines every downward rate from the upward one and the
    target distribution is hit exactly. The caller picks the occupancy `pi`,
    the dwell in state 0 and the dwell in state 1; the dwell in state 2 falls
    out of the balance equations.

    Barring the 0 <-> 2 hop is the physical claim: the channel climbs and
    falls through the middle rung, so the best state is reached from the
    middle and never straight from the worst.
    """
    p_01 = 1.0 / dwell_0
    p_10 = pi[0] * p_01 / pi[1]
    p_12 = 1.0 / dwell_1 - p_10
    if p_12 <= 0:
        raise ValueError(f"dwell_1={dwell_1} is longer than the return rate to state 0 "
                         f"allows (p_10={p_10:.6g}); shorten it or lower pi[0]")
    p_21 = pi[1] * p_12 / pi[2]
    return [[1.0 - p_01,  p_01,              0.0],
            [p_10,        1.0 - p_10 - p_12, p_12],
            [0.0,         p_21,              1.0 - p_21]]


# ── The four channels ────────────────────────────────────────────────────────
#
# `curves` lists one archetype name per state, so the state table and the
# chain are written from one source and cannot disagree on length. Index order
# is the chain's order, not a quality ranking: on the cyclic channels the
# sequence *is* the model.

CHANNELS = [
    {
        "name": "cyclic_fast_6",
        # The shipped channel, unchanged in every number -- only its name and
        # the comments move. Three curves over six slots, mirrored into a
        # triangle: the walk runs Good, Moderate, Poor, Poor, Moderate, Good
        # and wraps Good-to-Good, so it descends and climbs back without ever
        # jumping from worst to best. Collapsing the mirror to three states
        # would put exactly that jump at the wrap. The mirror also balances
        # the cycle: equal expected time in Good and in Poor.
        "curves": ["Good", "Moderate", "Poor", "Poor", "Moderate", "Good"],
        "build": lambda: one_way_ring(
            ["Good", "Moderate", "Poor", "Poor", "Moderate", "Good"], 0.98),
        "pi_target": [1 / 6] * 6,
        "note": "mean dwell 50 slots, full lap ~300",
    },
    {
        "name": "cyclic_slow_8",
        # The same construction one rung taller: four distinct curves need
        # eight slots to mirror (E G M P | P M G E), and the wrap is
        # Excellent-to-Excellent. Dwell 100 rather than 50, so the channel
        # moves at half the speed and a lap takes ~800 slots -- long enough
        # that a scheduler holding a stale estimate is wrong for longer.
        "curves": ["Excellent", "Good", "Moderate", "Poor",
                   "Poor", "Moderate", "Good", "Excellent"],
        "build": lambda: one_way_ring(
            ["Excellent", "Good", "Moderate", "Poor",
             "Poor", "Moderate", "Good", "Excellent"], 0.99),
        "pi_target": [1 / 8] * 8,
        "note": "mean dwell 100 slots, full lap ~800",
    },
    {
        "name": "good_moderate_2",
        # No deep fades at all: the link is either Good or Moderate and
        # spends 60% of its time Good. The baseline for "the channel varies
        # but never collapses" -- redundancy bought against a Poor state that
        # never arrives is wasted here, which is the point of including it.
        "curves": ["Good", "Moderate"],
        "build": lambda: two_state(pi_first=0.60, dwell_first=50.0),
        "pi_target": [0.60, 0.40],
        "note": "60% Good / 40% Moderate, dwell 50 and 33.3",
    },
    {
        "name": "rare_excellent_3",
        # The opposite regime: 95% of the time Poor or Moderate, with an
        # Excellent opening that arrives roughly once every 10k slots and
        # then holds for ~500. Chosen so the opening is rare per *visit* but
        # long once open -- a scheduler that notices it and spends it is
        # measurably better off than one that averages it away, which is the
        # comparison this channel exists for.
        "curves": ["Excellent", "Moderate", "Poor"],
        "build": lambda: birth_death(pi=(0.05, 0.40, 0.55),
                                     dwell_0=500.0, dwell_1=60.0),
        "pi_target": [0.05, 0.40, 0.55],
        "note": "5% Excellent / 40% Moderate / 55% Poor, Excellent dwell 500",
    },
]

# Frequencies are a *config* field, not a property of the channel, so nothing
# is written into the directories. These are the ones the printed snippet
# suggests: the first keeps the frequency the harness already uses, so
# renaming the shipped channel changes nothing else.
BASE_FREQ, FREQ_STEP = 14074000, 10000

# The stationary distribution is solved numerically and compared against the
# design target; anything above this is a bug in the construction, not a
# tolerance to be widened.
PI_TOLERANCE = 1e-12


def states_for(curve_names):
    """The fsmc_states.json table for a list of archetype names."""
    out = []
    for name in curve_names:
        slope, snr_50, sat = CURVES[name]
        out.append({"snr_50_db": snr_50, "slope": slope, "max_saturation": sat})
    return out


def write_channel(spec, freq, out_dir, draw):
    matrix = spec["build"]()
    curves = spec["curves"]
    states = states_for(curves)
    n = len(curves)
    pi = stationary(matrix)

    print(f"\n── {spec['name']}  ({n} states, {spec['note']})")
    print(f"   {'idx':<4} {'curve':<10} {'dwell':>7} {'pi':>7}   transition row")
    for s in range(n):
        dwell = 1.0 / (1.0 - matrix[s][s]) if matrix[s][s] < 1 else float("inf")
        row = "  ".join(f"{v:.4f}" for v in matrix[s])
        print(f"   {s:<4} {curves[s]:<10} {dwell:>7.1f} {pi[s]:>7.4f}   {row}")

    for s, row in enumerate(matrix):
        if abs(sum(row) - 1.0) > 1e-12:
            raise ValueError(f"{spec['name']} row {s} sums to {sum(row)!r}")

    # Every construction above is reversible or circulant, so the solved
    # stationary distribution has to land on the design target to numerical
    # precision. A drift here means the derivation is wrong, not that the
    # tolerance is tight.
    drift = max(abs(a - b) for a, b in zip(pi, spec["pi_target"]))
    if drift > PI_TOLERANCE:
        raise ValueError(f"{spec['name']}: stationary distribution is off its target "
                         f"by {drift:.3g} (got {pi}, wanted {spec['pi_target']})")

    if out_dir is None:
        return pi

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "transition_matrix.kov"), "w") as f:
        for row in matrix:
            # 9 significant digits rather than fixed decimals: the climb rate
            # out of Moderate on rare_excellent_3 is 2.5e-4 and a rounded zero
            # would cut the chain in two.
            f.write(", ".join(f"{v:.9g}" for v in row) + "\n")
    with open(os.path.join(out_dir, "fsmc_states.json"), "w") as f:
        f.write("[\n")
        f.write(",\n".join(
            f'    {{ "snr_50_db": {s["snr_50_db"]}, "slope": {s["slope"]},'
            f' "max_saturation": {s["max_saturation"]} }}' for s in states))
        f.write("\n]\n")

    if draw:
        try:
            outs = draw_fsmc(spec["name"], freq, matrix, curves, states, pi,
                             os.path.join(out_dir, FIGURE_BASENAME))
            print(f"   diagram: {outs}")
        except ImportError as exc:
            print(f"   diagram skipped ({exc}); --no-figure silences this",
                  file=sys.stderr)
    return pi


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", action="append", metavar="NAME",
                    help="write just this channel (repeatable)")
    ap.add_argument("--no-figure", action="store_true",
                    help="skip the per-channel state diagram")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    known = {c["name"] for c in CHANNELS}
    wanted = set(args.only) if args.only else known
    unknown = wanted - known
    if unknown:
        ap.error(f"unknown channel(s): {', '.join(sorted(unknown))}. "
                 f"Known: {', '.join(sorted(known))}")

    cfg = []
    for i, spec in enumerate(CHANNELS):
        freq = BASE_FREQ + i * FREQ_STEP
        if spec["name"] not in wanted:
            continue
        out_dir = None if args.dry_run else os.path.join(CHANNEL_DIR, spec["name"])
        write_channel(spec, freq, out_dir, draw=not args.no_figure)
        cfg.append({"type": "sigmoid", "frequency": freq, "name": spec["name"]})

    print("\n" + "=" * 70)
    if args.dry_run:
        print("dry run -- nothing written")
    else:
        print(f"wrote {len(cfg)} channels under {CHANNEL_DIR}/")
        print("note: *.json, *.png and *.pdf are gitignored, so fsmc_states.json and\n"
              "      the diagrams need `git add -f` to be committed.")

    print("\nconfig snippet -- a scheduler binds to a channel by `frequency`, so\n"
          "running on a different channel means moving both:\n")
    print(json.dumps({"channels": cfg}, indent=2))


if __name__ == "__main__":
    main()
