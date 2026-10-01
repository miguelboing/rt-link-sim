# rt-link-sim

Real-time scheduling over an unreliable link. C++ simulator for wireless packet scheduling under deadline + channel-quality constraints. Compares multiple schedulers (CATS, CHARM, CHEDF, EDF, Rate-Monotonic) against synthetic task sets, driven by a Python harness.

## Entry points

- `main.cpp` — single-run simulator. Reads `simulation_config.json` (or `argv[1]`), writes `simulation_log.json` (or `argv[2]`).
- `run_simulation.py` — Python harness that generates task sets via UUniFast, runs the C++ binary repeatedly across (scenario × utilization × scheduler × seed) points in parallel, and produces plots.
  - Usage: `python run_simulation.py <n_runs> <mode: tests|sweep> <run_name> [belief_threshold] [utilization_threshold]`
- `run_slurm.sh` — submission script for SLURM-based clusters (`#SBATCH` directives, modules and conda env are set up for AIRE at Leeds and are the site-specific part). Build must happen on a login node first; concurrent jobs would race on `main.o`.
- `analyze_low_u.py` — ad-hoc analysis script for low-utilization corner cases.
- `hf_experiment/run.py` — replay sweep. Reuses `run_simulation.run_sweep`/`plot_schedulability` but monkey-patches the module globals (`SCHEDULERS`, `ERROR_SWEEP_SCHEDULERS`, `BASE_CHANNELS`, `SWEEP_SCENARIOS`, `U_VALUES`, `TESTS_DIR`) to run over `ReplayChannel`, which replays per-tick outcomes from `frame_success.csv` and shows the predictor a fixed 0.07/0.58/0.80 view of 1/10/25 W. Keep both its rosters and labels in sync with `run_simulation.py` — it builds them from the `BASE_*_SCHEDULER` dicts with only `frequency` rebound, so they can't silently diverge.
  - Usage: `SEED=42 python hf_experiment/run.py <n_runs> [sweep|error_sweep]` (defaults: 5 runs, `sweep`). `error_sweep` overrides `predict_error` per level from `rs.PREDICT_ERRORS`, so `BASE_SIM["predict_error"]` only applies in `sweep` mode.

## Build

```bash
make            # produces ./main.o
make clean
```

The Makefile recurses into `system_model/`, `packet_generators/`, `schedulers/`, `physical_channels/`, each compiling `*.o` into the repo root, then links everything into `main.o`. After C++ edits, rebuild before running the Python harness.

Compiler: `g++` with `-Wall`, C++17 (uses `std::optional`). Headers from `libs/` (nlohmann/json, kovian).

## Layout

```
main.cpp                         # config-driven simulator entry
schedulers.hpp                   # umbrella include for all schedulers
schedulers/
  base_scheduler.hpp             # interface: schedule_frame()
  cats/                          # CATS — channel/belief-aware, this project's contribution
  charm/                         # CHARM baseline
  chedf/                         # CHARM's policy over an EDF queue (standalone copy)
  srm/                           # SRM — CHARM's redundancy rule at a static channel average
  sedf/                          # SEDF — the same over an EDF queue (standalone copy)
  earliest_deadline_first/       # EDF baseline
  rate_monotonic/                # RM baseline
system_model/
  system_model.hpp               # shared types
  buffer_packet/                 # packet buffer + deadline enforcement
  radio_interface/               # transmitter side
  target_receiver/               # receiver side
  ml_predictor/                  # channel-state predictor used by CATS
packet_generators/
  base_packet_generator.hpp
  fixed_rate/                    # only generator currently wired in
physical_channels/
  base_physical_channel.hpp
  sigmoid_channel/               # SNR→success-prob sigmoid
  replay_channel/                # replays recorded per-tick outcomes
  channel_20m/                   # FSMC states + transition matrix, 20m band
libs/                            # vendored deps (nlohmann, kovian)
```

Each component subdir has its own Makefile invoked from the top-level one. To add a scheduler/generator/channel: subclass the matching `base_*.hpp`, drop a Makefile in a new subdir, and wire it into the parent Makefile (and `schedulers.hpp` for schedulers, plus the `sched_type` chain in `main.cpp`).

