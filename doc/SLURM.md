# SLURM Cheatsheet

Reference for working with a SLURM-based cluster (the directives in this project are set up for AIRE at Leeds). Project context: jobs are submitted via `run_slurm.sh`, with `--job-name=cats-sweep` and outputs to `cats-<jobid>.out` / `cats-<jobid>.err`.

## Submitting jobs

```bash
sbatch run_slurm.sh <run_name> [belief_threshold] [utilization_threshold]

# Override defaults via --export
sbatch --export=N_RUNS=100,MODE=sweep,ALL run_slurm.sh my-run

# Override resource requests on the command line (overrides #SBATCH directives)
sbatch --time=08:00:00 --cpus-per-task=64 run_slurm.sh my-run
```

`ALL` in `--export` means "also pass through my current environment"; without it, the job starts with a clean env.

## Inspecting the queue

```bash
squeue -u $USER                 # your jobs
squeue -u $USER -t RUNNING      # only running
squeue -u $USER -t PENDING      # only queued
squeue --me                     # shorthand for -u $USER on newer SLURM
squeue -j <jobid>               # one specific job
squeue -n cats-sweep            # by job name
```

State codes you'll see:
- `R` running, `PD` pending, `CG` completing, `CD` completed
- `F` failed, `TO` timeout, `CA` cancelled, `OOM` out-of-memory

Why is my job pending? `Reason` column tells you: `Priority`, `Resources`, `QOSMaxJobsPerUserLimit`, `Dependency`, etc.

## Cancelling jobs

```bash
scancel <jobid>                 # one job
scancel -u $USER                # all your jobs
scancel -n cats-sweep           # by job name
scancel <jobid>_<step>          # one step of an array
scancel --state=PENDING -u $USER   # only queued, leave running alone
scancel --signal=TERM <jobid>   # send SIGTERM first (graceful) before SIGKILL
```

## Monitoring a running job

```bash
sstat -j <jobid> --format=JobID,AveCPU,AveRSS,MaxRSS,NTasks
squeue -j <jobid> -O JobID,State,TimeUsed,TimeLimit,NumCPUs

# Tail live stdout/stderr (run_slurm.sh writes cats-<jobid>.out / .err)
tail -f cats-<jobid>.out
tail -f cats-<jobid>.err
```

## Post-mortem on a finished job

```bash
sacct -j <jobid>                                          # exit code, state
sacct -j <jobid> --format=JobID,JobName,State,ExitCode,Elapsed,MaxRSS,ReqMem,NCPUs
sacct -u $USER --starttime=$(date -d '7 days ago' +%F)    # week of history

seff <jobid>                    # human-readable CPU/mem efficiency report
```

`seff` is the quickest sanity check after a run finished. It tells you if you over- or under-requested resources.

## Cluster info

```bash
sinfo                           # partition / node state overview
sinfo -p <partition> -o "%P %a %l %D %T %N"
sinfo -T                        # reservations
scontrol show job <jobid>       # full job detail (incl. submit line, working dir, env)
scontrol show node <nodename>
scontrol show partition <partition>

module avail                    # what software modules are available
module avail gcc                # filter
module list                     # currently loaded
```

## Interactive sessions

For debugging on a compute node (don't run heavy work on login nodes):

```bash
srun --pty --time=01:00:00 --cpus-per-task=4 --mem=8G bash
salloc --time=01:00:00 --cpus-per-task=8 --mem=16G    # then ssh into the allocated node
```

## Job arrays

Useful when sweeping a parameter that the wrapper script can read from `$SLURM_ARRAY_TASK_ID`:

```bash
sbatch --array=0-9 run_slurm.sh    # 10 tasks, IDs 0..9
sbatch --array=0-99%10 ...        # 100 tasks, max 10 concurrent
scancel <jobid>_5                 # cancel one task
scancel <jobid>                   # cancel whole array
```

## Project-specific gotchas

- **The conda env must exist before you submit.** `run_slurm.sh` activates `rt-link-sim`, falling back to the pre-rename name `cats-scheduler`. A job that dies immediately with `EnvironmentNameNotFound: Could not find conda environment` has neither. Create it once on a login node with `conda create -n rt-link-sim python=3.12 numpy matplotlib`, rename an existing one with `conda rename -n cats-scheduler rt-link-sim`, or point the script elsewhere with `sbatch --export=CONDA_ENV=my-env,ALL ...`.
- **Don't `make` inside the job.** `run_slurm.sh` deliberately skips the build because concurrent jobs race on `main.o` (Makefile `rm`s it then recreates it, leaving a window where the binary is missing). Always run `make` on a login node before `sbatch`.
- **Login node is shared.** Heavy `make`/Python on the login node will make you unpopular, so use `srun --pty bash` for anything non-trivial.
- **Time limit is wall clock**, not CPU time. With `--cpus-per-task=32` and `--time=04:00:00`, you have 4 hours wall, not 128 CPU-hours.
- **`--mem-per-cpu=1G`** combined with `--cpus-per-task=32` requests 32 GB total. Sweep mode (the default) only emits a tiny summary file per sim, so workers stay at tens of MB. Tests mode parses the full ~180 MB JSON log per worker, so bump to `--mem-per-cpu=2G` if running tests with many parallel runs. Mixing `--mem` and `--mem-per-cpu` is rejected.

## Useful env vars inside a job

```
$SLURM_JOB_ID            # numeric job id
$SLURM_JOB_NAME          # cats-sweep
$SLURM_SUBMIT_DIR        # cwd at sbatch time (run_slurm.sh cd's here)
$SLURM_CPUS_PER_TASK     # passed to ProcessPoolExecutor as n_workers
$SLURM_ARRAY_TASK_ID     # only set inside array jobs
$SLURM_ARRAY_JOB_ID      # the parent array's job id
```
