import json
import sys
import os
import copy
import hashlib
import math
import multiprocessing as mp
import random
import re
import subprocess
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor, FIRST_COMPLETED, as_completed, wait
from dataclasses import dataclass
from typing import Optional
from collections import defaultdict
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.lines import Line2D

TESTS_DIR = "tests"

# The modes the CLI accepts, checked before anything runs — see the guard in
# __main__ for why an unknown one must not fall through to `tests`.
MODES = ("tests", "sweep", "error_sweep")

# ── Figure style and paper symbols ────────────────────────────────────────────
#
# Every symbol below is written as matplotlib *mathtext* (`$...$`), which is
# rendered by matplotlib itself — no TeX installation required, so the harness
# still draws on the AIRE cluster. Set MPL_USETEX=1 to route the same strings
# through a real LaTeX install instead (slower); the markup is valid in both,
# so nothing else changes. That path needs latex + dvipng on PATH *and* the
# cm-super fonts — matplotlib's usetex preamble pulls in `type1ec.sty`, and
# without it (as on this machine) every savefig dies inside latex.
#
# The symbols are the paper's, and these constants are the single place they
# are spelled — never inline `$\theta_n$` at a call site.
SYM_RR   = r"$\theta_n$"                # reliability requirement of task n
SYM_N    = r"$N$"                       # number of tasks in the set
SYM_L    = r"$L_n$"                     # packet length of task n, in frames
SYM_U    = r"$U$"                       # total utilization
SYM_M    = r"$m$"                       # deliveries required per window
SYM_KWIN = r"$K_{\mathrm{win}}$"        # window length, in instances
SYM_ERR  = r"$\varepsilon$"             # predictor error level
SYM_PFP  = r"$P_{\mathrm{FP}}$"         # transmit power of a fixed-power baseline
SYM_PCH  = r"$P_{\mathrm{CH}}$"         # transmit power of a CHARM-family scheduler

plt.rcParams.update({
    "font.family":       "serif",
    # Latin Modern Roman is the LaTeX body face (texlive-lm on Fedora,
    # lmodern on Debian). If it isn't installed the list falls through to
    # STIX and then DejaVu, so figures still draw — just not in LM. A
    # machine that *has* the font but was first run without it needs its
    # matplotlib font cache cleared once: rm ~/.cache/matplotlib/fontlist-*.json
    "font.serif":        ["Latin Modern Roman", "STIXGeneral", "DejaVu Serif"],
    # Computer Modern for math: Latin Modern is a redraw of CM, so the two
    # match closely, and matplotlib cannot load the OTF Latin Modern Math.
    "mathtext.fontset":  "cm",
    # Sized for a figure that lands in a paper column at ~half width — the
    # 10 pt matplotlib defaults shrink to unreadable there.
    "axes.titlesize":    14,
    "axes.labelsize":    13,
    "xtick.labelsize":   12,
    "ytick.labelsize":   12,
    "legend.fontsize":   12,
    "figure.titlesize":  15,
    # A solid mid-gray grid drowned the curves; dotted light gray reads as a
    # guide instead. Both axes — a value is read off y, but tracing a
    # utilization down the column wants the vertical lines too. The two bar
    # charts stay y-only, where verticals would just cut through the bars.
    "axes.grid":         False,      # each axis still opts in explicitly
    "axes.axisbelow":    True,       # never over the data
    "grid.color":        "#9a9a9a",
    "grid.linestyle":    ":",
    "grid.linewidth":    0.8,
    "grid.alpha":        1.0,
})
if os.environ.get("MPL_USETEX"):
    plt.rcParams.update({"text.usetex": True,
                         "text.latex.preamble": r"\usepackage{amsmath}"})

# Formats every figure is written in, overridable with PLOT_FORMATS (comma
# separated, e.g. "pdf" or "png,svg"). PDF is the one to put in the paper: it
# is vector, so it stays sharp at any zoom, while the PNG is only a 150-dpi
# raster for quick viewing. Both are written from the same figure object, so
# they can't disagree.
PLOT_FORMATS = [f.strip().lstrip(".") for f in
                os.environ.get("PLOT_FORMATS", "png,pdf").split(",") if f.strip()]

# Type 42 (TrueType) rather than matplotlib's default Type 3: the text stays
# real text — selectable, searchable, and re-usable by the typesetter — and
# IEEE/ACM submission checks reject Type 3 outright.
plt.rcParams.update({"pdf.fonttype": 42, "ps.fonttype": 42})

# Per-tick diagnostic figures carry one point per frame, so at the sweep's
# 250k-frame duration a fully vector PDF would embed millions of segments —
# tens of MB that no viewer will pan smoothly. Above this many points the data
# artists are rasterized at save dpi while every label, axis and legend stays
# vector, which is what the zoom actually needs to stay sharp.
RASTERIZE_ABOVE = 20_000

def rasterize_dense_artists(fig, n_points: int) -> None:
    """Rasterize the plotted data of `fig` (not its text) when a series is
    long enough for vector output to become unwieldy. No-op below the
    threshold, and no-op for raster formats, which look the same either way."""
    if n_points <= RASTERIZE_ABOVE:
        return
    for ax in fig.axes:
        for artist in list(ax.lines) + list(ax.collections) + list(ax.patches):
            artist.set_rasterized(True)

def save_figure(path_base: str) -> str:
    """Write the current figure once per PLOT_FORMATS entry and return the
    paths for logging. `path_base` carries no extension — this owns it, so a
    new format needs no change at the call sites. dpi applies to the raster
    formats and to any artist rasterize_dense_artists() marked; the vector
    ones ignore it."""
    outs = []
    for fmt in PLOT_FORMATS:
        out = f"{path_base}.{fmt}"
        plt.savefig(out, dpi=150, bbox_inches="tight")
        outs.append(out)
    return ", ".join(outs)

def tex_safe(s: str) -> str:
    """Escape the characters a real LaTeX pass chokes on. Scheduler labels and
    test names carry underscores (`CHEDF_25W`), which mathtext takes literally
    but LaTeX reads as a subscript, so this is a no-op unless MPL_USETEX is on.
    Only ever call it on plain strings — running it over one that already
    contains a `$...$` symbol would escape the math too."""
    if not plt.rcParams.get("text.usetex"):
        return s
    return re.sub(r"([_&#%$])", r"\\\1", s)

# Master seed for reproducible runs. Read once at import time from the SEED
# env var. When None, behavior is non-deterministic (clock-based RNG in C++,
# unseeded random module in Python) — matches the pre-seed baseline.
MASTER_SEED: Optional[int] = int(os.environ["SEED"]) if os.environ.get("SEED") else None

def derive_seed(master: int, test_name: str, run_idx: int) -> int:
    """Stable 63-bit seed per (master, test_name, run_idx). Using blake2b
    rather than a linear mixing function so reordering of jobs in the pool
    can't accidentally produce correlated streams. Mask to int63 so the
    value round-trips cleanly through JSON / nlohmann's signed int64 path."""
    h = hashlib.blake2b(f"{master}|{test_name}|{run_idx}".encode(),
                        digest_size=8).digest()
    return int.from_bytes(h, "little") & 0x7FFF_FFFF_FFFF_FFFF

# ── UUniFast task-set generator ───────────────────────────────────────────────

def uunifast(n: int, U: float) -> list[float]:
    """Bini & Buttazzo UUniFast: returns n utilizations in (0,1) summing to U,
    uniformly distributed on the simplex."""
    utils = []
    s = U
    for i in range(1, n):
        nxt = s * random.random() ** (1.0 / (n - i))
        utils.append(s - nxt)
        s = nxt
    utils.append(s)
    return utils

def uunifast_packets(n: int, U: float, c_min: int, c_max: int,
                     rr_min: float, rr_max: float) -> list[dict]:
    """Turn a UUniFast draw into a fixed_rate packet list.
    Each task gets an independent frame count C ~ uniform_int[c_min, c_max]
    and reliability requirement rr ~ uniform[rr_min, rr_max]. Deadline is
    derived from utilization: D = floor(C / u). Since UUniFast yields u in
    (0,1), we have D >= C, so the floored D keeps actual u' = C/D in (u, 1]
    — utilization stays at-or-slightly-above the requested level instead of
    drifting upward as it would when rounding C from a fixed D."""
    utils = uunifast(n, U)
    packets = []
    for i, u in enumerate(utils):
        C = random.randint(c_min, c_max)
        D = max(C, int(C / u))  # floor; clamp to C if u ≈ 1
        packets.append({
            "id":                i + 1,
            "relative_deadline": D,
            "frames":            C,
            "reliability":      round(random.uniform(rr_min, rr_max), 2),
            "period":            D,
            "phase":             0,
        })
    return packets

def make_config(test: dict, seed: Optional[int] = None) -> dict:
    """Resolve a test definition into a concrete simulator config. If a
    'uunifast' spec is present, a fresh packet_generators block is drawn.

    When `seed` is given, Python's random module is seeded *before* the
    UUniFast draw so the generated task set is deterministic, and the same
    seed is injected into config["simulation"]["seed"] so the C++ side
    reseeds its three RNGs (target receiver, channel initial state, kovian
    transitions) with derived sub-seeds."""
    if seed is not None:
        random.seed(seed)
    config = copy.deepcopy(test["config"])
    if "uunifast" in test:
        spec = test["uunifast"]
        config["packet_generators"] = [{
            "type":    "fixed_rate",
            "packets": uunifast_packets(spec["n"], spec["U"],
                                        spec["c_min"],  spec["c_max"],
                                        spec["rr_min"], spec["rr_max"]),
        }]
    if seed is not None:
        config["simulation"] = dict(config["simulation"], seed=int(seed))
    return config

# ── Tests ─────────────────────────────────────────────────────────────────────

# CHARM as published: dequeues by shortest period (rate-monotonic order).
BASE_SCHEDULER = {
    "type": "CHARM",
    "tx_power": 10,
    "frequency": 14074000,
    "rx_period": 5,
}

BASE_SCHEDULER_25W = dict(BASE_SCHEDULER, tx_power=25)

# CHEDF — CHARM's retransmission/power policy over an EDF queue. A separate
# scheduler type, and on the C++ side a standalone class that copies CHARM's
# rule rather than subclassing it, so it takes the same parameters and differs
# from BASE_SCHEDULER in nothing but `type`. A CHARM/CHEDF pair on the same
# axes isolates queue discipline.
BASE_CHEDF_SCHEDULER     = dict(BASE_SCHEDULER, type="CHEDF")
BASE_CHEDF_SCHEDULER_25W = dict(BASE_CHEDF_SCHEDULER, tx_power=25)

BASE_CATS_SCHEDULER = {
    "type": "CATS",
    "frequency": 14074000,
    "belief_threshold": 0.7,
    "utilization_threshold": 0.9,
}

BASE_CHANNELS = [
    {
        "type": "sigmoid",
        "name": "channel_20m",
        "frequency": 14074000
    }
]

BASE_SIM = {
    "duration":      250000,  # Sufficient for this steady state matrix
    "predict_error": 0.00,    # ±half-width of uniform noise injected per predicted decode probability
    "window_k":      100,     # weakly-hard (m,k) window, in instances — see below
}

