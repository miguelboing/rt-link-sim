# `schedulers/policies/`

Decision rules that **more than one scheduler shares**, implemented once here
instead of copied into each.

A scheduler in `schedulers/*/` is a set of decisions: which packet to send, at
what power, on which channel, and when to give up a slot to listen instead.
Several of those decisions are identical across schedulers *by design* — a
comparison between two schedulers only isolates what it claims to isolate while
everything else about them is the same rule, not a copy of the same rule.
Anything in that category belongs here.

## What belongs here, and what does not

**Here:** a decision rule shared by two or more schedulers, with no state that
belongs to the simulator as a whole.

**Not here:**

- **The channel estimate.** `BaseScheduler`'s channel set — the fitted FSMC
  `M_hat_c` of every channel and the latest label `s_hat_c(t)` inferred on each
  — is the scheduler's *model of the world*, not a decision about it, and it is
  the paper's §3.2.1. It stays in `schedulers/base_scheduler.hpp` because every
  scheduler that reasons about the channel reads the same estimate, whatever it
  then decides.
- **A rule only one scheduler uses.** CATS's slack-aware power cap and its
  early-drop pass live in `schedulers/cats/`. Moving a single-user rule here
  buys nothing and makes it look shared.
- **Something the simulator owns.** The physical channel, the predictor and the
  packet buffer are not scheduler policy.

## Why shared and not inherited

A policy here is something a scheduler **holds or calls**, not a facility
`BaseScheduler` provides. The difference matters: a base-class facility asserts
that every scheduler works this way, so a scheduler wanting a different rule has
to opt out of something it never asked for. A policy asserts nothing — a
scheduler that shares the rule uses it, and one that does not simply does not.

So sharing a rule between some schedulers and *building it into all schedulers*
are different claims, and only the first is ever true here.

## Two shapes

Pick by whether the rule remembers anything between slots:

- **Stateful → a small class the scheduler owns as a member**, constructed with
  it and advanced explicitly. `belief_tracker.hpp` is this shape: the scheduler
  ticks it once per slot and reads it.
- **Stateless → a free `inline` function** taking what it needs as arguments.
  `edf_queue.hpp` is this shape: it reads the queue and answers.

Do not make a stateless rule a class for symmetry's sake; an object with no
members is a worse interface than a function.

## Contents

| | |
|---|---|
| `belief_tracker.hpp` | Belief in the label last heard on each channel, and therefore when to spend a slot refreshing it. Decays each channel by its believed state's `A_ss` once per slot; an observation restores it to 1. |
| `channel_choice.hpp` | `best_mean_channel()` — the channel with the highest fitted long-run mean decode probability at a given power. Static: it reads the models, not any observation, so it answers the same way whenever it is asked and is meant to be called once. Ties break on the lowest index. |
| `edf_queue.hpp` | `select_earliest_deadline()` — the queued packet with the earliest absolute deadline, or `nullptr` for an empty queue. Ties break on buffer position, which is release order. |

## Build

Header-only, so there is no Makefile here and `schedulers/Makefile` needs no
rule for it. A policy that ever grows a `.cpp` needs both — a Makefile in this
directory and a rule in the parent, like any scheduler subdirectory.

⚠️ The top-level Makefiles track **no header dependencies**, so editing
anything here leaves every dependent object stale. Run `make clean` after
touching a policy, and always before trusting an A/B comparison between two
builds.
