# Changelog

## v1.1

### Schedulers

- Added **MPRM** ("Mean-Predictor RM") and **MPEDF** ("Mean-Predictor EDF") — MP for the mean predictor they run on: CHARM's accumulated-probability redundancy rule driven by a *static* estimate of the channel instead of a refreshed prediction. The estimate is the channel's long-run mean decode probability at the scheduler's transmit power, `rho_bar_j = sum_s pi_s rho_s(SNR(P_j))`, fixed before the run starts, so the redundancy allocated to each frame is deterministic. They sit between the fixed-power baselines and the CHARM family, and exist to measure what *refreshed* channel information adds beyond knowing the channel's average quality.
- MPRM and MPEDF never enter RX_MODE and take no listening knob: a static estimate learns nothing from listening. They therefore spend every slot transmitting or idle, which also makes them cheaper in airtime than CHARM/CHEDF at the same power.
- `MPEDF_scheduler` is a deliberate copy of `MPRM_scheduler` differing only in the queue comparator, mirroring the existing CHARM/CHEDF arrangement and carrying the same keep-in-sync requirement.

### Prediction

- **The channel-state predictor now returns a model state, not decode probabilities.** `MLPredictor::predict_channel_state()` answers an RX slot with one state label, `s_hat_c(t)`; the scheduler derives `rho_hat_{c,s,j}` from it off its own response curves. One observation therefore refreshes every power level together, from the same state and with the same age, and a mislabelled state moves power selection, demand and urgency as one correlated error instead of three independent ones.
- **CHARM, CHEDF and CATS hold a model of every channel, not of one.** `BaseScheduler` carries the channel set — one `{name, model, believed_state, last_observed}` entry per channel in `C`, in the simulator's channel order, so the index `c` addresses the same channel on both sides — and the three schedulers read `belief(c)` and `rho_hat(c, P)` out of it instead of carrying their own `belief`, `model`, `believed_state` and `transmission_prob`. Nothing is cached: `rho_hat` is read on demand, which is what lets a scheduler ask about a channel it is not transmitting on. The model in an entry is held by value and is **not** optional, so an entry cannot exist without one; `main.cpp` builds the set only when every channel has a model, since leaving one out would renumber the rest. `receive_state()` gained the channel it observed. The scheduler still transmits on one channel for a whole run — the set is populated and read, but `c_sel(t)` is not yet a per-slot decision.
- **Belief is a policy object the schedulers own, not part of the channel set.** `BaseScheduler`'s set is the *estimate* — fitted model plus latest label, the paper's §3.2.1 — and carries no belief, no threshold and no decay; when to refresh an estimate is a listening policy, and a scheduler could choose a fixed interval, an urgency test or the entropy of the propagated distribution instead. The shared rule lives in `BeliefTracker` (`schedulers/policies/belief_tracker.hpp`), which CHARM, CHEDF and CATS hold by **composition** — one definition rather than three copies, since the rule is identical in all three and CHARM/CHEDF are already verbatim copies by design. Each ticks `belief.age(get_channels())` at the top of its `do_schedule_frame()` and calls `belief.observed(c)` from `receive_state()` only when `observe_state()` reports it recorded a label. `observe_state()` now returns a bool for exactly that purpose.
- **Belief is carried per channel and decayed once per slot, for every channel.** One multiplication per channel per slot; nothing assigns it except `observed()` and `age()`. What changed from the single-channel version is *what ages* — with C channels every channel ages every slot, because the physical channel evolves whether the radio transmits on it, idles, or listens elsewhere, and CHARM's old `belief *= decay` ran only on the slots that scheduler spent transmitting on its own channel. Cost is C multiplications per slot, flat, and it needs no stored timestamp. Two behavioural consequences: an RX slot that returns no usable label buys no belief (the old code restored belief at schedule time whether the observation arrived or not), and **the refresh interval is one slot shorter** — the old code tested belief before decaying it, so a measured 19/37/19/23 slots on the four channels becomes 18/36/18/22, which is `ceil(ln(threshold)/ln(A_ss))` exactly. CHARM's RX share on `cyclic_fast_6` moves 5.27% → 5.57% with it.
- ⚠️ **The decode-probability path is removed outright: every channel is now an idealized FSMC.** `BELIEF_DECAY` (0.99015), `BaseScheduler::receive_prediction()`, `wants_state_prediction()`, `get_prediction_powers()` and `MLPredictor::predict_channel_conditions()` are all deleted, and `main.cpp`'s RX branch collapsed to the single state path. A scheduler reasons about a fitted FSMC or there is nothing for it to reason about, so `main.cpp` errors out at startup if any channel has no model, naming it — rather than degrading to an estimate nobody can age. **`ReplayChannel` is consequently unrunnable and `hf_experiment/run.py` with it**; the class and its CSV are still in the tree. `predict_error` now has no effect on any run and `main.cpp` warns whenever it is set. Verified no effect on FSMC runs: 10/10 bit-identical logs across 4 channels × {CHARM, CATS} saturating plus CATS/CHEDF at moderate load.
- The scheduler's copy of each model is what will let the estimated model be perturbed (`Delta_A`, `Delta_rho`) without touching the physical channel; as shipped the two are identical, the zero-mismatch reference case.
- **New `ChannelModel`** (`system_model/channel_model/`) is the fitted FSMC — states, row-normalised kernel, stationary distribution, sigmoid responses and the SNR calibration — and is now the single place a channel's two files are read. `SigmoidChannel` holds one and steps an `FsmcChain` through it, so the physical and estimated sides cannot implement the sigmoid or the 20 dB calibration differently. `FsmcChain` keeps only the stepping; `matrix_from_file()` moved into `ChannelModel`.
- `BasePhysicalChannel` gained `get_fsmc_state()` and `estimated_model()`, both defaulting to "no state / no model". `ReplayChannel` returns `nullptr`, which is now a startup error rather than a fallback (see above).
- ⚠️ **`predict_error` no longer affects an FSMC channel.** It perturbs a predicted probability, and the state path has none to perturb; state-prediction error is the confusion matrix `E_c` and is not implemented yet. `main.cpp` warns when a run sets `predict_error` and takes the state path, so `error_sweep` curves that coincide are not read as insensitivity to prediction error.
- Verified behaviour-preserving: with belief decay pinned to the old constant, 16/16 cases (4 channels x CHARM/CHEDF/CATS/MPEDF, 30k ticks, seeded) are bit-identical to the previous build across every radio mode, transmission, power and decode outcome. The belief rule below is the only behavioural change.

