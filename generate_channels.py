#!/usr/bin/env python3
"""Synthesise C independent FSMC channels for the multi-channel experiments.

Each generated channel is a directory under `physical_channels/` holding the
two files `SigmoidChannel` reads at construction:

    <name>/transition_matrix.kov   the chain, one row per state
    <name>/fsmc_states.json        one sigmoid decode curve per state

and, unless --no-figure is given, a drawing of what was written:

    <name>/fsmc_diagram.{png,pdf}  the chain as a labelled state diagram

Curves are never invented: every state is one of the four archetypes below,
taken verbatim. What varies between channels is the transition matrix and
the number of states, drawn uniformly from 2..max_n_states, so a channel may
resolve the band coarsely or finely. SigmoidChannel reads the state count
from the files at construction, so a batch of mixed lengths loads as-is.

For the four designed channels the simulator ships with, see
`build_channels.py` -- this script is for random sensitivity work.

The state index is the quality rank -- state 0 is the best, state n-1 the
worst -- and the generated matrices bias transitions toward adjacent ranks.
A chain that hops straight from best to worst is the one transition a real
HF channel does not make, which is the same reason `cyclic_fast_6` lays its
six slots out as a mirrored triangle.

This script only synthesises the chain and copies the curve parameters. It
computes no decode probabilities and knows nothing about transmit power or
the link budget -- that model lives in `SigmoidChannel` and stays there.

Usage:
    python generate_channels.py <n_channels> [options]

    python generate_channels.py 4
    python generate_channels.py 8 --max-n-states 6 --seed 42
    python generate_channels.py 4 --no-figure
    python generate_channels.py 3 --dry-run
"""

import argparse
import json
import math
import os
import random
import re
import sys

# ── Archetype curves ─────────────────────────────────────────────────────────
#
# (name, slope, snr_50_db, max_saturation), ordered best to worst. These are
# the rungs the shipped channels are built from (build_channels.py imports
# this table), so a generated channel sits on the same scale as they do.
ARCHETYPES = [
    ("Excellent", 0.5,  5.0, 0.98),
    ("Good",      0.4, 10.0, 0.95),
    ("Moderate",  0.3, 15.0, 0.85),
    ("Poor",      0.2, 20.0, 0.70),
]

CHANNEL_DIR = "physical_channels"
# The shipped channels (see build_channels.py) and the two source directories
# that live alongside them. Writing a generated channel over any of these would
# destroy data the repo depends on.
RESERVED = {"cyclic_fast_6", "cyclic_slow_8", "good_moderate_2", "rare_excellent_3",
            "sigmoid_channel", "replay_channel"}


def build_states(n_states):
    """Pick `n_states` archetypes spanning best..worst.

    With n_states == 4 this is exactly the four archetypes, and more repeats
    rungs. Fewer takes a *contiguous* window from the centre of the band, so
    n_states == 2 gives Good and Moderate. Interpolating the rank instead
    would take a spread-out subset, which at n_states == 2 is Excellent and
    Poor -- the two extremes with no rung between them, making every
    transition in the chain the best-to-worst hop that the locality bias
    below exists to suppress.

    Either way the sequence stays monotone in quality, which is what makes
    "adjacent" meaningful below.
    """
    if n_states < len(ARCHETYPES):
        start = (len(ARCHETYPES) - n_states) // 2
        picks = list(range(start, start + n_states))
    else:
        picks = [round(i * (len(ARCHETYPES) - 1) / (n_states - 1)) for i in range(n_states)]

    states, names = [], []
    for idx in picks:
        name, slope, snr_50, sat = ARCHETYPES[idx]
        states.append({"snr_50_db": snr_50, "slope": slope, "max_saturation": sat})
        names.append(name)
    return states, names