## Channel model (FSMC)

`channel_20m/` holds two files read at construction: `transition_matrix.kov` (the chain) and `fsmc_states.json` (one sigmoid per state). The chain is a **6-state one-way ring**, `0→1→2→3→4→5→0`, with a 0.98 self-loop, so mean dwell is 50 ticks and a full lap is about 300.

⚠️ **The 6 slots carry only 3 distinct curves, mirrored as a triangle. The duplicates are load-bearing, do not deduplicate them.**

| Slot | Curve | slope | snr50 | max_saturation | 1 W / 10 W / 25 W |
|---|---|---|---|---|---|
| 0, 5 | Good | 0.4 | 10 | 0.95 | 0.475 / 0.933 / 0.946 |
| 1, 4 | Moderate | 0.3 | 15 | 0.85 | 0.155 / 0.695 / 0.796 |
| 2, 3 | Poor | 0.2 | 20 | 0.70 | 0.083 / 0.350 / 0.482 |

Laying the three qualities out as `G, M, P, P, M, G` is what makes the ring descend and then climb back: the walk goes Good, Moderate, Poor, Poor, Moderate, Good and wraps into Good again. Collapsing the mirror to three states would put a jump from worst straight to best at the wrap, which is the one transition a real HF channel does not make. It also balances the cycle, with equal expected time in Good and in Poor.

The **number of states** is `SigmoidChannel::N_STATES`, and the chain, both initial-state draws and the stationary vector are all expressed in terms of it, so changing it is a one-line edit. The two *data files* are still not coupled to it: `transition_matrix.kov` is checked by kovian, which throws on a row-count mismatch, and `fsmc_states.json` is checked in the constructor, which throws if the table is not exactly `N_STATES` long. A short state table used to be a silent out-of-bounds read.

`gen_probability()` evaluates the current state; `state_probability(s, P)` evaluates a named one; `mean_probability(P)` averages the latter over `stationary`, the chain's long-run state distribution. `stationary` is computed once at construction by power iteration on the transition matrix rather than hardcoded — the 20m ring is a circulant and so doubly stochastic, making its π exactly uniform (1/6 each, hence 1/3 per distinct curve), but a future matrix need not be. The resulting averages are **0.238 / 0.659 / 0.742** at 1 / 10 / 25 W; they are what the SRM/SEDF schedulers run on.

`noise_floor_dbm` is currently **dead data**. `gen_probability()` has the noise term commented out and uses received power directly as the SNR, so only `slope`, `snr_50_db` and `max_saturation` affect results. The operating points are fixed by the 20 dB pathloss: 1 W lands at 10 dB, 10 W at 20 dB, 25 W at 24 dB.

## Config format

`simulation_config.json` has four sections: `simulation` (duration in frames; optional `seed`), `scheduler` (type-specific params), `channels`, `packet_generators`. The Python harness rewrites this per run via `make_config()` in `run_simulation.py`. CATS-specific knobs: `belief_threshold`, `utilization_threshold`. CHARM and CHEDF both use `tx_power`, `rx_period`. SRM and SEDF take `tx_power` and `frequency` only — they never listen, so there is no `rx_period`, and their decode estimate comes from the channel rather than from the config.

Packet fields: `id`, `period`, `relative_deadline`, `frames` (transmission length), `reliability` (required reception probability), `phase`.

A roster is a list of `(label, config)` pairs, where the label names the curve in plots and the config is what gets written to `simulation_config.json`. The same scheduler type can appear more than once at different parameters; the fixed-power baselines are run at both 10 W and 25 W (the two upper power levels CATS predicts over) so CATS can be compared against a baseline burning comparable energy. Labels must be unique — they key the results dicts.

**There are two rosters, and they are deliberately different.**