### Listening policy

- **CHARM and CHEDF now listen on belief rather than on a fixed interval.** A periodic `rx_period` listen is an unoptimised stand-in for "is my channel estimate still worth anything", so both adopt the rule CATS already used: belief decays by `BELIEF_DECAY` on every transmitting slot (every slot, since the rework below), a listen restores it, and the scheduler listens once it falls below `belief_threshold`. `rx_period` is removed from the config, the constructors and the roster; `belief_threshold` (default 0.7) replaces it.
- **Belief now decays by the believed state's own self-transition probability `A_ss`**, read from the scheduler's copy of the fitted FSMC, so belief `d` slots after the observation is `A_ss^d` — the model probability the channel has not left the state it was last observed in, with `1 - A_ss` the per-slot probability of leaving. The refresh interval is `ceil(ln(threshold) / ln(A_ss))`, which is per channel *and* per state: 18 slots on `cyclic_fast_6`, 36 on `cyclic_slow_8`, 18/12 on `good_moderate_2`'s two states, 178/22/30 on `rare_excellent_3`'s three. The previous global constant produced a flat 37 on all four.
- Measured under a saturating load (where every listen is belief-driven rather than an empty-buffer fallback), RX share moved 2.6% -> 5.3% on `cyclic_fast_6`, 2.6% -> 6.1% on `good_moderate_2`, 11.9% -> 12.1% on `cyclic_slow_8` and 7.8% -> 8.7% on `rare_excellent_3`. `cyclic_slow_8` barely moves because 0.99015 was that chain's own |lambda_2| to begin with.
- `BELIEF_DECAY` (0.99015) is **gone** — see the FSMC-only change above. A per-channel constant is no longer needed, and neither is an eigensolver: `A_ss` is a matrix entry.
- CHARM/CHEDF listen on belief alone, without CATS's `no_urgent_packet` guard, so a listen can consume a slot a near-deadline packet needed.
- The CLI's `belief_threshold` argument now applies to every belief-driven scheduler, not only CATS, and prints which ones it touched.
- **No fixed-interval listener remains in the roster**, so the figures no longer carry a periodic-vs-belief control and nothing is "CHARM as published" any more.
- Measured against the previous build on identical task sets: RX share fell from a forced 20% to ~13% at U=0.5 and ~3.8% at U=0.8, schedulability was equal or better everywhere sampled, energy rose ~15% as freed slots went to transmitting, and CHARM's worst burst at U=0.8 fell from 684 to 85.
- **Configs written for v1.0 need updating.** A `rx_period` field is now ignored rather than rejected, and CHARM and CHEDF read `belief_threshold` unconditionally, so a config carrying the old field and not the new one fails at construction.