def build_transition_matrix(n_states, rng, dwell_min, dwell_max, locality, concentration):
    """A random, persistent, locality-biased chain.

    Row i is built in two steps:

      self-loop   p_ii = 1 - 1/D_i,  D_i ~ U(dwell_min, dwell_max)
                  a geometric holding time with mean D_i, drawn per state, so
                  a channel can linger in one rung and flicker through another.

      leaving     the remaining 1 - p_ii is split by a Dirichlet draw of
      mass        uniform concentration `c`, weighted by the locality profile,

                      l_ij = exp(-|i - j| / locality)
                      (p_ij)_{j != i} = l_ij * Gamma(c, 1), summing to 1 - p_ii

                  so the split tracks the locality profile and `c` sets how
                  far an individual channel strays from it.

                  The locality has to enter as a *weight*, not as the
                  concentration: folding it in as alpha_ij = c * l_ij drops
                  the shape parameter below 1 at every rank distance past one
                  (at c = 5, locality = 1 it runs 0.68 at d = 2 down to 0.03
                  at d = 5), and a Gamma draw with alpha << 1 is usually near
                  zero but occasionally order-1. The locality bias then holds
                  only in expectation: over 20k draws at n = 6 the share of
                  the worst rung's exit mass landing straight on the best rung
                  had a median of 0.0% against an intended 1.2%, but a 99th
                  percentile of 31% and a maximum of 89% -- some channels came
                  out with the opposite end of the ladder as the worst state's
                  likeliest destination. Weighting a uniform Dirichlet keeps
                  the same draws within [0, 7.9%], median 1.1%.

    Every off-diagonal stays strictly positive, so each chain is irreducible
    and aperiodic: a unique stationary distribution exists and the power
    iteration in SigmoidChannel converges.
    """
    matrix = []
    for i in range(n_states):
        self_loop = 1.0 - 1.0 / rng.uniform(dwell_min, dwell_max)

        # Dirichlet(c) via normalised Gamma(c, 1) draws, scaled by locality.
        draws = []
        for j in range(n_states):
            if j == i:
                draws.append(0.0)
                continue
            locality_weight = math.exp(-abs(i - j) / locality)
            draws.append(rng.gammavariate(concentration, 1.0) * locality_weight)

        total = sum(draws)
        row = [d / total * (1.0 - self_loop) for d in draws]
        row[i] = self_loop
        matrix.append(row)
    return matrix


def stationary(matrix):
    """Long-run state distribution of the generated chain, by power iteration.

    Reported so you can see what came out -- a chain that sits 60% of the time
    in the worst rung is worth noticing before a sweep runs on it. This is a
    property of the matrix being written, not a model of the radio.
    """
    n = len(matrix)
    pi = [1.0 / n] * n
    for _ in range(100000):
        nxt = [0.0] * n
        for i in range(n):
            rs = sum(matrix[i])
            if rs <= 0:
                continue
            for j in range(n):
                nxt[j] += pi[i] * matrix[i][j] / rs
        delta = sum(abs(nxt[j] - pi[j]) for j in range(n))
        pi = nxt
        if delta < 1e-15:
            break
    return pi

# ── State diagram ────────────────────────────────────────────────────────────
#
# One figure per generated channel: the chain as a directed graph, plus the
# per-state table of the curve parameters that were copied into
# fsmc_states.json. It draws only what the two written files contain -- the
# archetype *name* of each state, not its decode curve, since evaluating a
# curve means knowing the link budget, which is SigmoidChannel's business and
# stays there (see the module docstring).
#
# Figure style, the formats written and the usetex escaping all come from the
# harness, so these figures match the sweep's: run_simulation sets the rcParams
# at import and owns PLOT_FORMATS.

FIGURE_BASENAME = "fsmc_diagram"

# Node fill is the quality *rank* of the curve the state holds, which is
# ordinal, so it gets a single hue dark (best) to light (worst) rather than a
# categorical set of hues. Keyed on the curve and not on the state index: on a
# mirrored ring the index is a position in the cycle, so index-keyed shading
# would paint the Excellent slot at the end of the ring as the worst one. The
# same curve therefore has the same fill in every channel's diagram. Identity
# is never carried by color alone: every node is labelled with its index and
# its archetype name, and the table repeats both.
NODE_CMAP = "Blues"
NODE_SHADE_MAX = 0.80   # darkest step used, for the best curve
NODE_SHADE_MIN = 0.22   # lightest, for the worst
INK_ON_DARK = 0.55      # above this shade the label is set in white
EDGE_COLOR = "#4c4c4c"
ARC_RAD = 0.14          # curvature per step of rank distance. The i->j and j->i
                        # arcs bow to opposite sides so neither hides the other,
                        # and scaling with distance fans the long chords out of
                        # the middle, where otherwise every one of them crosses
                        # and their labels land on top of each other.
LOOP_RAD = -1.6         # self-loop curvature; negative bulges away from centre


def _fmt_prob(p):
    """A transition probability as it goes on an edge label.

    Far-rank transitions can land near 1e-8 and they are load-bearing -- they
    are what keeps the chain irreducible -- so they must not print as `0.000`.
    """
    if p >= 5e-4:
        return f"{p:.3f}"
    return f"{p:.0e}".replace("e-0", "e-")