- `SCHEDULERS` (13) drives `sweep` and `tests`: `RM_10W`, `RM_25W`, `EDF_10W`, `EDF_25W`, `SRM_10W`, `SRM_25W`, `SEDF_10W`, `SEDF_25W`, `CHARM_10W`, `CHARM_25W`, `CHEDF_10W`, `CHEDF_25W`, `CATS`. `sweep` runs at `predict_error = 0` (`BASE_SIM`), so no gap on that figure can be blamed on prediction noise. The roster is the full 2×3 of the two effects under study — channel knowledge (none for RM/EDF, the long-run average for SRM/SEDF, a refreshed prediction for CHARM/CHEDF) crossed with queue discipline (period-ordered RM/SRM/CHARM vs deadline-ordered EDF/SEDF/CHEDF) — so each gap has its own control. Entries are ordered by family with the two powers adjacent, because `_draw_scenario_panel` takes color from enumeration order.
- `ERROR_SWEEP_SCHEDULERS` (7) drives `error_sweep`: `EDF_10W`, `EDF_25W`, `SEDF_10W`, `SEDF_25W`, `CHEDF_10W`, `CHEDF_25W`, `CATS`. That figure draws one curve per predictor-sensitive scheduler *per error level*, so the 13-roster would put ~29 curves on each axis. It uses the deadline-ordered variants throughout (EDF, SEDF, CHEDF) so the surviving gaps are the power policy, not the queue discipline. EDF and SEDF are predictor-independent and contribute one flat curve each.

Anything reading a roster must pick the right one — `run_error_sweep`, `_draw_error_sweep_panel`, and `write_experiment_params` all switch on the mode. `_draw_scenario_panel` takes color/linestyle/marker straight from enumeration order of the data it is handed, not from `SCHEDULERS`, so it renders whatever roster it is given; its `linestyles`/`markers` lists must stay at least as long as the longest roster or two curves share a style.

**The palette switches on roster size.** `tab10` holds 10 colors, so a roster longer than that would wrap and give two curves the same color; `_draw_scenario_panel` falls back to `tab20` only past 10 entries, which leaves every figure of 10 curves or fewer on exactly the colors it had before. `tab20`'s entries are dark/light pairs of one hue, so with the roster ordered by family a scheduler's 10 W and 25 W curves come out as two shades of the same color — that pairing is the reason the roster is grouped by family rather than by power.

**Legends live outside the axes, in the header.** With 13 curves on each figure and labels that now read `CHARM (P_CH = 25 W, ε = 0.15)`, an in-axis box covers the curves it labels. `_title_and_legend()` owns the frame of every sweep figure: suptitle on top, axes, then one figure-level legend along the bottom. One legend per figure, not per axis — every row (and on the combined figures every column) plots the same roster. Both strips are *measured* (legend row count × `legend.fontsize`, and the suptitle line) and passed to `tight_layout(rect=…)`, so the axes fit between them whatever the legend's height; the panel drawers just plot and never call `legend()`.

The error sweep passes `group_key=_error_sweep_group` to get **one column per scheduler**: matplotlib fills a multi-column legend column-major, which stacks a scheduler's three ε curves only if every column holds the same number of entries, so `_grouped_legend_columns()` pads each group to the tallest with blank entries and lets adjacent one-entry groups (the fixed-power baselines, which have no ε levels) share a column. Without it the two EDFs shift everything and a CHEDF curve lands under them. `_legend_ncol()` picks the column count from the *rendered* label width — `$P_{\mathrm{CH}}$` is 17 characters of markup that draws as about three glyphs, so the raw length would misjudge it.

Neither figure draws the 10th/90th-percentile bands any more — with this many curves per axis the bands overlapped into an unreadable smear. `aggregate_runs` still records `<metric>_lo` / `<metric>_hi` in the results dict, so the spread is available offline.

**CHEDF is its own scheduler type and its own standalone class** (`schedulers/chedf/`, subclassing `BaseScheduler` directly), taking the same config parameters as CHARM: `tx_power`, `frequency`, `rx_period`.

⚠️ **`CHEDF_scheduler` is a deliberate copy of `CHARM_scheduler`, differing only in the `min_element` comparator.** Everything else — the accumulated-probability retransmission rule, the `rx_period` listening schedule, `receive_prediction()`, `get_prediction_powers()` — is duplicated verbatim, and the CHARM/CHEDF comparison is only interpretable while it stays that way. **Any change to that shared logic must be applied to both files.** Nothing in the build enforces this; the header on each class says so, and it is the one maintenance hazard of the split. (An earlier version had CHEDF subclass CHARM behind a virtual `select_packet()` hook, which made drift impossible; standalone classes were preferred for clarity.)

