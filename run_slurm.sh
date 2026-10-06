#!/bin/bash
#SBATCH --mail-type=BEGIN,END,FAIL
#SBATCH --mail-user=gcsv9491@leeds.ac.uk
#SBATCH --job-name=cats-sweep
#SBATCH --output=cats-%j.out
#SBATCH --error=cats-%j.err
#SBATCH --time=5:00:00
#SBATCH --mem-per-cpu=1G
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
# Resource sizing (post -O2 + summary-mode optimization). Estimates below are
# for the current 5-scheduler roster (RM/CHARM at both 10 W and 25 W, + CATS);
# they were ~1.6x lower back when SCHEDULERS held only 3 entries, so rescale
# if you add or drop a scheduler.
#   - sweep       @ 100k ticks, n_runs=2:   ~2 min wall on 32 CPUs
#   - sweep       @ 100k ticks, n_runs=50:  ~8 min  (prior default)
#   - sweep       @ 100k ticks, n_runs=500: ~75 min
#   - error_sweep @ 250k ticks, n_runs=50:  ~50 min  (scales with len(PREDICT_ERRORS) and
#                                                    duration; RM is dedup'd across errors)
#   - error_sweep @ 250k ticks, n_runs=500: ~8 h  -- exceeds the 5 h default below,
#                                                    submit with --time=12:00:00
# Sweep / error_sweep modes keep the worker memory footprint at tens of MB
# (no full log parsed in Python). Tests mode parses ~180 MB JSON per worker
# — if running tests with n_runs > 16 on this 32-CPU layout, raise
# --mem-per-cpu to 2G.

# Submit with:
#     sbatch run_slurm.sh <run_name> [belief_threshold] [utilization_threshold]
# Override defaults via --export, e.g.:
#     sbatch --export=N_RUNS=100,MODE=sweep,ALL       run_slurm.sh <run_name> [belief_threshold] [utilization_threshold]
#     sbatch --export=N_RUNS=200,MODE=error_sweep,ALL run_slurm.sh <run_name>
#     sbatch --export=N_RUNS=50,MODE=cats_comparison,ALL run_slurm.sh <run_name>
# Reproducible run (same SEED → bit-identical output PNGs):
#     sbatch --export=SEED=42,ALL run_slurm.sh <run_name>
# Override the weakly-hard (m,k) window used by the sched_ratio_mk curve
# (default 100, which makes m/k reproduce the 2-decimal reliability exactly;
# must be >= 10 for the 0.9 reliability tasks — see CLAUDE.md):
#     sbatch --export=WINDOW_K=50,ALL run_slurm.sh <run_name>

set -euo pipefail

# --- Modules ----------------------------------------------------------------
# Site-specific: adjust to whatever your cluster exposes (`module avail` to
# check). These names are the ones on AIRE (Leeds).
module load miniforge
module load gcc

# --- Python env -------------------------------------------------------------
# Create once on a login node:  conda create -n rt-link-sim python=3.12 numpy matplotlib
# The project was renamed from cats-scheduler, so accounts created before the
# rename still have an environment under the old name. Either name is accepted
# here; override both with  sbatch --export=CONDA_ENV=my-env,ALL ...
CONDA_ENV="${CONDA_ENV:-rt-link-sim}"
if ! conda activate "${CONDA_ENV}" 2>/dev/null; then
    if conda activate cats-scheduler 2>/dev/null; then
        echo "NOTE: conda env '${CONDA_ENV}' not found, using legacy 'cats-scheduler'." >&2
        echo "      Rename it on a login node:  conda rename -n cats-scheduler rt-link-sim" >&2
    else
        echo "ERROR: no conda env named '${CONDA_ENV}' or 'cats-scheduler'." >&2
        echo "       Create one on a login node:" >&2
        echo "         conda create -n ${CONDA_ENV} python=3.12 numpy matplotlib" >&2
        echo "       Environments visible to this job:" >&2
        conda info --envs >&2 || true
        exit 1
    fi
fi

# --- Workdir ----------------------------------------------------------------
cd "${SLURM_SUBMIT_DIR}"

# --- Build ------------------------------------------------------------------
# Build is intentionally NOT done here — concurrent jobs would race on main.o
# (the Makefile rm's then re-creates it, leaving a window where it's missing).
# Run `make` on a login node before submitting, and after any C++ edits.
if [ ! -x main.o ]; then
    echo "ERROR: main.o not found. Run 'make' on a login node before submitting." >&2
    exit 1
fi

# --- Run --------------------------------------------------------------------
# Stop BLAS/OMP from oversubscribing cores already taken by the worker pool.
export OMP_NUM_THREADS=1
export MKL_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=1

N_RUNS="${N_RUNS:-50}"   # runs averaged per (scenario, U, scheduler) point
MODE="${MODE:-sweep}"    # tests | sweep | error_sweep | cats_comparison
case "${MODE}" in
    tests|sweep|error_sweep|cats_comparison) ;;
    *) echo "ERROR: MODE='${MODE}' is not one of tests|sweep|error_sweep|cats_comparison." >&2
       echo "       A typo here used to run the far heavier 'tests' mode instead," >&2
       echo "       which OOM-kills the step at n_runs>16 on --mem-per-cpu=1G." >&2
       exit 1 ;;
esac
RUN_NAME="${1:-run-${SLURM_JOB_ID}}"
BELIEF_THRESHOLD="${2:-}"        # optional; if empty, run_simulation.py uses its default
UTILIZATION_THRESHOLD="${3:-}"   # optional; if empty, run_simulation.py uses its default
# Note: passing utilization_threshold requires belief_threshold to be set (positional args).

echo "Job ${SLURM_JOB_ID} | ${SLURM_CPUS_PER_TASK} CPUs | mode=${MODE} | n_runs=${N_RUNS} | name=${RUN_NAME} | belief=${BELIEF_THRESHOLD:-default} | util=${UTILIZATION_THRESHOLD:-default}"
python run_simulation.py "${N_RUNS}" "${MODE}" "${RUN_NAME}" ${BELIEF_THRESHOLD:+"${BELIEF_THRESHOLD}"} ${UTILIZATION_THRESHOLD:+"${UTILIZATION_THRESHOLD}"}