def _unit(v):
    norm = math.hypot(*v)
    return (v[0] / norm, v[1] / norm)


def _rotate(v, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c)


def _curve_shade(curve_name, index, n_states):
    """Position on the fill ramp for one state, from the curve it holds.

    Ends of the ramp are reserved: pure white has no edge against the page and
    the darkest step leaves no contrast for a label. A curve that is not one
    of the archetypes (a hand-written channel, say) has no rank to use, so it
    falls back to its position in the chain.
    """
    names = [a[0] for a in ARCHETYPES]
    if curve_name in names:
        rank = names.index(curve_name) / max(1, len(names) - 1)
    else:
        rank = index / max(1, n_states - 1)
    return NODE_SHADE_MAX - (NODE_SHADE_MAX - NODE_SHADE_MIN) * rank


def _bezier(p1, c, p2, t):
    """Point at parameter `t` of the quadratic Bezier matplotlib's arc3 draws."""
    u = 1.0 - t
    return (u * u * p1[0] + 2 * u * t * c[0] + t * t * p2[0],
            u * u * p1[1] + 2 * u * t * c[1] + t * t * p2[1])


def _clear_of_nodes(lx, ly, pos, r, step=0.05, limit=12):
    """Push an edge label radially off whichever node it landed on.

    An arc can pass close to a node it has nothing to do with, and a
    probability printed across a state's name is worse than one a little off
    its own arc.
    """
    for _ in range(limit):
        hit = next(((cx, cy) for cx, cy in pos
                    if math.hypot(lx - cx, ly - cy) < r + 0.06), None)
        if hit is None:
            break
        away = _unit((lx - hit[0], ly - hit[1]) if (lx, ly) != hit else (1.0, 0.0))
        lx, ly = lx + step * away[0], ly + step * away[1]
    return lx, ly


# Half-extents of a probability label in data units -- a number like "0.003"
# is much wider than it is tall, so labels that clear each other horizontally
# can still be a long way from colliding vertically, and the separation test
# has to be elliptical rather than a plain radius.
LABEL_HALF_W = 0.140
LABEL_HALF_H = 0.052


def _relax_labels(labels, pos, r, iters=400, spring=0.015):
    """Spread overlapping edge labels apart, in place.

    Placing each label a quarter of the way along its own arc keeps the
    numbers beside the state they leave, but two arcs departing at similar
    angles still put their labels on top of each other. This pushes
    overlapping pairs apart, re-clears them off the nodes each round, and
    pulls each one gently back toward where its arc actually runs, so a label
    never drifts so far that it reads as belonging to a different edge.
    """
    home = [(x, y) for x, y, _ in labels]
    for _ in range(iters):
        moved = False
        for a in range(len(labels)):
            for b in range(a + 1, len(labels)):
                dx = labels[b][0] - labels[a][0]
                dy = labels[b][1] - labels[a][1]
                nx, ny = dx / LABEL_HALF_W, dy / LABEL_HALF_H
                d = math.hypot(nx, ny)
                if d >= 2.0:
                    continue
                moved = True
                if d < 1e-9:
                    nx, ny, d = 1.0, 0.0, 1.0
                push = (2.0 - d) / 2.0 * 0.5
                ux, uy = nx / d * LABEL_HALF_W, ny / d * LABEL_HALF_H
                labels[a][0] -= push * ux
                labels[a][1] -= push * uy
                labels[b][0] += push * ux
                labels[b][1] += push * uy
        for k, (hx, hy) in enumerate(home):
            labels[k][0] += spring * (hx - labels[k][0])
            labels[k][1] += spring * (hy - labels[k][1])
            labels[k][0], labels[k][1] = _clear_of_nodes(
                labels[k][0], labels[k][1], pos, r, step=0.03, limit=4)
        if not moved:
            break