CHARM's comparator keeps its `is_periodic` guard (an aperiodic packet has no period to rank by, so CHARM declines to send when only such packets are queued); CHEDF's needs none, since a deadline is well defined for every packet.

**SRM and SEDF are the static-channel pair, and they repeat the CHARM/CHEDF arrangement exactly.** `SRM_scheduler` is CHARM's accumulated-probability redundancy rule over a period-ordered queue, and `SEDF_scheduler` is a deliberate copy of it differing only in the `min_element` comparator — the same hazard, unenforced by the build, so **any change to the shared rule must be applied to both files**. SRM keeps the `is_periodic` guard, SEDF needs none, for the same reasons as above.

What makes them *static*: instead of a prediction refreshed every `rx_period` ticks, they take one number fixed before the run — the channel's long-run mean decode probability at their transmit power, `rho_bar_j = sum_s pi_s rho_s(SNR(P_j))`, supplied by `BasePhysicalChannel::mean_probability()` and queried once by `main.cpp` at construction. Because the value never moves, the redundancy is deterministic: every instance of a task is sent `ceil(log(1 - theta^(1/L)) / log(1 - rho_bar))` times whatever the channel is doing, where CHARM's count varies with the state. They are the baseline for *what refreshed channel information adds beyond knowing the channel's average quality*.

Three things that follow, and bite if forgotten:

- **They never enter RX_MODE.** No `rx_period`, and `get_prediction_powers()` is left at the base class's empty default, which is what stops `main.cpp` routing a prediction to them. So they get every slot as a transmit opportunity where CHARM gives up 1 in `rx_period` — an SRM/CHARM gap mixes the information effect with that airtime difference, and the energy row is where it shows. This was a deliberate choice: a static estimate gains nothing by listening.
- **They are predictor-independent**, so they belong in `PREDICTOR_INDEPENDENT_TYPES` (they are) and the error sweep dispatches them once at `PREDICT_ERRORS[0]` and replicates. A `predict_error` sweep cannot move them.
- **The estimate is the channel's, not the config's.** `mean_probability()` is pure virtual: `SigmoidChannel` averages its per-state curves over the chain's stationary distribution, `ReplayChannel` returns the empirical decode rate of the recorded window, and a new channel must say what its own long-run average means. On the replay channel that empirical rate is close to the fixed `PROB_*` view the predictor is shown, so there SRM/SEDF and CHARM/CHEDF run on nearly the same number and the pair mostly isolates airtime — the informative comparison for these two is the FSMC sweep.

Under overload the ordering difference is large and mostly shows up in `max_burst`: fixed-priority order starves long-period tasks indefinitely (RM at U=1.0 reached a ~1000-instance burst, and period-ordered CHARM ~3900, versus ~46 and ~93 for the deadline-ordered equivalents on the same task sets).

## Schedulability metrics

Four metrics are computed in `main.cpp`'s summary block from one pass over the per-instance delivered/not sequence, then aggregated by `extract_sweep_metrics()`. `SWEEP_METRICS` is what gets aggregated; `PANEL_METRICS` is the subset that gets *plotted*, and it drives both the sweep and error-sweep figures so their row layout can't drift apart. Rows, in order: **schedulability ratio, windowed schedulability, energy**. `hf_experiment/run.py` rebinds `PANEL_METRICS` to drop the windowed row (see below); the figures size themselves from `len(PANEL_METRICS)`, and `_window_k_suffix()` drops the `(m,k window k=…)` note from the title when that row is absent.