### CATS policy ablation

- CATS's three policies are now individually switchable: `adaptive_power`, `early_drop` and `urgency_check`, all defaulting to enabled. A config that sets none of them is the published CATS and produces byte-identical output to the previous build.
- With `adaptive_power` off, the configured `tx_power` drives the transmitted power *and* the demand, infeasibility and urgency maths — no adaptive cap is retained anywhere. A power that is not a predictor tier (1, 10, 25 W) is rejected with a clear config error.
- With `early_drop` off, infeasible packets stay queued until deadline expiry or normal retirement. Classification is separated from removal, so they are not reclassified as urgent and do not suppress listening.
- With `urgency_check` off, listening depends on belief alone. Belief update, threshold, power selection and dropping are unchanged, and the empty-buffer listen still applies.
- New mode **`cats_comparison`** plots seven variants on the sweep's axes, writing `results_cats_comparison*`. Fixed-power variants run at both 10 W and 25 W.
- `run_sweep()` takes an optional roster and `plot_schedulability()` an optional basename/title, so the new mode reuses both without overwriting the sweep's figures.
- The CLI `belief_threshold` override now also reaches the comparison roster.

### Channel

- **`frequency` is gone from the entire simulator.** A channel is addressed by its **index** `c` — its position in the config's `channels` block, which is the paper's `C = {0, ..., C-1}` — and carries its directory name (`cyclic_fast_6`) for logs and errors only. The field was never a model input: nothing read it but the lookups that matched a scheduler to a channel, and a second identifier for the same thing is what has to be kept in step. `scheduled_frame_t`, `transmitted_frame_t` and `received_frame_t` carry `channel`; `BasePhysicalChannel` carries `name`; `MLPredictor` takes the index; every scheduler constructor takes the index instead of a frequency. In the config, `channels` entries lose `frequency` and a scheduler takes `"channel": c` (default 0); `main.cpp` rejects an index past the end of the block and an empty `channels` block, naming both. The per-slot log, the transmission block, the FSMC block (which gains the channel's `name`) and `received_packets.json` all key on `channel` now, so a `simulation_log.json` written before this change can no longer be parsed by `tests` mode.
- **Four designed channels now ship instead of one**, each a named propagation regime: `cyclic_fast_6` (the former `channel_20m`, renamed — the band stopped being part of the model when the noise floor went dead, and the cycle is what distinguishes it), `cyclic_slow_8` (the same mirrored ring over all four curve archetypes, at half the speed), `good_moderate_2` (never worse than Moderate, 60% of the time Good) and `rare_excellent_3` (95% Poor or Moderate, with an Excellent opening roughly every 10k slots that holds for ~500). `cyclic_fast_6` is numerically identical to the channel it replaces, so results carry over under the new name.
- **The FSMC state count is resolved at run time** rather than from the compile-time constant `SigmoidChannel::N_STATES`. The shipped channels have 6, 8, 2 and 3 states and one build loads all of them. `fsmc_states.json` defines the count and the matrix must be square of that size; the two files are cross-checked at construction, which is the check the shared constant used to provide. `kovian::MarkovChain` is templated on its dimension and so could not be used for this — `FsmcChain` (`physical_channels/sigmoid_channel/fsmc_chain.hpp`) replaces it, reading the same `.kov` format and keeping its row-width and row-sum validation.
- **The initial FSMC state is drawn from the stationary distribution** rather than uniformly, so a run starts where the channel spends its time and needs no warm-up. The two coincide only when pi is uniform, which held for the single shipped channel but not for `good_moderate_2` (60/40) or `rare_excellent_3`, where a uniform draw would open one run in three in a state the channel occupies 5% of the time. Seeded runs no longer reproduce byte-identically against the previous build.
- `build_channels.py` writes the four shipped channels from their design parameters (occupancy, dwell), asserting that each solved stationary distribution lands on its target; `generate_channels.py` remains for random sensitivity work and now draws a labelled state diagram per channel.
- **The 20 m state table now carries three distinct decode curves instead of two.** It was three Good states followed by three Poor ones, so the one-way ring stepped straight from the best channel to the worst. A Moderate rung was added and the six slots mirrored as `Good, Moderate, Poor, Poor, Moderate, Good`, so the walk descends and climbs back and the wrap no longer jumps between extremes. This changes every result against the previous release.
- **`noise_floor_dbm` removed** from `fsmc_states.json`, `markov_state_t`, the verbose stdout dump and the per-tick log. It was dead data: the noise term in the decode-probability calculation has always been commented out, so received power is used directly as the SNR and the noise floor is folded into each state's `snr_50_db`. Results are unchanged, verified byte-identical against the previous build on a seeded 3000-tick run. A `fsmc_states.json` that still carries the key loads fine, since the loader reads only the three keys it needs; a `simulation_log.json` written before this change can no longer be parsed by `tests` mode, because `FsmcStatus` no longer accepts the field.
- `BasePhysicalChannel` gained `mean_probability()`, the long-run average decode probability at a power. `SigmoidChannel` averages its per-state curves over the chain's stationary distribution, computed once at construction by power iteration on the transition matrix; `ReplayChannel` returns the empirical decode rate of the recorded window.
- `SigmoidChannel` state handling tidied: the state count was consolidated into the single constant `N_STATES` rather than three independent literals (and has since become a run-time value, above), the per-state decode curve is reachable without stepping the chain, and a `fsmc_states.json` shorter than the chain is rejected at construction instead of being read out of bounds.
- Behaviour of the existing schedulers is unchanged by the refactor; seeded runs reproduce byte-identically.

### Experiment harness

- `sweep` roster set to the 8 of the paper's Simulation 1: RM, EDF and MPEDF at 10 W, CHARM and CHEDF at 10 W and 25 W, and CATS. `error_sweep` grows from 5 to 7 with the two MPEDF entries. MPRM and MPEDF are predictor-independent, so the error sweep runs them once and replicates.
- MPRM is built and wired but used by no roster; it is kept for the period-ordered arm of the static comparison.
- Sweep figures fall back to the `tab20` palette past 10 curves, leaving shorter rosters on the colors they had.
- Added a second recorded channel window, `hf_experiment/radius_2500km/frame_success.csv`, so the replay experiment can be run at more than one link distance.

### Cluster support

- `run_slurm.sh` takes a `CONDA_ENV` override, falls back to the pre-rename `cats-scheduler` environment, and on finding neither prints the create command and the environments the job can actually see instead of dying on conda's own error.
- Resource-sizing estimates rescaled from the retired 5-entry roster to the current rosters, `cats_comparison` added to them, and they are now marked as extrapolations rather than measurements. The note on which schedulers are deduplicated across error levels was wrong: it is EDF and MPEDF, not RM.

### Documentation

- Added `doc/`, holding the new `doc/USAGE.md` and the system-model, channel-curve and scheduler-comparison figures, with `.gitignore` exceptions so the `*.png` / `*.pdf` rules no longer hide them.
- `SLURM.md` moved to `doc/SLURM.md` alongside the rest of the documentation.
- `CLAUDE.md` is no longer tracked; its operational content lives in `doc/USAGE.md`.

## v1.0

First complete release of the simulator and its experiment harness. `v0.5` ran a single EDF scheduler over a static channel from a hardcoded `main.cpp`; this release is a config-driven, reproducible, five-scheduler comparison framework. 161 commits.

### Project rename

- Renamed from cats-scheduler to **rt-link-sim**, after the simulator rather than one of the schedulers it compares.

### Schedulers

- Added **CATS**: belief about the channel state plus a utilization test, choosing both when to transmit and at what power.
- Added **CHARM**, a channel-aware baseline with accumulated-probability retransmission over a period-ordered queue.
- Added **CHEDF**, CHARM's policy over a deadline-ordered queue, isolating queue discipline from power policy.
- Renamed `smaller_period_first` to **Rate-Monotonic** to match the literature.
- Extended **EDF** with the base scheduler interface, radio-mode handling and power-tagged naming.
- All four baselines now run at 10 W and 25 W, the levels CATS predicts over, for comparable-energy comparisons.

### Experiment harness

- Added `run_simulation.py`: UUniFast task sets, parallel sweeps, schedulability and energy plots.
- Three modes: `sweep`, `error_sweep` and `tests`.
- Deadlines are drawn from a target utilization rather than the reverse.
- Every parameter affecting a run is snapshotted next to its plots.
- Added `hf_experiment/run.py`, the same machinery over recorded per-tick channel outcomes.

### Simulator

- `main.cpp` is now config driven: scheduler, channels and generators all come from `simulation_config.json`.
- Config and log paths are overridable from the command line.
- Added summary mode, which silences stdout and emits only an aggregate, making long sweeps affordable in memory.

### Channel and predictor

- Added finite-state Markov channel modelling through the vendored kovian library, with 20 m band state data.
- Added a replay channel that replays recorded success outcomes, for validation against measured data.
- Sigmoid channel gained saturation control, externalised FSMC state and more realistic values.
- The predictor now reports decode probabilities at several transmission powers.
- Added configurable prediction error, so predictor robustness can be swept.

### Metrics

- Added `sched_ratio_mk`, a weakly-hard (m,k)-firm constraint over sliding windows, which catches bursts that a whole-run average hides.
- Added `max_burst`, the longest run of consecutive undelivered instances.
- Added `total_energy`, summed transmission power over transmitted slots.
- Kept `sched_ratio` as the whole-run delivery rate against the requirement.

### Reproducibility

- Setting `SEED` derives every per-simulation seed deterministically and propagates it into the receiver, the channel and the Markov chain.
- The same seed now reproduces identical figures; leaving it unset keeps clock-based behaviour.

### Cluster support

- Added `run_slurm.sh` for SLURM systems, with resource sizing notes and mode validation.
- Added `SLURM.md` as a submission and monitoring reference.
- The script deliberately does not build, since concurrent jobs would race on the binary.

### Figures

- Added vector PDF output alongside PNG, with Type 42 fonts so IEEE and ACM submission checks pass.
- Labels now use the paper's nomenclature for reliability requirement, packet length and power.
- Moved legends outside the axes, where nine curves no longer cover the data.
- Removed percentile bands for the same reason; the spread is still recorded in the results.
- Changed gridlines to dotted light gray on both axes.

### Terminology

- **Success rate is now reliability requirement** across the C++ code, config format, harness and figures.
- Packet transmission length renamed from C to L.
- Configs written for `v0.5` need updating.

### Architecture and build

- `system_model/` now holds the buffer, radio interface, target receiver and predictor.
- The transmitter sits behind a `RadioInterface`; the receiver was renamed `TargetReceiver`.
- Physical channels moved out of `system_model/` into a top-level `physical_channels/` with a base class.
- Per-directory Makefiles throughout, plus `schedulers.hpp` as an umbrella include.
- Reduced the memory footprint of sweep workers and speeded up compilation.

### Fixes

- CHARM decided retransmissions from a hardcoded 10 W probability while transmitting at its configured power.
- A misspelled harness mode fell through to `tests`, the heaviest mode, and OOM-killed long cluster jobs; unknown modes are now rejected up front.
- Fixed-power schedulers embed their power in `get_name()`, so runs at different powers no longer overwrite each other's logs.
- Corrected the per-frame probability requirement in CATS and CHARM.
- Restored cppcheck compliance and made static analysis pass on a clean tree.

### Documentation

- Rewrote the README around the current architecture.
- Added `CLAUDE.md` with design rationale and known traps.
- Added `SLURM.md` as a cluster reference.

## v0.5

Prototype: EDF scheduler, sigmoid channel, JSON logging, hardcoded simulation in `main.cpp`.