def draw_fsmc(name, matrix, archetypes, states, pi, path_base):
    """Write the state diagram of one generated channel. Returns the paths."""
    import run_simulation as rs          # harness rcParams, PLOT_FORMATS, tex_safe
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle, FancyArrowPatch

    n = len(matrix)
    # Clockwise from the top, so reading the ring left to right follows the
    # quality rank the state index already encodes.
    pos = [(math.cos(math.radians(90 - i * 360.0 / n)),
            math.sin(math.radians(90 - i * 360.0 / n))) for i in range(n)]
    # Nodes must not touch: cap the radius at a third of the gap between
    # neighbours, which is what shrinks them as the state count grows.
    r = min(0.30, 0.35 * 2 * math.sin(math.pi / n))

    cmap = plt.get_cmap(NODE_CMAP)
    shades = [_curve_shade(name, i, n) for i, name in enumerate(archetypes)]
    fills = [cmap(s) for s in shades]

    fig = plt.figure(figsize=(11.5, 6.4))
    gs = fig.add_gridspec(1, 2, width_ratios=[1.0, 1.1], wspace=0.0)
    ax = fig.add_subplot(gs[0, 0])
    ax.set_xlim(-1.78, 1.78)
    ax.set_ylim(-1.78, 1.78)
    ax.set_aspect("equal")
    ax.axis("off")

    labels = []
    for i in range(n):
        for j in range(n):
            p = matrix[i][j]
            # A transition the matrix forbids is not drawn at all. The shipped
            # channels are full of structural zeros -- a one-way ring has n-2
            # per row, and rare_excellent_3 bars the Excellent-Poor hop on
            # purpose -- and drawing them would show the chain as fully
            # connected, which is the opposite of what those zeros say.
            if p <= 0.0:
                continue

            # Width is a redundant encoding of the number already printed, so
            # the shape of the chain reads before any label does.
            style = dict(color=EDGE_COLOR, linewidth=0.7 + 2.1 * p,
                         alpha=0.35 + 0.6 * math.sqrt(p), zorder=1)
            if i == j:
                out = _unit(pos[i])
                a1 = tuple(pos[i][k] + r * _rotate(out, 28)[k] for k in (0, 1))
                a2 = tuple(pos[i][k] + r * _rotate(out, -28)[k] for k in (0, 1))
                ax.add_patch(FancyArrowPatch(
                    a1, a2, connectionstyle=f"arc3,rad={LOOP_RAD}",
                    arrowstyle="-|>", mutation_scale=11, **style))
                lx = pos[i][0] + out[0] * (1.63 * r + 0.13)
                ly = pos[i][1] + out[1] * (1.63 * r + 0.13)
            else:
                dx, dy = pos[j][0] - pos[i][0], pos[j][1] - pos[i][1]
                hops = min(abs(i - j), n - abs(i - j))
                rad = ARC_RAD * hops
                # An arc3 leaves its endpoints along the tangent, not along the
                # chord, so attach on the node boundary in *that* direction or
                # the arrow detaches from the circle it starts on.
                t_out = _unit((0.5 * dx + rad * dy, 0.5 * dy - rad * dx))
                t_in = _unit((-0.5 * dx + rad * dy, -0.5 * dy - rad * dx))
                a1 = (pos[i][0] + r * t_out[0], pos[i][1] + r * t_out[1])
                a2 = (pos[j][0] + r * t_in[0], pos[j][1] + r * t_in[1])
                ax.add_patch(FancyArrowPatch(
                    a1, a2, connectionstyle=f"arc3,rad={rad}",
                    arrowstyle="-|>", mutation_scale=11, **style))
                # Label near the *source* end rather than at the arc's apex:
                # the apex of every long chord sits in the middle of the ring,
                # so at six states a dozen of them stack there. A quarter of
                # the way along, each number sits beside the state it leaves,
                # fanned out by the angle its own arc departs at.
                mx, my = (pos[i][0] + pos[j][0]) / 2, (pos[i][1] + pos[j][1]) / 2
                ctrl = (mx + rad * dy, my - rad * dx)
                lx, ly = _bezier(a1, ctrl, a2, 0.25)
                lx, ly = _clear_of_nodes(lx, ly, pos, r)
            labels.append([lx, ly, _fmt_prob(p)])

    _relax_labels(labels, pos, r)
    for lx, ly, txt in labels:
        ax.text(lx, ly, txt, ha="center", va="center", fontsize=8.5,
                color="#1a1a1a", zorder=4,
                bbox=dict(boxstyle="round,pad=0.12", fc="white", ec="none",
                          alpha=0.85))

    for i in range(n):
        ax.add_patch(Circle(pos[i], r, facecolor=fills[i], edgecolor="#1a1a1a",
                            linewidth=1.2, zorder=3))
        # Dark fills need light ink, and the ramp's own step says which those
        # are -- no need to measure a luminance.
        ink = "white" if shades[i] > INK_ON_DARK else "#1a1a1a"
        scale = r / 0.30
        ax.text(pos[i][0], pos[i][1] + 0.24 * r, str(i), ha="center", va="center",
                fontsize=max(9, 16 * scale), fontweight="bold", color=ink, zorder=4)
        ax.text(pos[i][0], pos[i][1] - 0.32 * r, archetypes[i], ha="center",
                va="center", fontsize=max(6.5, 10.5 * scale), color=ink, zorder=4)

    _state_table(fig.add_subplot(gs[0, 1]), matrix, archetypes, states, pi,
                 fills, shades)

    fig.suptitle(f"{rs.tex_safe(name)}  ({n}-state FSMC)",
                 y=0.97)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    paths = rs.save_figure(path_base)
    plt.close(fig)
    return paths