# The (m,k) window length decides how burst-sensitive `sched_ratio_mk` is:
# k -> inf recovers the whole-run average (`sched_ratio`), while the smallest
# usable k tolerates no bursts at all. Two hard constraints:
#
#   k >= 1 / (1 - reliability_req)   or m = ceil(req*k) == k and the window
#                                     silently degenerates to zero-miss. At
#                                     rr_max = 0.90 that floor is k = 10.
#   k <= instances per id             or the id has no complete window and is
#                                     scored vacuously met (see warn_vacuous).
#                                     This is the binding constraint at k=100.
#
# 100 is chosen to match the 2-decimal `reliability` draws in make_test_set:
# m = ceil(req*100) is exactly req*100, so m/k reproduces the requirement with
# no rounding and every task is compared against the rate it actually asked
# for. Deriving a per-task k from the fraction instead (0.50 -> 1/2,
# 0.51 -> 51/100) was rejected: the denominator is an artifact of the decimal,
# so a 0.01 change in the requirement would swing burst tolerance by ~50x.
# Override per run with the WINDOW_K env var to check how sensitive a
# conclusion is to this choice.
if os.environ.get("WINDOW_K"):
    BASE_SIM["window_k"] = int(os.environ["WINDOW_K"])

def scenario_label(n: int, c_min: int, c_max: int,
                   rr_min: float, rr_max: float, U: Optional[float] = None) -> str:
    """Render a scenario's parameters as a compact label. This string keys the
    results dicts, names the test directories and names the output PNGs, so it
    stays plain ASCII — `scenario_title()` is the display form that swaps in
    the paper's symbols. Always derived from the actual values so it can't
    drift."""
    parts = []
    if U is not None:
        parts.append(f"U={int(round(U * 100))}")
    parts.append(f"n={n}")
    parts.append(f"L=[{c_min},{c_max}]")
    # RR = reliability requirement (the per-packet `reliability` field).
    parts.append(f"RR=[{rr_min},{rr_max}]")
    return ",".join(parts)

# Plain token in a scenario label -> the paper symbol it stands for.
_SCEN_SYMBOLS = {"U": SYM_U, "n": SYM_N, "L": SYM_L, "RR": SYM_RR}

# Which power symbol a scheduler's transmit power is written with: the paper
# separates the fixed-power baselines (RM, EDF) from the CHARM-family ones
# (CHARM, CHEDF), which spend their power under an accumulated-probability
# retransmission rule. Keyed on the label's base name; a roster entry with no
# entry here falls back to P_FP, so a new fixed-power baseline needs nothing
# added but a new family does.
_POWER_SYMBOLS = {"RM": SYM_PFP, "EDF": SYM_PFP, "CHARM": SYM_PCH, "CHEDF": SYM_PCH,
                  "SRM": SYM_PCH, "SEDF": SYM_PCH}

def scheduler_title(label: str, *extra: str) -> str:
    """Display form of a roster label: `CHEDF_10W` reads as
    `CHEDF (P_CH = 10 W)` — the scheduler keeps its name and the parameters
    move into a parenthesis, the way the paper writes them. Any `extra` parts
    join that same parenthesis, so the error sweep gets
    `CHEDF (P_CH = 10 W, ε = 0.15)` rather than two bracketed groups.

    Display only. The raw label keys the results dicts and names the
    `<scheduler>_scheduled_packets.json` logs, so it must not be prettified at
    the source. A label with no `_<n>W` suffix (CATS, which picks its own
    power) passes through as just its name."""
    m     = re.fullmatch(r"(.+?)_(\d+)W", label)
    base  = m.group(1) if m else label
    parts = [f"{_POWER_SYMBOLS.get(base, SYM_PFP)} = {m.group(2)} W"] if m else []
    parts.extend(extra)
    return tex_safe(base) + (f" ({', '.join(parts)})" if parts else "")

def _math(sym: str, expr: str) -> str:
    """Compose one of the SYM_* constants — which carry their own `$…$` — with
    an expression, as a single mathtext run: `_math(SYM_L, r"\\in \\{1,3\\}")`.
    Keeps the constants the one place a symbol is spelled."""
    return f"${sym.strip('$')} {expr}$"

# One field of a scenario label: a scalar (`U=10`, `n=4`) or a range
# (`L=[1,3]`, `RR=[0.5,0.9]`). Matching the whole field, brackets included, is
# what lets the renderer below tell the two apart.
_SCEN_FIELD_RE = re.compile(r"(U|n)=([^,]+)|(L|RR)=\[([^,]+),([^\]]+)\]")

def _render_scen_field(m: "re.Match") -> str:
    """One field of a scenario label as mathtext.

    `=` is kept only for the values that really are exact (N, U). The two
    ranges are the interval a task's parameter is *drawn from*, so writing
    `L_n = [1,3]` would equate a scalar with a set; they get `∈` instead, with
    the bracket matching the domain — braces for the integer frame count
    (`random.randint`), square brackets for the continuous reliability
    (`random.uniform`)."""
    if m.group(1):                                   # scalar field
        name, value = m.group(1), m.group(2)
        if name == "U":
            # scenario_label stores U as an integer percentage so the string
            # stays filename-safe; show the fraction the axes actually use.
            value = f"{int(value) / 100:.2f}"
        return _math(_SCEN_SYMBOLS[name], f"= {value}")
    name, lo, hi = m.group(3), m.group(4), m.group(5)
    if lo == hi:
        return _math(_SCEN_SYMBOLS[name], f"= {lo}")
    if name == "L":
        # Enumerate a short run ({1,2,3}); elide a long one ({1,…,9}).
        span = range(int(lo), int(hi) + 1)
        body = ",".join(str(v) for v in span) if len(span) <= 4 else rf"{lo},\dots,{hi}"
        return _math(SYM_L, rf"\in \{{{body}\}}")
    return _math(SYM_RR, rf"\in [{lo}, {hi}]")

def scenario_title(label: str) -> str:
    """Display form of a scenario label — paper symbols in mathtext, one field
    per `_render_scen_field`. Applied only at draw time; the label itself has
    to stay filename-safe.

    A test name carries its roster label after an underscore
    (`U=10,n=2,...,RR=[0.5,0.9]_CHEDF_25W`); that tail is split off and run
    through `scheduler_title()`. Nothing else in a scenario label contains an
    underscore, so the split is unambiguous."""
    scen, _sep, sched = label.partition("_")
    fields = [_render_scen_field(m) for m in _SCEN_FIELD_RE.finditer(scen)]
    out    = ", ".join(fields) if fields else tex_safe(scen)
    return f"{out} — {scheduler_title(sched)}" if sched else out

# Each scenario: (U, n, c_min, c_max, rr_min, rr_max)
SCENARIOS = [
    (0.10, 2,  1, 3, 0.50, 0.90),
    (0.25, 5,  1, 3, 0.50, 0.90),
    (0.50, 8,  1, 3, 0.50, 0.90),
]

BASE_EDF_SCHEDULER     = {"type": "EDF", "tx_power": 10, "frequency": 14074000}
BASE_EDF_SCHEDULER_25W = dict(BASE_EDF_SCHEDULER, tx_power=25)
BASE_RM_SCHEDULER      = {"type": "Rate_M", "tx_power": 10, "frequency": 14074000}
BASE_RM_SCHEDULER_25W  = dict(BASE_RM_SCHEDULER, tx_power=25)

# SRM / SEDF — "Static RM" and "Static EDF". CHARM's accumulated-probability
# redundancy rule driven by a *static* estimate of the channel instead of a
# refreshed prediction: the long-run mean decode probability at the
# scheduler's power, rho_bar_j = sum_s pi_s rho_s(SNR(P_j)), averaged over the
# FSMC's stationary distribution.
#
# The C++ side computes rho_bar from the channel at construction, which is why
# these take no `rx_period` and carry no knob for one: a static estimate
# learns nothing from an RX slot, so SRM/SEDF never listen and spend every
# slot transmitting or idle. That makes them strictly cheaper in airtime than
# CHARM/CHEDF at the same power — an SRM/CHARM gap mixes the value of
# refreshed channel information with that airtime difference, so read it
# alongside the energy row rather than on its own.
#
# They occupy the middle of the roster's ladder of channel knowledge:
#   RM/EDF       — one send per frame, no channel knowledge
#   SRM/SEDF     — redundancy from the channel's long-run average
#   CHARM/CHEDF  — redundancy from a prediction refreshed every rx_period
#   CATS         — belief-driven, and picks its power too
BASE_SRM_SCHEDULER      = {"type": "SRM",  "tx_power": 10, "frequency": 14074000}
BASE_SRM_SCHEDULER_25W  = dict(BASE_SRM_SCHEDULER, tx_power=25)
BASE_SEDF_SCHEDULER     = {"type": "SEDF", "tx_power": 10, "frequency": 14074000}
BASE_SEDF_SCHEDULER_25W = dict(BASE_SEDF_SCHEDULER, tx_power=25)

# Fixed-power baselines are run at both power levels CATS can pick from
# (CATS predicts over {1, 10, 25} W), so a CATS curve can be read against a
# baseline that spends the same per-frame energy as its high-power choice.
#
# The two sweep modes deliberately use *different* rosters.
#
# `sweep` (predict_error = 0, see BASE_SIM) is the full comparison: it carries
# the complete 2x2 of the two effects under study — retransmission/power policy
# (RM/EDF vs CHARM/CHEDF) crossed with queue discipline (RM/CHARM vs EDF/CHEDF)
# — so each gap can be read against its own control, with the predictor perfect
# so none of them can be blamed on prediction noise.
# The roster of the paper's Simulation 1 (zero predictor error), which is what
# `sweep` runs. Read across the four properties it varies:
#
#   scheduler    listening          power      redundancy                drops early
#   RM_10W       none               10 W       none, one send per frame  no
#   EDF_10W      none               10 W       none, one send per frame  no
#   SEDF_10W     none               10 W       static channel average    no
#   CHARM_10W    every rx_period    10 W       refreshed estimate        no
#   CHARM_25W    every rx_period    25 W       refreshed estimate        no
#   CHEDF_10W    every rx_period    10 W       refreshed estimate        no
#   CHEDF_25W    every rx_period    25 W       refreshed estimate        no
#   CATS         adaptive           adaptive   refreshed estimate        yes
#
# Early dropping is not a knob: it is built into CATS (it discards a packet
# whose remaining slots cannot cover its reliability target) and no other
# scheduler does it.
#
# Deliberately *not* the full cross. Only the CHARM family is carried at both
# powers, since 10 W vs 25 W is the comparison that matters for a scheduler
# that spends its power under a retransmission rule; the fixed-power and
# static baselines are pinned at 10 W. SRM (the period-ordered static
# scheduler) is built and wired but no roster currently uses it — add it back
# here if the period-ordered arm of that comparison is wanted.
#
# Ordered by family, with a family's two power levels adjacent: the panel
# drawer takes color from enumeration order.
SCHEDULERS = [
    ("RM_10W",     BASE_RM_SCHEDULER),
    ("EDF_10W",    BASE_EDF_SCHEDULER),
    ("SEDF_10W",   BASE_SEDF_SCHEDULER),
    ("CHARM_10W",  BASE_SCHEDULER),
    ("CHARM_25W",  BASE_SCHEDULER_25W),
    ("CHEDF_10W",  BASE_CHEDF_SCHEDULER),
    ("CHEDF_25W",  BASE_CHEDF_SCHEDULER_25W),
    ("CATS",       BASE_CATS_SCHEDULER),
]

# `error_sweep` draws one curve per scheduler *per error level*, so the 9-entry
# roster would put ~21 curves on each axis. It is trimmed to the predictor-
# sensitive schedulers plus a fixed-power reference: CHEDF rather than CHARM
# (same queue discipline as CATS, so the surviving gap is the power policy),
# and EDF rather than RM for the same reason. EDF ignores the predictor, so it
# contributes one flat curve regardless of the error axis.
ERROR_SWEEP_SCHEDULERS = [
    ("EDF_10W",    BASE_EDF_SCHEDULER),
    ("EDF_25W",    BASE_EDF_SCHEDULER_25W),
    ("SEDF_10W",   BASE_SEDF_SCHEDULER),
    ("SEDF_25W",   BASE_SEDF_SCHEDULER_25W),
    ("CHEDF_10W",  BASE_CHEDF_SCHEDULER),
    ("CHEDF_25W",  BASE_CHEDF_SCHEDULER_25W),
    ("CATS",       BASE_CATS_SCHEDULER),
]

