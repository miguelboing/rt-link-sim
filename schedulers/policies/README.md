# `schedulers/policies/`

Decision rules that **more than one scheduler shares**, implemented once here
instead of copied into each.

A scheduler in `schedulers/*/` is a queue discipline plus a set of decisions:
which packet to send, at what power, on which channel, and when to give up a
slot to listen instead. Several of those decisions are identical across
schedulers by design — a CHARM/CHEDF comparison is only interpretable while the
two differ *solely* in dequeue order, and a CATS ablation only while CATS
differs from the baselines in the one policy being ablated. Anything in that
category belongs here.

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

Each policy is a small class a scheduler **holds**, not a facility
`BaseScheduler` provides. The difference matters: a base-class facility asserts
that every scheduler works this way, so a scheduler wanting a different rule
has to opt out of something it never asked for. A held object asserts nothing —
a scheduler that shares the rule owns one, and a scheduler that does not simply
does not.

That is also why these are not in `schedulers/base_scheduler.hpp`: sharing a
rule between three schedulers and *building it into all schedulers* are
different claims, and only the first one is true.

## Contents

| | |
|---|---|
| `belief_tracker.hpp` | Belief in the label last heard on each channel, and therefore when to spend a slot refreshing it. Decays each channel by its believed state's `A_ss` once per slot; an observation restores it to 1. Held by CHARM, CHEDF and CATS, which share this listening rule. |

## Build

Header-only, so there is no Makefile here and `schedulers/Makefile` needs no
rule for it. A policy that ever grows a `.cpp` needs both — a Makefile in this
directory and a rule in the parent, like any scheduler subdirectory.

⚠️ The top-level Makefiles track **no header dependencies**, so editing
anything here leaves every dependent object stale. Run `make clean` after
touching a policy, and always before trusting an A/B comparison between two
builds.