def _state_table(ax, matrix, archetypes, states, pi, fills, shades):
    """The per-state parameters, beside the diagram: the three numbers written
    into fsmc_states.json, plus what the chain says about each state."""
    from matplotlib.patches import Rectangle

    n = len(matrix)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    cols = [(0.0455, "s", "center"), (0.17, "curve", "left"),
            (0.44, "slope", "center"), (0.58, r"$\mathrm{SNR}_{50}$", "center"),
            (0.72, r"$\rho_{\max}$", "center"), (0.845, r"$\bar{D}$", "center"),
            (0.955, r"$\pi_s$", "center")]

    top, row_h = 0.86, min(0.105, 0.70 / n)
    for x, head, align in cols:
        ax.text(x, top, head, ha=align, va="baseline", fontsize=11.5)
    ax.plot([0.0, 1.0], [top - 0.030] * 2, color="#1a1a1a", lw=1.0)

    for i in range(n):
        y = top - 0.075 - i * row_h
        dwell = 1.0 / (1.0 - matrix[i][i]) if matrix[i][i] < 1 else float("inf")
        # The index cell carries the node's fill, so a row can be matched to a
        # node without counting round the ring.
        ax.add_patch(Rectangle((0.018, y - 0.010), 0.054, 0.040,
                               facecolor=fills[i], edgecolor="#1a1a1a", lw=0.8))
        vals = [str(i), archetypes[i], f"{states[i]['slope']:g}",
                f"{states[i]['snr_50_db']:g} dB", f"{states[i]['max_saturation']:g}",
                f"{dwell:.1f}", f"{pi[i]:.3f}"]
        for col, ((x, _, align), v) in enumerate(zip(cols, vals)):
            # Only the index sits on the swatch, so only it needs light ink
            # over the dark end of the ramp.
            ink = "white" if col == 0 and shades[i] > INK_ON_DARK else "#1a1a1a"
            ax.text(x, y, v, ha=align, va="baseline", fontsize=11, color=ink)

    ax.text(0.0, top - 0.075 - n * row_h - 0.055,
            r"$\bar{D}$: mean dwell in time-slots, $1/(1 - p_{ss})$."
            "\n" r"$\pi_s$: long-run share of time in state $s$."
            "\nEdge labels are $p_{ij}$; every row of the matrix sums to 1."
            "\nA transition of probability 0 is not drawn.",
            ha="left", va="top", fontsize=10, color="#3c3c3c", linespacing=1.6)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("n_channels", type=int, help="number of channels to generate")
    ap.add_argument("--max-n-states", type=int, default=4,
                    help="upper bound on states per channel; each channel draws "
                         "n ~ U{min..max} (default 4)")
    ap.add_argument("--min-n-states", type=int, default=2,
                    help="lower bound on states per channel (default 2). Set equal to "
                         "--max-n-states for a homogeneous batch")
    ap.add_argument("--seed", type=int, default=None,
                    help="master seed (default: $SEED, else random)")
    ap.add_argument("--prefix", default="channel_gen",
                    help="directory name prefix (default channel_gen)")
    ap.add_argument("--dwell-min", type=float, default=20.0,
                    help="min mean dwell in time-slots (default 20)")
    ap.add_argument("--dwell-max", type=float, default=100.0,
                    help="max mean dwell in time-slots (default 100)")
    ap.add_argument("--locality", type=float, default=1.0,
                    help="transition locality; higher jumps further (default 1.0)")
    ap.add_argument("--concentration", type=float, default=5.0,
                    help="Dirichlet concentration; higher is less random (default 5.0)")
    ap.add_argument("--no-figure", action="store_true",
                    help="skip the per-channel state diagram")
    ap.add_argument("--force", action="store_true", help="overwrite existing directories")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    if args.n_channels < 1:
        ap.error("n_channels must be >= 1")
    if args.min_n_states < 2:
        ap.error("--min-n-states must be >= 2")
    if args.max_n_states < args.min_n_states:
        ap.error("need --min-n-states <= --max-n-states")
    if args.dwell_min <= 1 or args.dwell_max < args.dwell_min:
        ap.error("need 1 < --dwell-min <= --dwell-max")
    if args.locality <= 0:
        ap.error("--locality must be > 0")
    if args.concentration <= 0:
        ap.error("--concentration must be > 0")

    seed = args.seed
    if seed is None:
        env = os.environ.get("SEED")
        seed = int(env) if env else random.randrange(2**31)
    rng = random.Random(seed)

    print(f"seed={seed}  channels={args.n_channels}  "
          f"states~U{{{args.min_n_states}..{args.max_n_states}}}  "
          f"dwell=[{args.dwell_min:g},{args.dwell_max:g}]  "
          f"locality={args.locality:g}  concentration={args.concentration:g}")

    names = [f"{args.prefix}_{i}" for i in range(args.n_channels)]
    clash = RESERVED.intersection(names)
    if clash:
        ap.error(f"refusing to write reserved channel directory: {', '.join(sorted(clash))}")

    if not args.dry_run and not args.force:
        existing = [n for n in names if os.path.isdir(os.path.join(CHANNEL_DIR, n))]
        if existing:
            ap.error(f"these already exist (use --force): {', '.join(existing)}")

    channels_cfg = []
    for i, name in enumerate(names):
        n_states = rng.randint(args.min_n_states, args.max_n_states)
        states, archetypes = build_states(n_states)
        matrix = build_transition_matrix(n_states, rng, args.dwell_min,
                                         args.dwell_max, args.locality, args.concentration)
        pi = stationary(matrix)

        print(f"\n── {name}  (channel {i}, {n_states} states)")
        print(f"   {'idx':<4} {'archetype':<10} {'dwell':>7} {'pi':>7}   transition row")
        for s in range(n_states):
            dwell = 1.0 / (1.0 - matrix[s][s]) if matrix[s][s] < 1 else float("inf")
            row = "  ".join(f"{v:.3f}" for v in matrix[s])
            print(f"   {s:<4} {archetypes[s]:<10} {dwell:>7.1f} {pi[s]:>7.3f}   {row}")

        if not args.dry_run:
            d = os.path.join(CHANNEL_DIR, name)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "transition_matrix.kov"), "w") as f:
                for row in matrix:
                    # 9 significant digits, not 6 decimal places: a far-rank
                    # transition can land near 1e-8, and rounding it to zero
                    # would silently cut the chain's connectivity. kovian
                    # parses whatever std::stod accepts, scientific included.
                    f.write(", ".join(f"{v:.9g}" for v in row) + "\n")
            with open(os.path.join(d, "fsmc_states.json"), "w") as f:
                f.write("[\n")
                f.write(",\n".join(
                    f'    {{ "snr_50_db": {s["snr_50_db"]}, "slope": {s["slope"]},'
                    f' "max_saturation": {s["max_saturation"]} }}' for s in states))
                f.write("\n]\n")

            if not args.no_figure:
                # The figure is the only part of this script that needs
                # matplotlib (and the harness, for the paper's figure style).
                # The channel itself is already on disk at this point, so a
                # missing dependency costs the drawing, not the run.
                try:
                    outs = draw_fsmc(name, matrix, archetypes, states, pi,
                                     os.path.join(d, FIGURE_BASENAME))
                    print(f"   diagram: {outs}")
                except ImportError as exc:
                    print(f"   diagram skipped ({exc}); --no-figure silences this",
                          file=sys.stderr)

        channels_cfg.append({"type": "sigmoid", "name": name})

    print("\n" + "=" * 70)
    if args.dry_run:
        print("dry run -- nothing written")
    else:
        print(f"wrote {len(names)} channels under {CHANNEL_DIR}/")
        print("note: *.json, *.png and *.pdf are gitignored, so fsmc_states.json and "
              "the diagrams need `git add -f`\n      (or a .gitignore exception) to be "
              "committed.")

    print("\nconfig snippet -- a channel is addressed by its index in this list,\n"
          "which is what a scheduler's `channel` field names:\n")
    print(json.dumps({"channels": channels_cfg}, indent=2))



if __name__ == "__main__":
    main()