# Schedulers that never consult the ML predictor. Their results are identical
# at every predict_error level, so the error sweep runs them once and
# replicates. Keyed on scheduler *type*, not the label, so adding another
# fixed-power variant needs no change here.
# SRM/SEDF belong here too: their decode estimate is computed from the
# channel before the run and never refreshed, so predict_error cannot reach
# them. On the error sweep they contribute one flat curve per power, like
# the fixed-power baselines.
PREDICTOR_INDEPENDENT_TYPES = {"Rate_M", "EDF", "SRM", "SEDF"}

def is_predictor_independent(scheduler: dict) -> bool:
    return scheduler["type"] in PREDICTOR_INDEPENDENT_TYPES

TESTS = [
    {
        "name": f"{scenario_label(n, c_min, c_max, rr_min, rr_max, U)}_{sch_name}",
        "config": {
            "simulation": BASE_SIM,
            "scheduler":  scheduler,
            "channels":   BASE_CHANNELS,
        },
        "uunifast": {
            "U":      U,
            "n":      n,
            "c_min":  c_min,
            "c_max":  c_max,
            "rr_min": rr_min,
            "rr_max": rr_max,
        },
    }
    for U, n, c_min, c_max, rr_min, rr_max in SCENARIOS
    for sch_name, scheduler in SCHEDULERS
]

CONFIG_FILE = "simulation_config.json"
LOG_FILE    = "simulation_log.json"
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
BINARY      = os.path.join(SCRIPT_DIR, "main.o")

# ── Data classes ──────────────────────────────────────────────────────────────

@dataclass
class Packet:
    id: int
    id_count: int
    deadline: int
    frames: int
    frame_count: int
    reliability_req: float

@dataclass
class Transmission:
    packet: Packet
    tx_power: int
    frequency: int
    probability: float
    reliability_req: float
    received: bool

@dataclass
class Prediction:
    powers: list[int]
    probs: list[float]
    frequency: int

@dataclass
class FsmcStatus:
    frequency: int
    state: int
    slope: float
    snr_50_db: float
    max_saturation: float
    noise_floor_dbm: float

@dataclass
class Frame:
    tick: int
    radio_mode: str
    buffer: list[Packet]
    fsmc: list[FsmcStatus]
    missed_packets: list[Packet]
    dropped_packets: list[Packet]
    transmission: Optional[Transmission] = None
    prediction: Optional[Prediction] = None

# ── Parsers ───────────────────────────────────────────────────────────────────

def parse_packet(d: dict) -> Packet:
    return Packet(
        id               = d["id"],
        id_count         = d["id_count"],
        deadline         = d["deadline"],
        frames           = d["frames"],
        frame_count      = d.get("frame_count", 0),
        reliability_req = d["reliability_req"]
    )

def parse_frame(d: dict) -> Frame:
    transmission = None
    if "transmission" in d:
        t = d["transmission"]
        transmission = Transmission(
            packet           = parse_packet(t["packet"]),
            tx_power         = t["tx_power"],
            frequency        = t["frequency"],
            probability      = t["probability"],
            reliability_req = t["reliability_req"],
            received         = t["received"]
        )

    prediction = None
    if "prediction" in d:
        p = d["prediction"]
        prediction = Prediction(
            powers    = p["powers"],
            probs     = p["probs"],
            frequency = p["frequency"]
        )

    return Frame(
        tick            = d["tick"],
        radio_mode      = d["radio_mode"],
        buffer          = [parse_packet(p) for p in d["buffer"]],
        fsmc            = [FsmcStatus(**ch) for ch in d["fsmc"]],
        missed_packets  = [parse_packet(p) for p in d["missed_packets"]],
        dropped_packets = [parse_packet(p) for p in d.get("dropped_packets", [])],
        transmission    = transmission,
        prediction      = prediction
    )

# ── Runner ────────────────────────────────────────────────────────────────────

def write_config(config: dict, path: str):
    with open(path, "w") as f:
        json.dump(config, f, indent=4)

def run_simulation(binary: str, config_file: str, log_file: Optional[str] = None):
    cmd = [binary, config_file] + ([log_file] if log_file else [])
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=SCRIPT_DIR)
    if result.returncode != 0:
        print("Simulation failed:")
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError("Simulation exited with code", result.returncode)

def load_results(log_file: str) -> list[Frame]:
    with open(log_file) as f:
        raw = json.load(f)
    return [parse_frame(entry) for entry in raw]

def print_test_summary(test: dict):
    cfg = test["config"]
    print(f"\n── Test: {test['name']} ──")
    sched = cfg['scheduler']
    tx_power_str = f"  tx_power={sched['tx_power']}W" if 'tx_power' in sched else ""
    rx_period_str = f"  rx_period={sched['rx_period']}" if 'rx_period' in sched else ""
    belief_str = f"  belief_threshold={sched['belief_threshold']}" if 'belief_threshold' in sched else ""
    util_str = f"  util_threshold={sched['utilization_threshold']}" if 'utilization_threshold' in sched else ""
    print(f"   Scheduler  : {sched['type']}{tx_power_str}  freq={sched['frequency']}Hz{rx_period_str}{belief_str}{util_str}")
    print(f"   Duration   : {cfg['simulation']['duration']} ticks")
    print(f"   Channels   : {', '.join(ch['name'] for ch in cfg['channels'])}")
    print(f"   Packets:")
    for gen in cfg["packet_generators"]:
        for p in gen["packets"]:
            print(f"     id={p['id']}  period={p['period']}  deadline={p['relative_deadline']}"
                  f"  frames={p['frames']}  reliability={p['reliability']}  phase={p['phase']}")

def run_test(test: dict, seed: Optional[int] = None) -> tuple[list["Frame"], dict]:
    config = make_config(test, seed=seed)
    print_test_summary({ "name": test["name"], "config": config })
    cfg_path = os.path.abspath(CONFIG_FILE)
    log_path = os.path.abspath(LOG_FILE)
    write_config(config, cfg_path)
    run_simulation(BINARY, cfg_path, log_path)
    frames = load_results(log_path)
    print(f"   Done — {len(frames)} frames loaded")
    return frames, config

# ── Metrics extraction ────────────────────────────────────────────────────────

def extract_metrics(frames: list[Frame], config: dict) -> dict:
    ticks = [f.tick for f in frames]

    fsmc_state  = [f.fsmc[0].state if f.fsmc else None for f in frames]
    tx_prob     = [f.transmission.probability if f.transmission else None for f in frames]
    tx_power    = [f.transmission.tx_power if f.transmission else 0 for f in frames]
    pred_prob   = [f.prediction.probs[0] if f.prediction else None for f in frames]
    missed_ticks  = [f.tick for f in frames if f.missed_packets]
    dropped_ticks = [f.tick for f in frames if f.dropped_packets]

    cumulative_missed = []
    cumulative_dropped = []
    total_missed = 0
    total_dropped = 0
    for f in frames:
        total_missed  += len(f.missed_packets)
        total_dropped += len(f.dropped_packets)
        cumulative_missed.append(total_missed)
        cumulative_dropped.append(total_dropped)

    cumulative_generated = []
    seen = set()
    total_gen = 0
    for f in frames:
        for p in f.buffer:
            key = (p.id, p.id_count)
            if key not in seen:
                seen.add(key)
                total_gen += 1
        cumulative_generated.append(total_gen)

    # Seed all packet IDs and requirements from config so untransmitted packets still appear
    per_id_ticks   = defaultdict(list)
    per_id_success = defaultdict(list)
    per_id_req     = {}
    id_total       = defaultdict(int)
    id_received    = defaultdict(int)
    for gen in config["packet_generators"]:
        for p in gen["packets"]:
            per_id_req[p["id"]] = p["reliability"]

    for f in frames:
        if f.transmission:
            pid = f.transmission.packet.id
            id_total[pid]    += 1
            id_received[pid] += int(f.transmission.received)
            per_id_ticks[pid].append(f.tick)
            per_id_success[pid].append(id_received[pid] / id_total[pid])

    # Undelivered packets: instances where not all frame slots were successfully received.
    # Track per (id, id_count, frame_count) whether that frame slot ever had received=True.
    # This correctly handles retransmissions: a slot counts as delivered if received=True
    # at least once, regardless of how many times it was transmitted.
    instance_frames_needed = {}   # (id, id_count) -> frames
    frame_slot_received    = set()  # (id, id_count, frame_count) that had received=True

    for f in frames:
        if f.transmission:
            t   = f.transmission
            key = (t.packet.id, t.packet.id_count)
            instance_frames_needed[key] = t.packet.frames
            if t.received:
                frame_slot_received.add((t.packet.id, t.packet.id_count, t.packet.frame_count))

    # Also register instances that were missed or dropped without any transmission
    for f in frames:
        for p in f.missed_packets + f.dropped_packets:
            key = (p.id, p.id_count)
            if key not in instance_frames_needed:
                instance_frames_needed[key] = p.frames

    undelivered_per_id = defaultdict(int)
    generated_per_id   = defaultdict(int)
    for (pid, pid_count), needed in instance_frames_needed.items():
        generated_per_id[pid] += 1
        slots_received = sum(
            1 for slot in range(needed)
            if (pid, pid_count, slot) in frame_slot_received
        )
        if slots_received < needed:
            undelivered_per_id[pid] += 1

    total_transmissions = sum(1 for f in frames if f.transmission)

    return dict(
        ticks                = ticks,
        fsmc_state           = fsmc_state,
        tx_prob              = tx_prob,
        tx_power             = tx_power,
        pred_prob            = pred_prob,
        missed_ticks         = missed_ticks,
        dropped_ticks        = dropped_ticks,
        cumulative_missed    = cumulative_missed,
        cumulative_dropped   = cumulative_dropped,
        cumulative_generated = cumulative_generated,
        undelivered_per_id   = undelivered_per_id,
        generated_per_id     = generated_per_id,
        total_transmissions  = total_transmissions,
        per_id_ticks         = per_id_ticks,
        per_id_success       = per_id_success,
        per_id_req           = per_id_req,
    )

# ── Per-test plot ─────────────────────────────────────────────────────────────

