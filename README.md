# rt-link-sim

Real-time scheduling over an unreliable link. A C++ simulator for wireless packet scheduling under deadline and channel-quality constraints, driven by a Python experiment harness.

The project's contribution is **CATS** (Channel-Adaptive Transmission Scheduler), which combines a belief about the current channel state with a utilization test to decide *when* to transmit and at *what power*. The repository also implements six baselines so CATS can be measured against them on identical task sets: CHARM, CHEDF, SRM, SEDF, EDF and Rate-Monotonic. They form a ladder of channel knowledge — none (EDF, Rate-Monotonic), the channel's long-run average (SRM, SEDF), a prediction refreshed during the run (CHARM, CHEDF) — crossed with the two queue disciplines.

## Overview

A simulation advances one frame (time slot) per tick. On every tick:

1. Packet generators release packets into the buffer according to their periods.
2. The ML predictor reports, per channel, the decode probability at each power level the scheduler asked to be predicted.
3. The scheduler picks one buffered packet and a transmission power, or stays idle, or listens.
4. The radio interface builds the frame and the physical channel draws the outcome.
5. The receiver decodes or drops the frame.
6. The buffer discards packets whose deadline has passed.

A packet needs `frames` successful slots before its deadline to count as delivered.

## Schedulers

| Type in config | Class | Parameters | Notes |
|---|---|---|---|
| `CATS` | `CATS_scheduler` | `frequency`, `belief_threshold`, `utilization_threshold` | Channel and belief aware, picks its own power |
| `CHARM` | `CHARM_scheduler` | `tx_power`, `frequency`, `rx_period` | Baseline, accumulated-probability retransmission over a period-ordered queue |
| `CHEDF` | `CHEDF_scheduler` | `tx_power`, `frequency`, `rx_period` | CHARM's policy over a deadline-ordered queue |
| `SRM` | `SRM_scheduler` | `tx_power`, `frequency` | CHARM's redundancy rule driven by a static channel average, period-ordered queue |
| `SEDF` | `SEDF_scheduler` | `tx_power`, `frequency` | The same over a deadline-ordered queue |
| `EDF` | `EDF_scheduler` | `tx_power`, `frequency` | Fixed-power baseline |
| `Rate_M` | `RM_scheduler` | `tx_power`, `frequency` | Fixed-power baseline |

The fixed-power baselines are normally run at both 10 W and 25 W, the two upper levels CATS predicts over, so CATS can be compared against a baseline burning comparable energy.

> **Note:** `CHEDF_scheduler` is a deliberate copy of `CHARM_scheduler` that differs only in its queue comparator, and `SEDF_scheduler` is the same copy of `SRM_scheduler`. Any change to the shared retransmission or listening logic has to be applied to both files of a pair, or the comparison stops being interpretable.

### The static-channel baselines, SRM and SEDF

SRM and SEDF replace CHARM's refreshed prediction with a single number fixed before the run starts: the channel's long-run mean decode probability at the scheduler's transmit power,

$$\bar{\rho}_j = \sum_s \pi_s \, \rho_s\!\left(\mathrm{SNR}(P_j)\right)$$

where $\rho_s$ is the decode probability of channel state $s$ and $\pi_s$ is the long-run fraction of time the channel spends in that state, so the more frequent conditions weigh more. They then allocate redundant transmissions from that average with CHARM's accumulated-probability rule. Because the estimate never moves, the redundancy is deterministic: every instance of a task is sent the same number of times, whatever the channel is actually doing.

This isolates what *refreshed* channel information adds beyond knowing only the channel's average quality — the RM/SRM gap is what redundancy-from-the-average buys over no redundancy at all, and the SRM/CHARM gap is what tracking the channel adds on top.

Two consequences worth keeping in mind when reading a figure:

- **They never listen.** A static estimate learns nothing from an RX slot, so SRM and SEDF have no `rx_period` and spend every slot transmitting or idle. They therefore get more transmit opportunities than CHARM/CHEDF at the same power, and an SRM/CHARM gap mixes the information effect with that airtime difference. Read it alongside the energy row.
- **The estimate comes from the channel, not from a config field.** `BasePhysicalChannel::mean_probability()` supplies it, and `main.cpp` queries it once at construction. `SigmoidChannel` computes $\pi$ as the stationary distribution of its own transition matrix; `ReplayChannel` returns the empirical decode rate of the recorded window. A new channel has to implement the method — it is pure virtual.

## Build

```bash
make            # produces the executable ./main.o
make clean
```

The top-level Makefile recurses into `system_model/`, `packet_generators/`, `schedulers/` and `physical_channels/`, compiles each component into an object file in the repository root, then links everything into `main.o`.

**Prerequisites**

- `g++` with C++17 support (the code uses `std::optional`)
- `make`
- Python 3.8 or newer with `matplotlib`, for the harness and the plots
- [nlohmann/json](https://github.com/nlohmann/json) and [kovian](https://github.com/RubyGB/kovian), both vendored in `libs/`

Header dependencies are not tracked by the Makefile, so run `make clean` before `make` after editing a header. After any C++ edit, rebuild before running the Python harness.

## Running a single simulation

```bash
./main.o [config_file] [log_file] [summary]
```

Defaults are `simulation_config.json` for the config and `simulation_log.json` for the log. Passing `summary` as the third argument silences stdout, skips the per-frame JSON log and writes only a small aggregate, which is what the harness uses.

### Config format

`simulation_config.json` has four sections:

```json
{
    "simulation": {
        "duration": 1500,
        "predict_error": 0.0,
        "window_k": 100,
        "seed": 42
    },
    "scheduler": {
        "type": "CATS",
        "frequency": 14074000,
        "belief_threshold": 0.7,
        "utilization_threshold": 0.9
    },
    "channels": [
        { "type": "sigmoid", "name": "channel_20m", "frequency": 14074000 }
    ],
    "packet_generators": [
        {
            "type": "fixed_rate",
            "packets": [
                {
                    "id": 1,
                    "relative_deadline": 24,
                    "frames": 1,
                    "reliability": 0.56,
                    "period": 24,
                    "phase": 0
                }
            ]
        }
    ]
}
```

- `simulation`: `duration` in frames, plus the optional `seed`, `predict_error` (half-width of the uniform noise injected into each predicted decode probability) and `window_k`.
- `scheduler`: `type` plus the parameters listed in the table above.
- `channels`: `sigmoid` takes `name` and `frequency`; `replay` takes `frequency` and `csv_path`.
- `packet_generators`: only `fixed_rate` is wired in. Packet fields are `id`, `period`, `relative_deadline`, `frames` (transmission length in slots), `reliability` (required reception probability) and `phase`.

## Running the experiment harness

`run_simulation.py` generates task sets with UUniFast, rewrites the config per run, invokes the C++ binary in parallel across every (scenario, utilization, scheduler, seed) point, and plots the result.

```bash
python run_simulation.py <n_runs> <mode> <run_name> [belief_threshold] [utilization_threshold]
```

Three modes:

- `sweep`: schedulability and energy against utilization, one curve per scheduler, at zero prediction error, so no gap on the figure can be blamed on predictor noise. Thirteen schedulers are compared, covering the full cross of channel knowledge and queue discipline.
- `error_sweep`: the same sweep repeated at each level in `PREDICT_ERRORS` (0.0, 0.15, 0.30), drawing one curve per predictor-sensitive scheduler per error level. It uses a reduced roster of seven, all deadline-ordered, so the surviving gaps come from the power policy rather than the queue discipline. EDF and SEDF ignore the predictor, so each contributes one flat curve.
- `tests`: per-test diagnostic runs. This is by far the heaviest mode, because it parses the full simulation log in Python.

An unknown mode is rejected before any work starts.

Outputs land under `tests/<run_name>/`. Utilization is swept over 0.1 to 1.0 and three scenarios vary the task count (4, 10 and 20 tasks).

### Environment variables

| Variable | Effect |
|---|---|
| `SEED` | Master seed. Per-simulation seeds are derived deterministically, so the same value reproduces identical figures. Unset means clock-based and non-deterministic. |
| `WINDOW_K` | Overrides the weakly-hard window length (default 100). |
| `PLOT_FORMATS` | Comma-separated output formats, default `png,pdf`. |
| `MPL_USETEX` | Renders figure text through a real LaTeX installation instead of matplotlib's mathtext. |

### Replay experiment

`hf_experiment/run.py` reuses the same sweep machinery over a `ReplayChannel`, which replays per-tick outcomes recorded in `frame_success.csv` and shows the predictor a fixed view of the channel.

```bash
SEED=42 python hf_experiment/run.py <n_runs> [sweep|error_sweep]
```

### Cluster runs

Publication-level results need much longer simulations than the defaults: hundreds of thousands of ticks per run, and enough independent runs per point that each curve is an average rather than a single draw. That is hours of CPU time even with the sweep parallelised, so a cluster is usually the practical way to produce them. `run_slurm.sh` is there to help with that on SLURM-based systems.

```bash
sbatch run_slurm.sh <run_name> [belief_threshold] [utilization_threshold]
sbatch --export=N_RUNS=200,MODE=error_sweep,SEED=42,ALL run_slurm.sh <run_name>
```

The script requests the resources through `#SBATCH` directives, loads the toolchain and Python environment, and then invokes `run_simulation.py` with `N_RUNS`, `MODE` and the usual environment variables passed in via `--export`. It ships configured for the AIRE cluster at Leeds, so the notification address, time limit, CPU count, `module load` lines and conda environment name at the top of the file are the parts to adapt to another site.

It deliberately does **not** build, because concurrent jobs would race on `main.o`, so run `make` on a login node first. See [SLURM.md](SLURM.md) for submission and monitoring recipes.

## Metrics

Four metrics are computed from a single pass over the per-instance delivered sequence:

- **`sched_ratio`**: fraction of packet ids whose whole-run delivery rate met their `reliability`. Insensitive to *when* misses land, since an early burst is averaged away by a long clean tail.
- **`sched_ratio_mk`**: fraction of ids meeting a weakly-hard (m,k)-firm constraint, that is, at least `m = ceil(reliability * k)` deliveries in every window of `k` consecutive instances. Windows slide rather than tumble, so a burst straddling a boundary cannot be masked twice. Burst-sensitive, and it converges to `sched_ratio` as `k` grows.
- **`max_burst`**: longest run of consecutive undelivered instances. Parameter-free, and often the quantity a control loop actually cares about. Aggregated but not plotted, because it spans three orders of magnitude across utilization.
- **`total_energy`**: sum of transmission power over every transmitted slot.

Two constraints bound `k`, and the harness warns at runtime when either is violated. It must be at least `1/(1 - reliability)`, or `m` equals `k` and the window degenerates into a zero-miss requirement. It must also be no larger than the number of instances an id produces, or that id has no complete window and is scored as vacuously met.

Because an id passes only if every window holds, `sched_ratio_mk` can only decrease as a run gets longer. Comparisons between schedulers are therefore valid only at equal `duration`.

## Reproducibility

Set `SEED` before running the harness:

```bash
SEED=42 python run_simulation.py 50 sweep my-run
```

A per-simulation seed is then derived from a hash of the master seed, the test name and the run index. It seeds Python's `random` for UUniFast and is propagated to the C++ side, where the receiver, the sigmoid channel and the Markov chain each get a distinct sub-seed.

## Outputs

- `simulation_log.json`: per-tick log. Large, roughly 8 MB for a 5000-frame run.
- `generated_packets.json`, `received_packets.json`, `<scheduler>_scheduled_packets.json`: per-component logs.
- `results_*.png` and `results_*.pdf`: figures from the harness. PDFs embed Type 42 fonts, since IEEE and ACM submission checks reject Type 3.

All JSON, PNG, PDF and SVG outputs are gitignored.

### Log formats

```jsonc
// generated_packets.json
{
  "system_tick": 0, "packet_id": 1, "packet_id_count": 0,
  "deadline": 24, "frames": 1, "period": 24,
  "reliability_req": 0.56, "is_periodic": true,
  "generator_type": "FixedRate"
}

// received_packets.json
{
  "system_tick": 1, "packet_id": 2, "packet_id_count": 0,
  "frame_count": 0, "frames": 1, "deadline": 4,
  "frequency": 14074000, "transmission_power": 10,
  "success_prob": 0.9329131, "reliability_req": 0.77,
  "received": true
}
```

## Project structure

```
main.cpp                         # config-driven simulator entry point
run_simulation.py                # experiment harness: task sets, sweeps, plots
run_slurm.sh                     # SLURM submission script
schedulers.hpp                   # umbrella include for all schedulers
schedulers/
  base_scheduler.hpp             # interface
  cats/                          # CATS, channel and belief aware
  charm/                         # CHARM baseline
  chedf/                         # CHARM's policy over an EDF queue
  srm/                           # static-channel-average redundancy, RM order
  sedf/                          # the same over an EDF queue
  earliest_deadline_first/       # EDF baseline
  rate_monotonic/                # Rate-Monotonic baseline
system_model/
  system_model.hpp               # shared types
  buffer_packet/                 # packet buffer and deadline enforcement
  radio_interface/               # transmitter side
  target_receiver/               # receiver side
  ml_predictor/                  # channel-state predictor used by CATS
packet_generators/
  fixed_rate/                    # the only generator currently wired in
physical_channels/
  sigmoid_channel/               # SNR to success-probability sigmoid
  replay_channel/                # replays recorded per-tick outcomes
  channel_20m/                   # FSMC state data for the 20 m band
hf_experiment/                   # replay sweep built on run_simulation.py
libs/                            # vendored dependencies
```

## Extending the simulator

Each component directory carries its own Makefile, invoked from the parent one.

**A new scheduler**: subclass `BaseScheduler`, implement `do_schedule_frame()` and `get_name()`, add a Makefile under `schedulers/<name>/`, wire it into the parent Makefile, add the include to `schedulers.hpp`, and extend the `sched_type` chain in `main.cpp`.

Two traps worth knowing about:

- `get_prediction_powers()` must return the power the scheduler actually transmits at. CHARM once hardcoded 10 W while transmitting at its configured power, which silently made a 25 W run decide retransmissions from the 10 W decode probability.
- `get_name()` has to embed the power for any scheduler type that runs at more than one power level, or two runs overwrite each other's `<scheduler>_scheduled_packets.json`.

**A new packet generator**: subclass `BasePacketGenerator`, implement `generate_packets()`, add a Makefile under `packet_generators/<name>/` and wire it into the parent Makefile.

**A new channel model**: subclass `BasePhysicalChannel`, implement `gen_frame_with_probability()` and `gen_probability()`, add a Makefile under `physical_channels/<name>/` and wire it into the parent Makefile.

## Code quality

The codebase is cppcheck compliant with warning, style, performance and portability checks enabled. Static analysis runs in CI from `.github/workflows/static-analysis.yml`. Please do not introduce new warnings.

Shared mutable state is passed with `std::shared_ptr`, as with `system_tick`, `buffer_packet` and `spawn_log` in `main.cpp`.

## Credits

- **[nlohmann/json](https://github.com/nlohmann/json)** by Niels Lohmann, JSON for Modern C++ (MIT License)
- **[kovian](https://github.com/RubyGB/kovian)**, a single-header library for finite state space Markov chain simulations

## License

See [LICENSE](LICENSE) for details.

## Contributing

Active work happens on `dev`, and pull requests land on `main`. Please make sure that:

- Code follows the existing style conventions
- New components include their own Makefile
- Static analysis stays clean