- `sched_ratio` — fraction of ids whose **whole-run** delivery rate meets `reliability`. Insensitive to *when* misses land: an early burst gets averaged away by a long clean tail.
- `sched_ratio_mk` — fraction of ids meeting the weakly-hard **(m,k)-firm** constraint for the *whole run*: at least `m = ceil(reliability * k)` of every `k` consecutive instances, **sliding** (not tumbling, so a burst straddling a boundary can't be masked in both halves). One violated window anywhere fails the id. Burst-sensitive; converges to `sched_ratio` as k grows.
- `max_burst` — longest run of consecutive undelivered instances. Parameter-free, so it can't be tuned; often the quantity a control loop actually cares about. **Aggregated but no longer plotted**: it spans three orders of magnitude across U (single digits at U=0.2, thousands under overload), which flattens the low-U end to an unreadable line on a shared linear axis. Read it from the raw results, or add it back to `PANEL_METRICS` with a log scale.
- `total_energy` — unchanged, sum of tx_power over all transmitted time-slots (W·time-slot).

`window_k` lives in `BASE_SIM` (**default 100**) and is overridable per run with the `WINDOW_K` env var.

**Why 100.** `make_test_set` draws `reliability` as a 2-decimal float, so at k=100 the threshold `m = ceil(req*100)` is exactly `req*100` — `m/k` reproduces the requirement with no rounding, and each task is judged against the rate it actually asked for. Deriving a *per-task* k from the fraction instead (`0.50 → 1/2`, `0.51 → 51/100`) was considered and rejected: the denominator is an artifact of the decimal representation, so a 0.01 change in the requirement would swing burst tolerance by ~50× (1 tolerated miss in 2 vs. 49 in 100).

**`m` must be computed with an epsilon.** `window_m()` in `run_simulation.py` and the matching line in `main.cpp` both use `ceil(req*k - 1e-9)`. A bare `ceil` overshoots by one wherever the 2-decimal float rounds up — `ceil(0.55*100) == 56` and `ceil(0.56*100) == 57`, both inside the sweep's RR band — silently holding those tasks to a stricter rate than requested. The epsilon is far below the smallest legitimate gap (1/100 at k=1). Keep the two implementations in sync.

Two constraints bound `k`, and `warn_window_k()` / `warn_vacuous()` report violations at runtime:

- **`k >= 1/(1 - reliability)`** or `m == k` and the window silently degenerates to a zero-miss requirement — much stricter than the long-run rate it was derived from. At `rr_max = 0.9` the floor is k=10, so k=100 clears it comfortably; this only bites on a `WINDOW_K` override.
- **`k <= instances per id`** or the id has no complete window. Such ids are scored **vacuously met** (never as a violation), which is the *only* way `sched_ratio_mk` can exceed `sched_ratio`. This is the binding constraint at k=100: at the default 250k duration it reaches **~22% of ids** in the worst cell (n=20, U=0.1) versus ~3% at k=20, and `warn_vacuous()` prints the grid-wide worst case every run. At short durations it dominates entirely: the replay experiment's 160 ticks give every id fewer than k instances, so the metric there is vacuously 1.0 everywhere and carries no information. **`hf_experiment/run.py` therefore drops the windowed row from its `PANEL_METRICS` and raises `VACUOUS_WARN_THRESHOLD` out of reach** (the warning is about a windowed curve it no longer draws). `sched_ratio_mk` is still aggregated into its results dict, just not plotted; if `BASE_SIM["duration"]` there ever grows past ~100 instances per id, restore both.

**`sched_ratio_mk` is not run-length invariant.** An id passes only if *every* window holds, so extending a run can only add chances to fail — per-id, the verdict is monotone non-increasing in `duration`. Comparisons between schedulers are therefore only valid at equal `duration`, and the absolute level of the curve is not portable across durations. `window_violation_rate` (fraction of all windows violated, already in `SWEEP_METRICS` though not plotted) is the run-length-robust companion if that matters.

`window_k` remains a free parameter — sweep it before drawing a conclusion from a gap between two schedulers.

## Reproducibility

Set `SEED=<int>` env var before running the harness: `SEED=42 python run_simulation.py ...`. A per-sim seed is then derived deterministically (blake2b of master seed + test name + run index), seeded into Python's `random` for UUniFast and propagated via `config["simulation"]["seed"]` to the C++ side, where `TargetReceiver`, `SigmoidChannel`, and the kovian `MarkovChain` each get distinct sub-seeds. Same `SEED` → bit-identical output PNGs; unset → clock-based (non-deterministic).

## Outputs

- `simulation_log.json` — per-tick log (large, ~8 MB for 5000-frame runs).
- `<scheduler>_scheduled_packets.json`, `generated_packets.json`, `received_packets.json` — per-component logs from `main.cpp`.
- `results_*.png` / `results_*.pdf` — plots produced by `run_simulation.py`. Every figure is written once per entry in `PLOT_FORMATS` (env var, default `png,pdf`); the PDF is the vector copy for the paper, the PNG a 150-dpi raster for quick viewing. `save_figure()` owns the extension, so call sites pass a base path.
- `*.json`, `*.png`, `*.pdf` and `*.svg` are gitignored; don't commit them.

## Figure labels

Plots use the paper's symbols, spelled once as module constants at the top of `run_simulation.py` — `SYM_RR` (`$\theta_n$`, reliability requirement), `SYM_N` (`$N$`), `SYM_L` (`$L_n$`, packet length), `SYM_U` (`$U$`), `SYM_M` (`$m$`), `SYM_KWIN` (`$K_{\mathrm{win}}$`), `SYM_ERR` (`$\varepsilon$`, predictor error), and two power symbols — `SYM_PFP` (`$P_{\mathrm{FP}}$`) for the fixed-power baselines RM/EDF, `SYM_PCH` (`$P_{\mathrm{CH}}$`) for the CHARM-family CHARM/CHEDF. Use the constant, never an inline `$…$`.

**Roster labels are raw; legends are not.** `scheduler_title()` renders `CHEDF_10W` as `CHEDF (P_CH = 10 W)`, moving the parameters into a parenthesis, and takes extra parts that join the *same* parenthesis — the error sweep passes the ε term, giving `CHEDF (P_CH = 10 W, ε = 0.15)` rather than two bracketed groups. Which power symbol a label gets comes from `_POWER_SYMBOLS`, keyed on the label's base name and defaulting to `P_FP`; a new *family* needs an entry there, a new fixed-power baseline does not. A label with no `_<n>W` suffix (CATS, which picks its own power) passes through as just its name. The raw label still keys the results dicts and names `<scheduler>_scheduled_packets.json`, so never prettify at the source.

`plot_comparison` drops the scenario from its legends when every compared test shares one, moving it to the suptitle; with mixed scenarios each legend keeps its full name, since then the scenario is what's being compared.

The grid is **dotted light gray on both axes** — style in the `grid.*` rcParams, so there is one place to change it. Solid mid-gray competed with the curves; dotted reads as a guide. The two bar charts (`plot_test`'s instance counts, `plot_comparison`'s final reliability) pass `axis="y"`, since verticals would only cut through the bars. `axes.axisbelow` keeps the grid behind the data.

Text is set in **Latin Modern Roman** (the LaTeX body face; `texlive-lm` on Fedora, `lmodern` on Debian) and math in matplotlib's bundled **Computer Modern** mathtext — LM is a redraw of CM, so they match, and matplotlib can't load the OTF Latin Modern Math. `font.serif` falls through to STIX then DejaVu where LM isn't installed, so figures still draw on AIRE. A machine that has LM but was first run without it keeps a stale font cache — `rm ~/.cache/matplotlib/fontlist-*.json` once.

Rendering is mathtext, not a TeX install. `MPL_USETEX=1` switches the same strings to real LaTeX; that path additionally needs `type1ec.sty` (texlive-cm-super), which is *not* installed on the dev box — without it every `savefig` dies inside latex. `tex_safe()` escapes `_` etc. in scheduler labels and test names for that path and is a no-op otherwise; only ever pass it a plain string, never one that already contains a `SYM_*`.

PDFs embed Type 42 (TrueType) fonts, not matplotlib's default Type 3 — IEEE/ACM submission checks reject Type 3. Poppler prints a cosmetic "Mismatch between font type and embedded font file" for the Latin Modern OTF (CFF data in a CID-TrueType wrapper); it renders correctly everywhere tried. `rasterize_dense_artists()` rasterizes *data* artists (never text) on figures with more than `RASTERIZE_ABOVE` (20k) points, which is only the per-tick diagnostic figures at long durations — a fully vector PDF of a 250k-frame run is tens of MB and pans badly. The sweep figures are far below the threshold and stay fully vector.

**`scenario_label()` stays ASCII** — it keys the results dicts, names the test directories and names the PNGs. `scenario_title()` is the display-only form; it splits off the roster label a test name carries after an underscore (`…RR=[0.5,0.9]_CHEDF_25W`) to run through `scheduler_title()` — nothing else in a scenario label contains an underscore, which is what makes that split safe — and renders each remaining field via `_render_scen_field()`. Titles must go through it; filenames must not.

`=` is used only for the values that are exact (`$N = 10$`; `$U = 0.10$`, converted back from the integer percentage the filename-safe label stores). The other two fields are the interval a task's parameter is *drawn from*, so `L_n = [1,3]` would equate a scalar with a set — they get `∈`, with the bracket matching the domain: braces for the integer frame count (`random.randint`, enumerated as `{1,2,3}` up to four values and elided as `{1,…,9}` beyond), square brackets for the continuous reliability (`random.uniform`). A degenerate range (`L=[2,2]`) collapses back to `=`.

## Conventions

- Code is **cppcheck-compliant** (warning + style + performance + portability). CI runs static analysis from `.github/workflows/static-analysis.yml`. Don't introduce warnings.
- Shared mutable state is passed via `std::shared_ptr` (see `system_tick`, `buffer_packet`, `spawn_log` in `main.cpp`).
- Branch convention: `dev` for active work, PRs land on `main`.

## Things that have bitten in the past

- `run_slurm.sh` does **not** build — concurrent SLURM jobs race on `main.o` (the Makefile `rm`s then recreates it, leaving a window where the binary is missing). Always `make` on a login node before submitting.
- UUniFast in `run_simulation.py` generates **deadlines from utilization** (D = floor(C/u)), not the other way around. Don't "fix" it back to fixed-D + computed-C — that drifts utilization upward.
- A scheduler's `get_prediction_powers()` must match the power it actually transmits at. CHARM used to hardcode `{10}` while transmitting at its configured `tx_power`, which silently made a 25 W CHARM decide retransmissions from the 10 W decode probability. It now returns `{tx_power}`. Any new fixed-power scheduler needs the same coupling.
- `get_name()` on CHARM, CHEDF, RM and EDF embeds the power (`CHARM_25W`) because those types run at two power levels and would otherwise overwrite each other's `*_scheduled_packets.json`. EDF returned a bare `"EDF"` until it was promoted to the roster at both powers. CHEDF is a separate class, but since it was copied from CHARM its `get_name()` is the easiest line to forget to change — the pair then collides on the same log.
- **Module globals do not reach the sweep workers.** `run_sweep` uses a multiprocessing *spawn* pool, so children re-import `run_simulation` from disk and see the file's values, not the parent's. Patching `rs.BINARY` (or any other global `_run_single_sweep` reads) from a driver script silently has no effect — both halves of an A/B run then execute the same binary and produce a fake null result. Swap the binary on disk instead. This is why `hf_experiment/run.py` only patches globals that the *parent* consults when building the `test` dicts.
- **A misspelled mode used to run `tests`.** `MODE=error` instead of `error_sweep` fell through the mode chain to the default branch, and `tests` is by far the heaviest mode — it parses the full ~180 MB log per run in Python, so a 200-run job OOM-killed the SLURM step hours in (`--mem-per-cpu=1G` × 32 workers). Both `run_simulation.py` (against `MODES`) and `run_slurm.sh` now reject an unknown mode before any work starts. The tell in a `.out` file is the header line: `mode=…`, plus per-test dispatch lines and `results_<test>.png` output, where a sweep prints `[sweep] dispatching N sims`.
- Anything special-casing a scheduler by *label* (e.g. the error sweep skipping predictor-independent schedulers at non-zero error) breaks the moment a variant is added. Key on `config["type"]` via `is_predictor_independent()` instead.