def plot_test(m: dict, test_name: str, scheduler_type: str):
    ticks  = m["ticks"]
    colors = plt.cm.tab10.colors

    fig = plt.figure(figsize=(14, 16))
    gs  = gridspec.GridSpec(4, 2, figure=fig, hspace=0.45, wspace=0.35)

    # 1. FSMC state (states 0-2 = Excellent, 3-5 = Worst)
    quality = [None if s is None else (0 if s < 3 else 1) for s in m["fsmc_state"]]
    ax1 = fig.add_subplot(gs[0, :])
    ax1.plot(ticks, quality, color="steelblue", linewidth=0.8)
    ax1.set_ylabel("Channel Quality")
    ax1.set_xlabel("Tick")
    ax1.set_title("Channel Quality Evolution")
    ax1.set_yticks(range(2))
    ax1.set_yticklabels(["Excellent", "Worst"])
    ax1.invert_yaxis()
    ax1.grid(True)

    # 2. Actual vs predicted probability
    ax2 = fig.add_subplot(gs[1, 0])
    tx_ticks   = [ticks[i] for i, v in enumerate(m["tx_prob"])   if v is not None]
    pred_ticks = [ticks[i] for i, v in enumerate(m["pred_prob"]) if v is not None]
    ax2.scatter(tx_ticks,   [v for v in m["tx_prob"]   if v is not None], s=4, color="steelblue", label="Actual",    alpha=0.6)
    ax2.scatter(pred_ticks, [v for v in m["pred_prob"] if v is not None], s=4, color="orange",    label="Predicted", alpha=0.6)
    ax2.set_ylabel("Probability")
    ax2.set_xlabel("Tick")
    ax2.set_title("Actual vs Predicted Probability")
    ax2.set_ylim(0, 1.05)
    ax2.legend(markerscale=3)
    ax2.grid(True)

    # 3. Cumulative reliability per packet
    ax3 = fig.add_subplot(gs[1, 1])
    for i, pid in enumerate(sorted(m["per_id_req"].keys())):
        color = colors[i % len(colors)]
        if m["per_id_ticks"][pid]:
            ax3.plot(m["per_id_ticks"][pid], m["per_id_success"][pid], color=color, linewidth=1, label=f"id={pid}")
        else:
            ax3.scatter([], [], color=color, label=f"id={pid} (never scheduled)")
        ax3.axhline(y=m["per_id_req"][pid], color=color, linestyle="--", linewidth=0.8, label=f"{SYM_RR} id={pid} ({m['per_id_req'][pid]})")
    ax3.set_ylabel("Reliability")
    ax3.set_xlabel("Tick")
    ax3.set_title("Cumulative TX Reliability per Packet")
    ax3.set_ylim(0, 1.05)
    ax3.legend()
    ax3.grid(True)

    # 4. Cumulative generated vs missed vs dropped
    ax4 = fig.add_subplot(gs[2, 0])  # row 2
    ax4.plot(ticks, m["cumulative_generated"], color="steelblue", linewidth=1, label="Generated")
    ax4.plot(ticks, m["cumulative_missed"],    color="crimson",   linewidth=1, label="Missed")
    ax4.plot(ticks, m["cumulative_dropped"],   color="darkorange",linewidth=1, label="Dropped")
    if m["missed_ticks"]:
        ax4.scatter(m["missed_ticks"],
                    [m["cumulative_missed"][ticks.index(t)] for t in m["missed_ticks"]],
                    color="crimson", s=15, zorder=5)
    if m["dropped_ticks"]:
        ax4.scatter(m["dropped_ticks"],
                    [m["cumulative_dropped"][ticks.index(t)] for t in m["dropped_ticks"]],
                    color="darkorange", s=15, zorder=5)
    ax4.set_ylabel("Cumulative packets")
    ax4.set_xlabel("Tick")
    ax4.set_title("Cumulative Generated vs Missed vs Dropped")
    ax4.legend()
    ax4.grid(True)

    # 5. Undelivered packets per packet ID (generated vs not fully received)
    ax5 = fig.add_subplot(gs[3, :])
    pids     = sorted(set(list(m["generated_per_id"].keys()) + list(m["per_id_req"].keys())))
    x        = range(len(pids))
    bar_w    = 0.35
    gen_vals = [m["generated_per_id"].get(pid, 0)   for pid in pids]
    und_vals = [m["undelivered_per_id"].get(pid, 0)  for pid in pids]
    ax5.bar([i - bar_w/2 for i in x], gen_vals, bar_w, label="Generated",   color="steelblue", alpha=0.8)
    ax5.bar([i + bar_w/2 for i in x], und_vals, bar_w, label="Undelivered", color="crimson",   alpha=0.8)
    ax5.set_xticks(list(x))
    ax5.set_xticklabels([f"id={pid}" for pid in pids])
    ax5.set_ylabel("Packet instances")
    ax5.set_title("Generated vs Undelivered Packet Instances (not all frames received)")
    ax5.legend()
    ax5.grid(True, axis="y")

    # 6. Per-frame TX power (0W for IDLE/RX) with running average
    ax6 = fig.add_subplot(gs[2, 1])
    pw_vals = m["tx_power"]
    cumulative_avg = [sum(pw_vals[:i+1]) / (i+1) for i in range(len(pw_vals))]
    final_avg = cumulative_avg[-1] if cumulative_avg else 0
    ax6.plot(ticks, pw_vals, color="steelblue", linewidth=0.6, alpha=0.4, label="Power per frame")
    ax6.plot(ticks, cumulative_avg, color="orange", linewidth=1.2, label=f"Average = {final_avg:.2f} W")
    ax6.set_ylabel("Power (W)")
    ax6.set_xlabel("Frame")
    ax6.set_title("TX Power per Frame (0 W = IDLE/RX)")
    ax6.legend()
    ax6.grid(True)

    # The test name ends in the roster label (richer than the bare type, which
    # carries no power), so scheduler_type is only the fallback for a name
    # that has no such suffix.
    title = scenario_title(test_name)
    if "_" not in test_name:
        title = f"{scheduler_title(scheduler_type)} — {title}"
    plt.suptitle(title, fontsize=13)
    os.makedirs(TESTS_DIR, exist_ok=True)
    rasterize_dense_artists(fig, len(ticks))
    out = save_figure(os.path.join(TESTS_DIR, f"results_{test_name}"))
    plt.close()
    print(f"Plot saved to {out}")

# ── Comparison plot ───────────────────────────────────────────────────────────

def plot_comparison(results: list[tuple[str, dict]]):
    colors = plt.cm.tab10.colors
    n      = len(results)

    # Tests being compared usually differ only in the scheduler, and repeating
    # the whole scenario in four legends is what made them unreadable. When
    # every name shares a scenario, the legends carry just the scheduler and
    # the scenario moves to the suptitle; otherwise each legend keeps its full
    # name, since then the scenario is what is being compared.
    scen_parts = {name.partition("_")[0] for name, _m in results}
    shared     = scen_parts.pop() if len(scen_parts) == 1 else None
    legend     = {name: (scheduler_title(name.partition("_")[2]) if shared
                         else scenario_title(name))
                  for name, _m in results}

    fig = plt.figure(figsize=(14, 10))
    gs  = gridspec.GridSpec(2, 2, figure=fig, hspace=0.45, wspace=0.35)

    # 1. Cumulative missed per test
    ax1 = fig.add_subplot(gs[0, 0])
    for i, (name, m) in enumerate(results):
        ax1.plot(m["ticks"], m["cumulative_missed"], color=colors[i], linewidth=1, label=legend[name])
    ax1.set_ylabel("Cumulative missed")
    ax1.set_xlabel("Tick")
    ax1.set_title("Cumulative Missed Deadlines")
    ax1.legend()
    ax1.grid(True)

    # 2. Miss ratio (missed / generated) per test
    ax2 = fig.add_subplot(gs[0, 1])
    for i, (name, m) in enumerate(results):
        ratio = [ms / gn if gn > 0 else 0
                 for ms, gn in zip(m["cumulative_missed"], m["cumulative_generated"])]
        ax2.plot(m["ticks"], ratio, color=colors[i], linewidth=1, label=legend[name])
    ax2.set_ylabel("Miss ratio")
    ax2.set_xlabel("Tick")
    ax2.set_title("Miss Ratio (missed / generated)")
    ax2.set_ylim(0, 1.05)
    ax2.legend()
    ax2.grid(True)

    # 3. Final cumulative reliability per packet per test
    ax3 = fig.add_subplot(gs[1, 0])
    x_pos  = 0
    bar_w  = 0.35
    for i, (name, m) in enumerate(results):
        for j, pid in enumerate(sorted(m["per_id_req"].keys())):
            final_sr = m["per_id_success"][pid][-1] if m["per_id_success"][pid] else 0
            req      = m["per_id_req"][pid]
            pos      = x_pos + j + i * bar_w
            ax3.bar(pos, final_sr, width=bar_w, color=colors[i], alpha=0.8,
                    label=legend[name] if j == 0 else "")
            ax3.plot([pos - bar_w / 2, pos + bar_w / 2], [req, req],
                     color="black", linewidth=1.2, linestyle="--")
        x_pos += len(m["per_id_req"]) + 1
    ax3.set_ylabel("Final reliability")
    ax3.set_title("Final Reliability vs Requirement per Packet")
    ax3.set_ylim(0, 1.05)
    ax3.legend()
    ax3.grid(True, axis="y")

    # 4. FSMC state evolution per test (Excellent vs Worst)
    ax4 = fig.add_subplot(gs[1, 1])
    for i, (name, m) in enumerate(results):
        quality = [None if s is None else (0 if s < 3 else 1) for s in m["fsmc_state"]]
        ax4.plot(m["ticks"], quality, color=colors[i], linewidth=0.6, alpha=0.8, label=legend[name])
    ax4.set_ylabel("Channel Quality")
    ax4.set_xlabel("Tick")
    ax4.set_title("FSMC State Evolution")
    ax4.set_yticks(range(2))
    ax4.set_yticklabels(["Excellent", "Worst"])
    ax4.invert_yaxis()
    ax4.legend()
    ax4.grid(True)

    plt.suptitle("Test Comparison" + (f" — {scenario_title(shared)}" if shared else ""),
                 fontsize=13)
    os.makedirs(TESTS_DIR, exist_ok=True)
    rasterize_dense_artists(fig, max((len(m["ticks"]) for _n, m in results), default=0))
    out = save_figure(os.path.join(TESTS_DIR, "results_comparison"))
    plt.close()
    print(f"Comparison plot saved to {out}")

# ── Summary table ─────────────────────────────────────────────────────────────

def print_summary_table(test_name: str, metrics: dict, config: dict):
    tx_power      = metrics["tx_power"]
    total_energy  = sum(tx_power)                                   # W·time-slot
    average_power = total_energy / len(tx_power) if tx_power else 0.0  # W

    pids = sorted(metrics["per_id_req"].keys())

    # Build per-id req lookup from config
    id_req = {p["id"]: p["reliability"] for gen in config["packet_generators"] for p in gen["packets"]}

    # Last column is widened to fit the "Energy (W·time-slot)" header (20 chars);
    # a narrower width silently overflows and knocks the whole table out of line.
    col_w = [6, 20, 16, 18, 18, 14, 22]
    header = (
        f"{'ID':<{col_w[0]}}"
        f"{'Undelivered':>{col_w[1]}}"
        f"{'Generated':>{col_w[2]}}"
        f"{'Reliability':>{col_w[3]}}"
        f"{'Reliability Req':>{col_w[4]}}"
        f"{'Avg Power (W)':>{col_w[5]}}"
        f"{'Energy (W·time-slot)':>{col_w[6]}}"
    )
    sep = "-" * sum(col_w)

    print(f"\n{'═' * sum(col_w)}")
    print(f"  Summary: {test_name}")
    print(f"{'═' * sum(col_w)}")
    print(header)
    print(sep)

    for pid in pids:
        undelivered = metrics["undelivered_per_id"].get(pid, 0)
        generated   = metrics["generated_per_id"].get(pid, 0)
        reliability = (1 - undelivered / generated) if generated > 0 else 0.0
        req         = id_req.get(pid, float("nan"))
        print(
            f"{pid:<{col_w[0]}}"
            f"{undelivered:>{col_w[1]}.2f}"
            f"{generated:>{col_w[2]}.2f}"
            f"{reliability:>{col_w[3]}.2f}"
            f"{req:>{col_w[4]}.2f}"
            f"{average_power:>{col_w[5]}.2f}"
            f"{total_energy:>{col_w[6]}.2f}"
        )

    print(sep)
    print(f"  (Avg power and energy are per-simulation, shared across all packet IDs)")

# ── Comparison table ──────────────────────────────────────────────────────────

