# Changelog

## Unreleased

### Listening policy

- **CHARM and CHEDF now listen on belief rather than on a fixed interval.** A periodic `rx_period` listen is an unoptimised stand-in for "is my channel estimate still worth anything", so both adopt the rule CATS already used: confidence decays by `BELIEF_DECAY` on every transmitting slot, a listen restores it, and the scheduler listens once it falls below `belief_threshold`. `rx_period` is removed from the config, the constructors and the roster; `belief_threshold` (default 0.7) replaces it.
- `BELIEF_DECAY` moved out of CATS into `schedulers/base_scheduler.hpp` so the three belief-driven schedulers share one definition. It is the modulus of the FSMC's second-largest eigenvalue, a property of the channel: recompute it if the transition matrix changes.
- CHARM/CHEDF listen on belief alone, without CATS's `no_urgent_packet` guard, so a listen can consume a slot a near-deadline packet needed.
- The CLI's `belief_threshold` argument now applies to every belief-driven scheduler, not only CATS, and prints which ones it touched.
- **No fixed-interval listener remains in the roster**, so the figures no longer carry a periodic-vs-belief control and nothing is "CHARM as published" any more.
- Measured against the previous build on identical task sets: RX share fell from a forced 20% to ~13% at U=0.5 and ~3.8% at U=0.8, schedulability was equal or better everywhere sampled, energy rose ~15% as freed slots went to transmitting, and CHARM's worst burst at U=0.8 fell from 684 to 85.

### Schedulers

- Added **MPRM** ("Mean-Predictor RM") and **MPEDF** ("Mean-Predictor EDF") — MP for the mean predictor they run on: CHARM's accumulated-probability redundancy rule driven by a *static* estimate of the channel instead of a refreshed prediction. The estimate is the channel's long-run mean decode probability at the scheduler's transmit power, `rho_bar_j = sum_s pi_s rho_s(SNR(P_j))`, fixed before the run starts, so the redundancy allocated to each frame is deterministic. They sit between the fixed-power baselines and the CHARM family, and exist to measure what *refreshed* channel information adds beyond knowing the channel's average quality.
- MPRM and MPEDF never enter RX_MODE and take no listening knob: a static estimate learns nothing from listening. They therefore spend every slot transmitting or idle, which also makes them cheaper in airtime than CHARM/CHEDF at the same power.
- `MPEDF_scheduler` is a deliberate copy of `MPRM_scheduler` differing only in the queue comparator, mirroring the existing CHARM/CHEDF arrangement and carrying the same keep-in-sync requirement.

### Channel

- `BasePhysicalChannel` gained `mean_probability()`, the long-run average decode probability at a power. `SigmoidChannel` averages its per-state curves over the chain's stationary distribution, computed once at construction by power iteration on the transition matrix; `ReplayChannel` returns the empirical decode rate of the recorded window.
- `SigmoidChannel` state handling tidied: the state count is now the single constant `N_STATES` rather than three independent literals, the per-state decode curve is reachable without stepping the chain, and a `fsmc_states.json` shorter than the chain is rejected at construction instead of being read out of bounds.
- Behaviour of the existing schedulers is unchanged; seeded runs reproduce byte-identically.

### Experiment harness

- `sweep` roster set to the 8 of the paper's Simulation 1: RM, EDF and MPEDF at 10 W, CHARM and CHEDF at 10 W and 25 W, and CATS. `error_sweep` grows from 5 to 7 with the two MPEDF entries. MPRM and MPEDF are predictor-independent, so the error sweep runs them once and replicates.
- MPRM is built and wired but used by no roster; it is kept for the period-ordered arm of the static comparison.
- Sweep figures fall back to the `tab20` palette past 10 curves, leaving shorter rosters on the colors they had.

## v1.0

First complete release of the simulator and its experiment harness. `v0.5` ran a single EDF scheduler over a static channel from a hardcoded `main.cpp`; this release is a config-driven, reproducible, five-scheduler comparison framework. 161 commits.

### Project rename

- Renamed from cats-scheduler to **rt-link-sim**, after the simulator rather than one of the schedulers it compares.

### Schedulers

- Added **CATS**, the project's contribution: belief about the channel state plus a utilization test, choosing both when to transmit and at what power.
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