def print_comparison_table(all_results: list[tuple[str, dict]]):
    name_w = max(len(name) for name, _ in all_results) + 2
    col_w  = 16
    # The energy column carries a 20-char header, so it gets its own width.
    e_w    = 22

    header = (
        f"{'Test':<{name_w}}"
        f"{'Undelivered':>{col_w}}"
        f"{'Generated':>{col_w}}"
        f"{'Dropped':>{col_w}}"
        f"{'Transmissions':>{col_w}}"
        f"{'Met Criteria':>{col_w}}"
        f"{'Avg Pwr (W)':>{col_w}}"
        f"{'Energy (W·time-slot)':>{e_w}}"
    )
    sep = "-" * len(header)

    print(f"\n{'═' * len(header)}")
    print("  Scheduler Comparison")
    print(f"{'═' * len(header)}")
    print(header)
    print(sep)

    for name, m in all_results:
        tx_power      = m["tx_power"]
        avg_power     = sum(tx_power) / len(tx_power) if tx_power else 0.0  # W
        total_energy  = sum(tx_power)                                       # W·time-slot
        total_undel   = sum(m["undelivered_per_id"].values())
        total_gen     = sum(m["generated_per_id"].values())
        total_dropped = sum(m["cumulative_dropped"][-1:] or [0])

        pids     = sorted(m["per_id_req"].keys())
        met      = sum(
            1 for pid in pids
            if m["generated_per_id"].get(pid, 0) > 0
            and (1 - m["undelivered_per_id"].get(pid, 0) / m["generated_per_id"][pid]) >= m["per_id_req"][pid]
        )
        criteria = f"{met}/{len(pids)}"

        print(
            f"{name:<{name_w}}"
            f"{total_undel:>{col_w}.2f}"
            f"{total_gen:>{col_w}.2f}"
            f"{total_dropped:>{col_w}.2f}"
            f"{m['total_transmissions']:>{col_w}.2f}"
            f"{criteria:>{col_w}}"
            f"{avg_power:>{col_w}.2f}"
            f"{total_energy:>{e_w}.2f}"
        )

    print(sep)

# ── Reliability criteria table ────────────────────────────────────────────────

def print_reliability_criteria_table(all_results: list[tuple[str, dict]]):
    name_w = max(len(name) for name, _ in all_results) + 2
    id_w   = 6
    col_w  = 18

    header = (
        f"{'Test':<{name_w}}"
        f"{'ID':<{id_w}}"
        f"{'Reliability':>{col_w}}"
        f"{'Reliability Req':>{col_w}}"
        f"{'Delta':>{col_w}}"
        f"{'Met':>{col_w}}"
    )
    sep = "-" * len(header)

    print(f"\n{'═' * len(header)}")
    print("  Reliability Criteria per Test/ID")
    print(f"{'═' * len(header)}")
    print(header)
    print(sep)

    for name, m in all_results:
        pids = sorted(m["per_id_req"].keys())
        for pid in pids:
            generated   = m["generated_per_id"].get(pid, 0)
            undelivered = m["undelivered_per_id"].get(pid, 0)
            reliability = (1 - undelivered / generated) if generated > 0 else 0.0
            req         = m["per_id_req"][pid]
            delta       = reliability - req
            met         = "YES" if reliability >= req else "NO"
            print(
                f"{name:<{name_w}}"
                f"{pid:<{id_w}}"
                f"{reliability:>{col_w}.2f}"
                f"{req:>{col_w}.2f}"
                f"{delta:>+{col_w}.2f}"
                f"{met:>{col_w}}"
            )
        print(sep)

# ── Schedulability sweep ──────────────────────────────────────────────────────

# Each sweep scenario fixes (n, c_min, c_max, rr_min, rr_max); U is swept.
SWEEP_SCENARIOS = [
    (4,  1, 3, 0.50, 0.90),
    (10, 1, 3, 0.50, 0.90),
    (20, 1, 3, 0.50, 0.90),
]

U_VALUES = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]

# Prediction-error levels probed by the `error_sweep` mode. The first value
# must be 0.0 so the Rate-Monotonic curve (predictor-independent) can be
# borrowed across all error levels without re-running it.
PREDICT_ERRORS = [0.0, 0.15, 0.30]

def schedulability_ratio(metrics: dict) -> float:
    """Fraction of packet IDs whose final success ratio met the requirement."""
    pids = sorted(metrics["per_id_req"].keys())
    if not pids:
        return 0.0
    met = 0
    for pid in pids:
        gen = metrics["generated_per_id"].get(pid, 0)
        if gen <= 0:
            continue
        ratio = 1 - metrics["undelivered_per_id"].get(pid, 0) / gen
        if ratio >= metrics["per_id_req"][pid]:
            met += 1
    return met / len(pids)

# ── Parallel-safe single run ──────────────────────────────────────────────────

def _run_single(args: tuple) -> dict:
    """Run one simulation in a temp file pair and return extracted metrics.
    Designed to be called from a worker process. Resolves UUniFast specs
    fresh per call so rounding drift averages over runs. When a seed is
    supplied it is used to make both the task draw and the C++ RNGs
    deterministic for this single sim."""
    test, run_id, seed = args
    config = make_config(test, seed=seed)
    cfg_file = f"/tmp/sim_config_{os.getpid()}_{run_id}.json"
    log_file = f"/tmp/sim_log_{os.getpid()}_{run_id}.json"
    try:
        with open(cfg_file, "w") as f:
            json.dump(config, f)
        result = subprocess.run([BINARY, cfg_file, log_file],
                                capture_output=True, check=False,
                                cwd=SCRIPT_DIR)
        if result.returncode != 0 or not os.path.exists(log_file):
            raise RuntimeError(
                f"{BINARY} failed (exit={result.returncode}) "
                f"for run {run_id}.\n"
                f"stderr: {result.stderr.decode(errors='replace')[-2000:]}\n"
                f"stdout: {result.stdout.decode(errors='replace')[-500:]}"
            )
        frames = load_results(log_file)
        return extract_metrics(frames, config)
    finally:
        for path in (cfg_file, log_file):
            try:
                os.remove(path)
            except FileNotFoundError:
                pass

def window_m(req: float, k: int) -> int:
    """Smallest m with m/k >= req — the (m,k)-firm threshold for one task.

    Mirrors the C++ computation in main.cpp exactly, epsilon included. The
    nudge matters: `reliability` is drawn as a 2-decimal float, and a bare
    ceil overshoots by one wherever that float rounds up. math.ceil(0.55*100)
    is 56 and math.ceil(0.56*100) is 57, both inside the sweep's RR band,
    which would hold those tasks to a stricter rate than they requested.
    """
    return math.ceil(req * k - 1e-9)

def warn_window_k(scenarios: list) -> None:
    """Warn when the configured (m,k) window is too short for the strictest
    reliability requirement in the scenario set. At k < 1/(1-req) we get
    m = ceil(req*k) == k, i.e. the window silently demands zero misses —
    a much harder constraint than the long-run `req` it was derived from."""
    k = BASE_SIM.get("window_k", 100)
    if k <= 0:
        print("window_k <= 0 — (m,k) check disabled; sched_ratio_mk will be 1.0")
        return
    rr_max = max(s[4] for s in scenarios)
    if window_m(rr_max, k) >= k:
        # Smallest k admitting one miss. Searched on the same ceil the C++ side
        # uses rather than 1/(1-req), which floating point rounds up by one at
        # req=0.9 (1/(1-0.9) == 10.000000000000002).
        floor_k = next(kk for kk in range(2, 10001) if window_m(rr_max, kk) < kk)
        print(f"WARNING: window_k={k} is too short for a reliability requirement up to {rr_max} "
              f"— m == k, so those tasks are held to zero misses. "
              f"Use window_k >= {floor_k} (WINDOW_K env var).")

def extract_sweep_metrics(summary: dict) -> dict:
    """Turn a C++ summary block into the per-run schedulability metrics.

    Three views of the same run, from most to least forgiving:

    - `sched_ratio`     — fraction of ids whose whole-run delivery rate meets
                          reliability_req. Insensitive to *when* misses land:
                          an early burst can be averaged away by a long clean
                          tail.
    - `sched_ratio_mk`  — fraction of ids meeting the weakly-hard (m,k)-firm
                          constraint, i.e. at least m = ceil(req * k) of every
                          k consecutive instances (sliding). Burst-sensitive;
                          converges to sched_ratio as k grows.
    - `max_burst`       — longest run of consecutive undelivered instances over
                          all ids. Parameter-free, so it can't be tuned.

    An id with fewer than k instances has no complete window and is counted as
    vacuously met — never as a violation. Same for ids the simulator never
    touched (`generated == 0`), which are likewise not failures.
    """
    per_id = summary["per_id"]
    if not per_id:
        return {"sched_ratio": 0.0, "sched_ratio_mk": 0.0,
                "max_burst": 0.0, "window_violation_rate": 0.0}

    met = mk_met = max_burst = vacuous = 0
    tot_windows = tot_violations = 0
    for entry in per_id:
        gen = entry["generated"]
        if gen > 0 and 1 - entry["undelivered"] / gen >= entry["reliability_req"]:
            met += 1

        windows    = entry.get("windows_total", 0)
        violations = entry.get("window_violations", 0)
        tot_windows    += windows
        tot_violations += violations
        if windows == 0:
            vacuous += 1
        if windows == 0 or violations == 0:
            mk_met += 1
        max_burst = max(max_burst, entry.get("max_consecutive_misses", 0))

    return {
        "sched_ratio":           met / len(per_id),
        "sched_ratio_mk":        mk_met / len(per_id),
        "max_burst":             float(max_burst),
        "window_violation_rate": (tot_violations / tot_windows) if tot_windows else 0.0,
        # Share of ids with fewer than k instances, hence no complete window.
        # These are scored vacuously met, so they inflate sched_ratio_mk — the
        # one way sched_ratio_mk can exceed sched_ratio. Reported so a run can
        # tell you when the window is too long for the task set.
        "vacuous_rate":          vacuous / len(per_id),
    }

# Metrics averaged across runs, each with 10th/90th-percentile bands. Keys must
# exist in every dict returned by _run_single_sweep. The bands are kept in the
# results dict for offline reading only — neither figure plots them any more.
SWEEP_METRICS = ("sched_ratio", "sched_ratio_mk", "max_burst",
                 "window_violation_rate", "vacuous_rate", "total_energy")

# Above this share of ids having no complete (m,k) window, sched_ratio_mk is
# meaningfully inflated by vacuous passes and the run should say so.
VACUOUS_WARN_THRESHOLD = 0.05

def warn_vacuous(results: dict, depth: int) -> None:
    """Report the worst vacuous_rate over a results grid. `depth` is how many
    dict levels sit above the per-U metrics (2 for run_sweep's
    [scen][sch][U], 3 for run_error_sweep's [err][scen][sch][U])."""
    def leaves(node, d):
        if d == 0:
            yield from node.values()
            return
        for child in node.values():
            yield from leaves(child, d - 1)

    rates = [m["vacuous_rate"] for m in leaves(results, depth)]
    if rates and max(rates) > VACUOUS_WARN_THRESHOLD:
        print(f"NOTE: up to {100 * max(rates):.1f}% of tasks had fewer than "
              f"k={BASE_SIM.get('window_k', 100)} instances at some grid point, so they "
              f"pass the (m,k) check vacuously and inflate the windowed curve. "
              f"Lower WINDOW_K or raise BASE_SIM['duration'] to shrink this.")

def _percentile(xs: list, p: float) -> float:
    s = sorted(xs)
    if not s:
        return 0.0
    k = (len(s) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)

def aggregate_runs(runs: list) -> dict:
    """Mean + 10/90 band for every metric in SWEEP_METRICS. Shared by
    run_sweep and run_error_sweep so the two can't drift apart."""
    out = {}
    for key in SWEEP_METRICS:
        vals = [r[key] for r in runs]
        out[key]            = sum(vals) / len(vals)
        out[f"{key}_lo"]    = _percentile(vals, 10)
        out[f"{key}_hi"]    = _percentile(vals, 90)
    return out

def _run_single_sweep(args: tuple) -> dict:
    """Sweep-mode worker: invokes main.o in 'summary' mode so the C++ side
    silences stdout, skips per-frame JSON building, and emits only a tiny
    aggregate (per-id generated/undelivered counts + total tx_power).
    Bypasses extract_metrics entirely — at 100k ticks this was the dominant
    cost of the sweep pipeline.

    `total_energy` is sum(tx_power) over all transmitted time-slots, in units
    of W·time-slot. Dimensionally equivalent to energy modulo the (unspecified)
    time-slot duration in seconds — same constant for every scheduler, so
    comparisons are unchanged."""
    test, run_id, seed = args
    config = make_config(test, seed=seed)
    cfg_file = f"/tmp/sim_config_{os.getpid()}_{run_id}.json"
    log_file = f"/tmp/sim_log_{os.getpid()}_{run_id}.json"
    try:
        with open(cfg_file, "w") as f:
            json.dump(config, f)
        result = subprocess.run([BINARY, cfg_file, log_file, "summary"],
                                capture_output=True, check=False,
                                cwd=SCRIPT_DIR)
        if result.returncode != 0 or not os.path.exists(log_file):
            raise RuntimeError(
                f"{BINARY} failed (exit={result.returncode}) "
                f"for run {run_id}.\n"
                f"stderr: {result.stderr.decode(errors='replace')[-2000:]}\n"
                f"stdout: {result.stdout.decode(errors='replace')[-500:]}"
            )
        with open(log_file) as f:
            summary = json.load(f)

        return dict(extract_sweep_metrics(summary),
                    total_energy=float(summary["total_tx_power"]))
    finally:
        for path in (cfg_file, log_file):
            try:
                os.remove(path)
            except FileNotFoundError:
                pass

# ── Multi-run averaging ───────────────────────────────────────────────────────

def average_metrics(runs: list[dict]) -> dict:
    """Average scalar table metrics across runs; use last run for plot data."""
    n    = len(runs)
    last = runs[-1]

    # Collect all pids seen across runs
    all_pids = set()
    for m in runs:
        all_pids |= set(m["undelivered_per_id"].keys())
        all_pids |= set(m["generated_per_id"].keys())

    undelivered_per_id = {
        pid: sum(m["undelivered_per_id"].get(pid, 0) for m in runs) / n
        for pid in all_pids
    }
    generated_per_id = {
        pid: sum(m["generated_per_id"].get(pid, 0) for m in runs) / n
        for pid in all_pids
    }
    total_transmissions = sum(m["total_transmissions"] for m in runs) / n
    total_dropped_final = sum(m["cumulative_dropped"][-1] if m["cumulative_dropped"] else 0 for m in runs) / n

    # For power: average the per-run tx_power lists element-wise (same length assumed)
    avg_tx_power = [
        sum(m["tx_power"][i] for m in runs) / n
        for i in range(len(last["tx_power"]))
    ]

    # Reliability requirements vary per run now (drawn from rr_min..rr_max),
    # so average them per-id alongside the other metrics.
    per_id_req = {
        pid: sum(m["per_id_req"].get(pid, 0) for m in runs) / n
        for pid in all_pids
    }

    averaged = dict(last)  # copy plot data from last run
    averaged["undelivered_per_id"]  = undelivered_per_id
    averaged["generated_per_id"]    = generated_per_id
    averaged["per_id_req"]          = per_id_req
    averaged["total_transmissions"] = total_transmissions
    averaged["tx_power"]            = avg_tx_power
    # Patch cumulative_dropped final value used in comparison table
    averaged["cumulative_dropped"]  = last["cumulative_dropped"][:-1] + [total_dropped_final]

    return averaged

# ── Sweep runner ──────────────────────────────────────────────────────────────

def run_sweep(n_runs: int, n_workers: int) -> dict:
    """For each (scenario, U, scheduler), run n_runs sims in a single big pool.
    Returns nested dict results[scen_name][sch_name][U] = avg_schedulability."""
    jobs = []
    for n, c_min, c_max, rr_min, rr_max in SWEEP_SCENARIOS:
        scen_name = scenario_label(n, c_min, c_max, rr_min, rr_max)
        for U in U_VALUES:
            for sch_name, scheduler in SCHEDULERS:
                test = {
                    "name": f"{scen_name}_U={U:.2f}_{sch_name}",
                    "config": {
                        "simulation": BASE_SIM,
                        "scheduler":  scheduler,
                        "channels":   BASE_CHANNELS,
                    },
                    "uunifast": {
                        "U":      U, "n":      n,
                        "c_min":  c_min, "c_max":  c_max,
                        "rr_min": rr_min, "rr_max": rr_max,
                    },
                }
                for run_idx in range(n_runs):
                    sim_seed = (derive_seed(MASTER_SEED, test["name"], run_idx)
                                if MASTER_SEED is not None else None)
                    jobs.append(((scen_name, U, sch_name), test, run_idx, sim_seed))

    total = len(jobs)
    print(f"  [sweep] dispatching {total} sims "
          f"({len(SWEEP_SCENARIOS)} scen x {len(U_VALUES)} U x "
          f"{len(SCHEDULERS)} sch x {n_runs} runs)", flush=True)

    # Use spawn so child interpreters start clean — without 'spawn', forked
    # workers inherit the parent's matplotlib/numpy state via COW which gets
    # ref-counted into real allocations very quickly, doubling resident memory.
    ctx = mp.get_context("spawn")

    # Keep a bounded number of futures in flight (~4*workers). Submitting all
    # 45k up front pickles the args into the internal queue eagerly, adding
    # tens of MB of dead weight to the parent for the whole run.
    window = max(4 * n_workers, n_workers + 8)

    combo_runs = defaultdict(list)
    with ProcessPoolExecutor(max_workers=n_workers, mp_context=ctx) as executor:
        in_flight = {}   # future -> key
        job_iter  = iter(jobs)
        done = 0

        # Prime the window.
        for _ in range(min(window, total)):
            try:
                key, test, _run_idx, sim_seed = next(job_iter)
            except StopIteration:
                break
            fut = executor.submit(_run_single_sweep, (test, done + len(in_flight), sim_seed))
            in_flight[fut] = key

        while in_flight:
            finished, _ = wait(in_flight, return_when=FIRST_COMPLETED)
            for fut in finished:
                key = in_flight.pop(fut)
                combo_runs[key].append(fut.result())
                done += 1
                if done % max(1, total // 50) == 0 or done == total:
                    print(f"  [sweep] {done}/{total} done", end="\r", flush=True)
                # Top up the window with the next job, if any.
                try:
                    key, test, _run_idx, sim_seed = next(job_iter)
                except StopIteration:
                    continue
                fut2 = executor.submit(_run_single_sweep,
                                       (test, done + len(in_flight), sim_seed))
                in_flight[fut2] = key
    print()

    results = {scenario_label(*scen): {sch_name: {} for sch_name, _ in SCHEDULERS}
               for scen in SWEEP_SCENARIOS}
    for (scen_name, U, sch_name), runs in combo_runs.items():
        results[scen_name][sch_name][U] = aggregate_runs(runs)
    warn_vacuous(results, 2)
    return results

# Rows drawn by both the sweep and error-sweep figures: (metric key, y label,
# y limits or None to autoscale). Order here is the row order in every figure.
#
# `max_burst` is deliberately not a row: it is still computed and aggregated
# (see SWEEP_METRICS) and still worth reading from the raw results, but its
# scale spans three orders of magnitude across U, which flattens the
# low-utilization end into an unreadable line on a shared axis.
PANEL_METRICS = [
    ("sched_ratio",    "Schedulability ratio\n(met / total)",   (-0.05, 1.05)),
    ("sched_ratio_mk", "Windowed schedulability\n"
                       f"(({SYM_M}, {SYM_KWIN}), met / total)", (-0.05, 1.05)),
    ("total_energy",   "Energy (W·time-slot)",                  None),
]

def _legend_ncol(labels, per_row: int) -> int:
    """How many legend columns fit. Width is judged on the *rendered* label:
    `$P_{\\mathrm{CH}}$` is 17 characters of markup that draws as about three
    glyphs, so counting the raw string would push every roster into one
    column. `per_row` is how many of the *short* labels fit across."""
    longest = max(len(re.sub(r"\$[^$]*\$", "xxx", l)) for l in labels)
    return per_row if longest <= 22 else max(2, per_row - 1)

def _grouped_legend_columns(handles, labels, key):
    """Re-order legend entries so that each scheduler gets its own column.

    Matplotlib fills a multi-column legend *column-major*, i.e. consecutive
    entries go down a column before starting the next. That is what the error
    sweep wants — a scheduler's three error levels stacked in one column — but
    only if every column holds the same number of entries. The two
    predictor-independent schedulers contribute one curve each, so without
    padding the counts drift and a CHEDF entry lands under the EDFs.

    Groups are `key(label)`-equal runs, padded to the tallest group with blank
    entries. Adjacent one-entry groups (the fixed-power baselines, which have
    no error levels) share a column rather than each claiming one. Returns the
    flattened entries plus the column count to pass as `ncol`."""
    groups: list[list[tuple]] = []
    keys:   list[str] = []
    for h, l in zip(handles, labels):
        k = key(l)
        if keys and keys[-1] == k:
            groups[-1].append((h, l))
        else:
            groups.append([(h, l)])
            keys.append(k)

    rows = max(len(g) for g in groups)
    cols: list[list[tuple]] = []
    prev_single = False
    for g in groups:
        if len(g) == 1 and prev_single and len(cols[-1]) < rows:
            cols[-1].extend(g)
        else:
            cols.append(list(g))
        prev_single = len(g) == 1

    out_h, out_l = [], []
    for col in cols:
        for h, l in col:
            out_h.append(h)
            out_l.append(l)
        for _ in range(rows - len(col)):          # blank filler, draws nothing
            out_h.append(Line2D([], [], linestyle="none"))
            out_l.append("")
    return out_h, out_l, len(cols)

def _title_and_legend(fig, ax_src, title: str, per_row: int, group_key=None) -> None:
    """Lay out a sweep figure: suptitle on top, axes, then one legend for the
    whole figure along the bottom.

    One legend, not one per axis: with 9 curves (11 on the error sweep) an
    in-axis box covers the curves it labels, and every row (and on the
    combined figures every column) plots the same roster, so repeating it just
    spends the space again. Entries come from `ax_src`, which carries them all.

    Both strips are measured, not guessed — legend row count x font size, and
    the suptitle line — and handed to tight_layout as `rect`, so the axes fit
    between them however many rows the legend needs.

    `group_key` switches on the grouped column layout described in
    `_grouped_legend_columns`; without it the entries just flow into as many
    columns as fit."""
    handles, labels = ax_src.get_legend_handles_labels()
    if not handles:
        fig.tight_layout()
        fig.suptitle(title)
        return
    if group_key is not None:
        handles, labels, ncol = _grouped_legend_columns(handles, labels, group_key)
    else:
        ncol = _legend_ncol(labels, per_row)
    leg_rows  = math.ceil(len(labels) / ncol)
    fig_h     = fig.get_figheight()
    # Inches, converting from points at 72 pt/in, with a little padding.
    title_in  = plt.rcParams["figure.titlesize"] * 2.0 / 72
    legend_in = leg_rows * plt.rcParams["legend.fontsize"] * 1.55 / 72 + 0.12

    fig.tight_layout(rect=(0, legend_in / fig_h, 1, 1 - title_in / fig_h))
    fig.suptitle(title, y=1 - 0.35 * title_in / fig_h)
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, 0.0),
               ncol=ncol, frameon=False, columnspacing=1.6, handlelength=2.6)

def _draw_scenario_panel(axes, scen_name: str, sch_results: dict):
    """Render one column of the sweep figure — one row per PANEL_METRICS entry
    — onto pre-existing axes. Shared between the combined plot and the
    per-scenario plots so they stay in sync. `axes` must be indexable with at
    least len(PANEL_METRICS) entries."""
    # tab10 holds 10 colors, so a roster longer than that wraps and gives two
    # curves the same color. tab20 is used only past that point, which keeps
    # the <=10-curve figures on exactly the colors they have always had; its
    # entries are dark/light pairs of one hue, so with the roster ordered by
    # family a scheduler's 10 W and 25 W curves come out as two shades of the
    # same color.
    colors     = plt.cm.tab10.colors if len(sch_results) <= 10 else plt.cm.tab20.colors
    # One distinct linestyle per scheduler — the list must be at least as long
    # as the roster or two curves end up sharing a style.
    linestyles = ["-", "--", "-.", ":", (0, (3, 1, 1, 1)), (0, (5, 1)),
                  (0, (1, 1)), (0, (7, 2, 1, 2)), (0, (3, 1, 1, 1, 1, 1)),
                  (0, (5, 2, 1, 2)), (0, (2, 2)), (0, (8, 2)),
                  (0, (1, 2, 4, 2))]
    markers    = ["o", "s", "^", "D", "v", "P", "X", "*", "h",
                  "<", ">", "p", "d"]

    # Only the mean is drawn. aggregate_runs still records the 10/90 band per
    # metric (`<key>_lo` / `<key>_hi`) in the results dict, but the bands were
    # overlapping into an unreadable smear with a roster this size, so read
    # them from the raw results rather than the figure.
    for i, (sch_name, u_to_metrics) in enumerate(sch_results.items()):
        xs    = sorted(u_to_metrics.keys())
        color = colors[i % len(colors)]
        ls    = linestyles[i % len(linestyles)]
        mk    = markers[i % len(markers)]
        for row, (key, _label, _ylim) in enumerate(PANEL_METRICS):
            axes[row].plot(xs, [u_to_metrics[u][key] for u in xs], color=color,
                           linestyle=ls, marker=mk, markersize=7,
                           linewidth=1.5, label=scheduler_title(sch_name))

    for row, (_key, _label, ylim) in enumerate(PANEL_METRICS):
        if ylim is not None:
            axes[row].set_ylim(*ylim)
        axes[row].grid(True)
    axes[0].set_title(scenario_title(scen_name))
    axes[len(PANEL_METRICS) - 1].set_xlabel(f"Utilization {SYM_U}")

def _safe_filename(s: str) -> str:
    """Make a scenario label safe to use as a filename component."""
    return s.replace("[", "").replace("]", "").replace(",", "_").replace("=", "")

def _window_k_suffix() -> str:
    """" (K_win=N)" for figures that actually draw the windowed row, empty
    otherwise — hf_experiment/run.py drops that row, and naming a window
    length the figure never uses only invites the reader to look for it. Only
    the value goes in the title; the (m, K_win) pair is already on that row's
    y-axis, and the scenario label makes these titles long enough."""
    if not any(key == "sched_ratio_mk" for key, _l, _y in PANEL_METRICS):
        return ""
    return f" ({SYM_KWIN}={BASE_SIM.get('window_k', 100)})"

def plot_schedulability(results: dict):
    scen_names = list(results.keys())
    n_scen     = len(scen_names)
    os.makedirs(TESTS_DIR, exist_ok=True)

    n_rows = len(PANEL_METRICS)
    title  = f"Schedulability and Energy vs Utilization{_window_k_suffix()}"

    # Combined figure: one column per scenario, one row per PANEL_METRICS entry.
    fig, axes = plt.subplots(n_rows, n_scen, figsize=(6 * n_scen, 4.2 * n_rows),
                             sharex="col")
    if n_scen == 1:
        axes = axes.reshape(n_rows, 1)
    for col, scen_name in enumerate(scen_names):
        _draw_scenario_panel(axes[:, col], scen_name, results[scen_name])
    for row, (_key, label, _ylim) in enumerate(PANEL_METRICS):
        axes[row, 0].set_ylabel(label)
    _title_and_legend(fig, axes[0, 0], title, per_row=5)
    out = save_figure(os.path.join(TESTS_DIR, "results_schedulability"))
    plt.close()
    print(f"Schedulability plot saved to {out}")

    # Per-scenario figures: same layout, one file per format each.
    for scen_name in scen_names:
        fig, axs = plt.subplots(n_rows, 1, figsize=(7, 4.2 * n_rows), sharex=True)
        _draw_scenario_panel(axs, scen_name, results[scen_name])
        # The panel titles each column for the combined figure; here the
        # scenario is already in the suptitle, so drop the duplicate.
        axs[0].set_title("")
        for row, (_key, label, _ylim) in enumerate(PANEL_METRICS):
            axs[row].set_ylabel(label)
        _title_and_legend(fig, axs[0], f"{title} — {scenario_title(scen_name)}",
                          per_row=3)
        out = save_figure(os.path.join(TESTS_DIR,
                                       f"results_schedulability_{_safe_filename(scen_name)}"))
        plt.close()
        print(f"Schedulability plot saved to {out}")

# ── Prediction-error sweep ────────────────────────────────────────────────────

def run_error_sweep(n_runs: int, n_workers: int) -> dict:
    """Run the schedulability sweep once per predict_error level in
    PREDICT_ERRORS. Predictor-independent schedulers ignore the predictor, so
    they are dispatched only at PREDICT_ERRORS[0] (must be 0.0) and replicated
    across the other levels when assembling the result dict. Returns
    results[err][scen_name][sch_name][U] = metrics.

    Iterates ERROR_SWEEP_SCHEDULERS, not SCHEDULERS — this figure carries a
    trimmed roster because every predictor-sensitive scheduler contributes one
    curve per error level."""
    assert PREDICT_ERRORS[0] == 0.0, "PREDICT_ERRORS[0] must be 0.0"

    jobs = []
    for err in PREDICT_ERRORS:
        sim_block = dict(BASE_SIM, predict_error=err)
        for n, c_min, c_max, rr_min, rr_max in SWEEP_SCENARIOS:
            scen_name = scenario_label(n, c_min, c_max, rr_min, rr_max)
            for U in U_VALUES:
                for sch_name, scheduler in ERROR_SWEEP_SCHEDULERS:
                    # Predictor-independent schedulers (RM) — run them once.
                    if is_predictor_independent(scheduler) and err != PREDICT_ERRORS[0]:
                        continue
                    test = {
                        "name": f"err={err:.2f}_{scen_name}_U={U:.2f}_{sch_name}",
                        "config": {
                            "simulation": sim_block,
                            "scheduler":  scheduler,
                            "channels":   BASE_CHANNELS,
                        },
                        "uunifast": {
                            "U":      U, "n":      n,
                            "c_min":  c_min, "c_max":  c_max,
                            "rr_min": rr_min, "rr_max": rr_max,
                        },
                    }
                    for run_idx in range(n_runs):
                        sim_seed = (derive_seed(MASTER_SEED, test["name"], run_idx)
                                    if MASTER_SEED is not None else None)
                        jobs.append(((err, scen_name, U, sch_name), test, run_idx, sim_seed))

    total = len(jobs)
    print(f"  [error_sweep] dispatching {total} sims "
          f"({len(PREDICT_ERRORS)} err x {len(SWEEP_SCENARIOS)} scen x "
          f"{len(U_VALUES)} U x {len(ERROR_SWEEP_SCHEDULERS)} sch x {n_runs} runs; "
          f"predictor-independent dedup'd)", flush=True)

    ctx     = mp.get_context("spawn")
    window  = max(4 * n_workers, n_workers + 8)
    combo_runs = defaultdict(list)
    with ProcessPoolExecutor(max_workers=n_workers, mp_context=ctx) as executor:
        in_flight = {}
        job_iter  = iter(jobs)
        done = 0
        for _ in range(min(window, total)):
            try:
                key, test, _run_idx, sim_seed = next(job_iter)
            except StopIteration:
                break
            fut = executor.submit(_run_single_sweep, (test, done + len(in_flight), sim_seed))
            in_flight[fut] = key
        while in_flight:
            finished, _ = wait(in_flight, return_when=FIRST_COMPLETED)
            for fut in finished:
                key = in_flight.pop(fut)
                combo_runs[key].append(fut.result())
                done += 1
                if done % max(1, total // 50) == 0 or done == total:
                    print(f"  [error_sweep] {done}/{total} done", end="\r", flush=True)
                try:
                    key, test, _run_idx, sim_seed = next(job_iter)
                except StopIteration:
                    continue
                fut2 = executor.submit(_run_single_sweep,
                                       (test, done + len(in_flight), sim_seed))
                in_flight[fut2] = key
    print()

    results = {err: {scenario_label(*scen): {sch_name: {} for sch_name, _ in ERROR_SWEEP_SCHEDULERS}
                     for scen in SWEEP_SCENARIOS}
               for err in PREDICT_ERRORS}
    for (err, scen_name, U, sch_name), runs in combo_runs.items():
        results[err][scen_name][sch_name][U] = aggregate_runs(runs)
    # Replicate the predictor-independent schedulers across the non-zero
    # error levels — they were only dispatched at PREDICT_ERRORS[0].
    base_err = PREDICT_ERRORS[0]
    for sch_name, scheduler in ERROR_SWEEP_SCHEDULERS:
        if not is_predictor_independent(scheduler):
            continue
        for err in PREDICT_ERRORS[1:]:
            for scen_name in results[err]:
                results[err][scen_name][sch_name] = results[base_err][scen_name][sch_name]
    warn_vacuous(results, 3)
    return results

def _error_sweep_group(label: str) -> str:
    """Legend grouping key for the error sweep: the label with its error term
    stripped, so a scheduler's three ε curves group together. Reads the labels
    `scheduler_title(sch_name, "ε = …")` produces, where the ε term follows a
    comma for a fixed-power scheduler (`CHEDF (P_CH = 25 W, ε = 0.15)`) but an
    open bracket for CATS, which has no power term (`CATS (ε = 0.15)`) — hence
    the split on either. The two must stay in step; a label whose ε term were
    formatted differently would give that curve its own column, not a wrong
    one."""
    return re.split(r",? ?" + re.escape(SYM_ERR), label)[0].rstrip(" (")

def _draw_error_sweep_panel(axes, scen_name: str, err_to_sch: dict):
    """Draw one scenario column of the error-sweep figure — one row per
    PANEL_METRICS entry. Color/marker are fixed per scheduler so the curves
    match the other plots; linestyle varies with prediction_error.
    Predictor-independent schedulers (EDF, SEDF) are drawn once — CHEDF and
    CATS get one curve per error level."""
    colors  = plt.cm.tab10.colors
    markers = ["o", "s", "^", "D", "v", "P", "X", "*", "h"]
    err_linestyles = {err: ls for err, ls in zip(PREDICT_ERRORS, ["-", "--", "-.", ":"])}

    # Stable scheduler→(color, marker) mapping derived from roster order.
    sch_style = {sch_name: (colors[i % len(colors)], markers[i % len(markers)])
                 for i, (sch_name, _) in enumerate(ERROR_SWEEP_SCHEDULERS)}

    base_err = PREDICT_ERRORS[0]

    # Predictor-independent schedulers first — one solid curve each.
    for sch_name, scheduler in ERROR_SWEEP_SCHEDULERS:
        if not is_predictor_independent(scheduler):
            continue
        u_to_m = err_to_sch[base_err].get(sch_name, {})
        if not u_to_m:
            continue
        color, mk = sch_style[sch_name]
        xs = sorted(u_to_m.keys())
        for row, (key, _label, _ylim) in enumerate(PANEL_METRICS):
            axes[row].plot(xs, [u_to_m[u][key] for u in xs], color=color,
                           linestyle="-", marker=mk, markersize=7,
                           linewidth=1.5, label=scheduler_title(sch_name))

    # Predictor-sensitive schedulers — one curve per error level, linestyle
    # differentiates.
    for sch_name in (s for s, cfg in ERROR_SWEEP_SCHEDULERS if not is_predictor_independent(cfg)):
        color, mk = sch_style[sch_name]
        for err in PREDICT_ERRORS:
            u_to_m = err_to_sch[err].get(sch_name, {})
            if not u_to_m:
                continue
            xs    = sorted(u_to_m.keys())
            ls    = err_linestyles[err]
            label = scheduler_title(sch_name, f"{SYM_ERR} = {err:.2f}")
            for row, (key, _label, _ylim) in enumerate(PANEL_METRICS):
                axes[row].plot(xs, [u_to_m[u][key] for u in xs], color=color,
                               linestyle=ls, marker=mk, markersize=6,
                               linewidth=1.4, label=label)

    for row, (_key, _label, ylim) in enumerate(PANEL_METRICS):
        if ylim is not None:
            axes[row].set_ylim(*ylim)
        axes[row].grid(True)
    axes[0].set_title(scenario_title(scen_name))
    axes[len(PANEL_METRICS) - 1].set_xlabel(f"Utilization {SYM_U}")

def plot_error_sweep(results: dict):
    """Combined + per-scenario figures for the prediction-error sweep.
    `results` is indexed [err][scen_name][sch_name][U]."""
    scen_names = list(next(iter(results.values())).keys())
    n_scen     = len(scen_names)
    os.makedirs(TESTS_DIR, exist_ok=True)

    # err_to_sch_by_scenario[scen_name][err][sch_name] = u_to_metrics
    by_scen = {scen: {err: results[err][scen] for err in PREDICT_ERRORS}
               for scen in scen_names}

    n_rows = len(PANEL_METRICS)
    title  = f"Prediction-Error Sweep{_window_k_suffix()}"

    fig, axes = plt.subplots(n_rows, n_scen, figsize=(6 * n_scen, 4.2 * n_rows),
                             sharex="col")
    if n_scen == 1:
        axes = axes.reshape(n_rows, 1)
    for col, scen_name in enumerate(scen_names):
        _draw_error_sweep_panel(axes[:, col], scen_name, by_scen[scen_name])
    for row, (_key, label, _ylim) in enumerate(PANEL_METRICS):
        axes[row, 0].set_ylabel(label)
    _title_and_legend(fig, axes[0, 0],
                      f"Schedulability and Energy vs Utilization — {title}",
                      per_row=5, group_key=_error_sweep_group)
    out = save_figure(os.path.join(TESTS_DIR, "results_error_sweep"))
    plt.close()
    print(f"Error-sweep plot saved to {out}")

    for scen_name in scen_names:
        fig, axs = plt.subplots(n_rows, 1, figsize=(7, 4.2 * n_rows), sharex=True)
        _draw_error_sweep_panel(axs, scen_name, by_scen[scen_name])
        axs[0].set_title("")   # already in the suptitle
        for row, (_key, label, _ylim) in enumerate(PANEL_METRICS):
            axs[row].set_ylabel(label)
        _title_and_legend(fig, axs[0], f"{title} — {scenario_title(scen_name)}",
                          per_row=3, group_key=_error_sweep_group)
        out = save_figure(os.path.join(TESTS_DIR,
                                       f"results_error_sweep_{_safe_filename(scen_name)}"))
        plt.close()
        print(f"Error-sweep plot saved to {out}")

# ── Experiment metadata ───────────────────────────────────────────────────────

def write_experiment_params(mode: str, n_runs: int, run_name: Optional[str]) -> None:
    """Dump every input that affects the simulation to a JSON file in cwd
    (which the caller has already chdir'd to tests/<run_name>). Captures
    scheduler configs (post CLI overrides), scenarios relevant to the mode,
    BASE_SIM, channels, the master seed if set, and the current git commit.
    Together with the matching SEED this is enough to re-run identically."""
    params = {
        "run_name":    run_name,
        "mode":        mode,
        "n_runs":      n_runs,
        "master_seed": MASTER_SEED,
        "started_at":  time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "simulation":  BASE_SIM,
        # The two sweep modes iterate different rosters — dump the one this
        # run actually used, or the snapshot won't reproduce the figure.
        "schedulers":  {name: cfg for name, cfg in
                        (ERROR_SWEEP_SCHEDULERS if mode == "error_sweep"
                         else SCHEDULERS)},
        "channels":    BASE_CHANNELS,
    }

    # error_sweep drives SWEEP_SCENARIOS too, so it takes this branch — only
    # `tests` mode runs the fixed-U SCENARIOS list.
    if mode in ("sweep", "error_sweep"):
        params["scenarios"] = {
            "SWEEP_SCENARIOS": [
                {"n": n, "c_min": c_min, "c_max": c_max,
                 "rr_min": rr_min, "rr_max": rr_max}
                for n, c_min, c_max, rr_min, rr_max in SWEEP_SCENARIOS
            ],
            "U_VALUES": U_VALUES,
        }
        if mode == "error_sweep":
            params["predict_errors"] = PREDICT_ERRORS
    else:
        params["scenarios"] = {
            "SCENARIOS": [
                {"U": U, "n": n, "c_min": c_min, "c_max": c_max,
                 "rr_min": rr_min, "rr_max": rr_max}
                for U, n, c_min, c_max, rr_min, rr_max in SCENARIOS
            ],
        }

    # Git commit — adds code-level reproducibility. Suppress failure if not
    # in a git repo or git isn't on PATH; the metadata is still useful.
    try:
        proc = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=SCRIPT_DIR, capture_output=True, text=True, check=False
        )
        commit = proc.stdout.strip()
        if commit:
            params["git_commit"] = commit
        dirty = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=SCRIPT_DIR, capture_output=True, text=True, check=False
        ).stdout.strip()
        if dirty:
            params["git_dirty"] = True
    except FileNotFoundError:
        pass

    out_path = "experiment_params.json"
    with open(out_path, "w") as f:
        json.dump(params, f, indent=2)
    print(f"Experiment parameters saved to {os.path.abspath(out_path)}")

# ── Main ──────────────────────────────────────────────────────────────────────

def format_duration(seconds: float) -> str:
    h, rem = divmod(int(seconds), 3600)
    m, s   = divmod(rem, 60)
    if h:
        return f"{h}h {m}m {s}s"
    if m:
        return f"{m}m {s}s"
    return f"{seconds:.2f}s"

if __name__ == "__main__":
    n_runs                = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    mode                  = sys.argv[2] if len(sys.argv) > 2 else "tests"
    run_name              = sys.argv[3] if len(sys.argv) > 3 else None
    belief_threshold      = float(sys.argv[4]) if len(sys.argv) > 4 else None
    utilization_threshold = float(sys.argv[5]) if len(sys.argv) > 5 else None
    n_workers             = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 1)

    # Fail fast on a misspelled mode. This used to fall through to `tests`,
    # which is the most expensive mode by far — it parses the full ~180 MB log
    # per run in Python — so a typo like `error` for `error_sweep` did not
    # error out, it quietly launched a 200-run tests job and OOM-killed the
    # SLURM step hours later.
    if mode not in MODES:
        sys.exit(f"unknown mode {mode!r} — expected one of {', '.join(MODES)}.\n"
                 f"usage: python run_simulation.py <n_runs> <{'|'.join(MODES)}> "
                 f"<run_name> [belief_threshold] [utilization_threshold]")

    if belief_threshold is not None:
        BASE_CATS_SCHEDULER["belief_threshold"] = belief_threshold
        print(f"CATS belief_threshold overridden to {belief_threshold}")

    if utilization_threshold is not None:
        BASE_CATS_SCHEDULER["utilization_threshold"] = utilization_threshold
        print(f"CATS utilization_threshold overridden to {utilization_threshold}")

    if MASTER_SEED is not None:
        print(f"SEED={MASTER_SEED} — per-sim seeds derived deterministically; "
              f"runs are reproducible")
    else:
        print("SEED unset — runs are non-deterministic "
              "(set SEED=<int> env var for reproducibility)")

    if run_name:
        target = os.path.join(TESTS_DIR, run_name)
        os.makedirs(target, exist_ok=True)
        os.chdir(target)
        TESTS_DIR = "."   # already inside tests/<run_name>, don't nest another tests/
        print(f"Outputs will be written under: {os.path.abspath('.')}")

    # Snapshot every parameter that affects the run. Called after CLI
    # overrides have mutated BASE_CATS_SCHEDULER, so the dump reflects the
    # values actually used.
    write_experiment_params(mode, n_runs, run_name)

    t_start = time.perf_counter()

    if mode == "sweep":
        print(f"Running schedulability sweep ({n_runs} runs/point, {n_workers} workers, "
              f"window_k={BASE_SIM.get('window_k', 100)})")
        warn_window_k(SWEEP_SCENARIOS)
        results = run_sweep(n_runs, n_workers)
        plot_schedulability(results)
        print(f"Total elapsed: {format_duration(time.perf_counter() - t_start)}")
        sys.exit(0)

    if mode == "error_sweep":
        print(f"Running prediction-error sweep ({n_runs} runs/point, "
              f"{n_workers} workers, errors={PREDICT_ERRORS}, "
              f"window_k={BASE_SIM.get('window_k', 100)})")
        warn_window_k(SWEEP_SCENARIOS)
        results = run_error_sweep(n_runs, n_workers)
        plot_error_sweep(results)
        print(f"Total elapsed: {format_duration(time.perf_counter() - t_start)}")
        sys.exit(0)

    print(f"Running {n_runs} simulation(s) per test ({n_workers} workers)")

    all_results = []

    for test in TESTS:
        if n_runs == 1:
            sim_seed = (derive_seed(MASTER_SEED, test["name"], 0)
                        if MASTER_SEED is not None else None)
            frames, display_config = run_test(test, seed=sim_seed)
            run_metrics = [extract_metrics(frames, display_config)]
        else:
            print(f"  [{test['name']}] dispatching {n_runs} runs ...", flush=True)
            args = [
                (test, i,
                 derive_seed(MASTER_SEED, test["name"], i) if MASTER_SEED is not None else None)
                for i in range(n_runs)
            ]
            run_metrics = [None] * n_runs
            with ProcessPoolExecutor(max_workers=n_workers) as executor:
                futures = {executor.submit(_run_single, a): i for i, a in enumerate(args)}
                done = 0
                for future in as_completed(futures):
                    run_metrics[futures[future]] = future.result()
                    done += 1
                    print(f"  [{test['name']}] {done}/{n_runs} done", end="\r", flush=True)
            print()
            # For the printed summary, regenerate with the *first* run's seed so
            # display_config matches what run 0 actually saw.
            first_seed = (derive_seed(MASTER_SEED, test["name"], 0)
                          if MASTER_SEED is not None else None)
            display_config = make_config(test, seed=first_seed)

        metrics        = average_metrics(run_metrics) if n_runs > 1 else run_metrics[0]
        scheduler_type = test["config"]["scheduler"]["type"]
        plot_test(run_metrics[0], test["name"], scheduler_type)
        print_summary_table(test["name"], metrics, display_config)
        all_results.append((test["name"], metrics))

    if len(all_results) > 1:
        plot_comparison(all_results)
        print_comparison_table(all_results)
        print_reliability_criteria_table(all_results)

    print(f"\nTotal elapsed: {format_duration(time.perf_counter() - t_start)}")

