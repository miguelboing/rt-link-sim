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

A rule with one caller today also belongs here if it is a self-contained
question rather than a scheduler's private bookkeeping, and especially if
splitting it out removes a trap. `feasibility.hpp` is the case in point: "can
this packet still make its deadline" is a question any deadline-driven
scheduler can ask, and because the policy *cannot* remove a packet, the
split between classifying and dropping is now structural rather than a rule
someone has to remember.

`urgency.hpp` is the same argument from the other side. Whether the queue can
spare a slot is one question; whether a packet is about to miss its deadline is
another, and only the second is a fact about the queue. Keeping them apart is
what lets a scheduler read the classification without being committed to any
particular way of resolving it — which is what the shipped schedulers differ
on, and the reason it stays here on one caller: every other scheduler that
classifies its queue resolves the question by listening anyway.

**Not here:**

- **The channel estimate.** `BaseScheduler`'s channel set — the fitted FSMC
  `M_hat_c` of every channel and the latest label `s_hat_c(t)` inferred on each
  — is the scheduler's *model of the world*, not a decision about it, and it is
  the paper's §3.2.1. It stays in `schedulers/base_scheduler.hpp` because every
  scheduler that reasons about the channel reads the same estimate, whatever it
  then decides.
- **A scheduler's private bookkeeping.** A scheduler's own members and the
  per-release state it keeps for them stay with it. Lifting something here
  only to give it one caller and a longer include path buys nothing — though a
  rule *about* that state can still belong here, as the power-cap policy does:
  the cap is the scheduler's member, and only the rule for what it should
  become is shared.
- **The action, as opposed to the decision.** Dropping a packet is paired with
  forgetting whatever per-release state the scheduler kept for it, which a
  policy cannot know about. `feasibility.hpp` says which packets are hopeless;
  the caller decides whether to remove them.
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
| `power_tier.hpp` | The powers a scheduler may transmit at, and the two decisions that follow from having more than one. `slack_power_cap()` picks the lowest tier whose predicted utilization of the known task set fits under a threshold; `select_power_tier()` picks the cheapest tier at or below that cap which finishes the frame, and reports -1 rather than deciding what to do when none does. The probability at a tier is a caller-supplied callable, because only the scheduler knows which channel it could spend that tier on. `POWER_TIERS` lives here so there is exactly one copy of it. |
| `feasibility.hpp` | `retx_count_required()` — transmissions a frame needs to clear its reliability target at a given decode probability — and `classify_feasibility()`, which runs that over the queue and returns the packets that can no longer make their deadline plus whether any has exactly enough slots left. Classifies only; removal is the caller's. |
| `urgency.hpp` | `listening_affordable()` — whether this slot may be given up for listening, from the `feasibility_t` of the same slot and the caller's switch. False while some packet has exactly enough slots left, because that listen is paid for with its deadline. Answers affordability only: whether the estimate is worth refreshing at all is the staleness test, and stays with the scheduler. |
| `rm_queue.hpp` | `select_shortest_period()` — rate-monotonic order, the fixed-priority counterpart to the one below. Needs a periodicity guard the deadline rule does not: an aperiodic packet has no period to rank by, so it sorts behind every periodic one and is only returned when nothing else is queued. |
| `edf_queue.hpp` | `select_earliest_deadline()` — the queued packet with the earliest absolute deadline, or `nullptr` for an empty queue. Ties break on buffer position, which is release order. |

## Build

Header-only, so there is no Makefile here and `schedulers/Makefile` needs no
rule for it. A policy that ever grows a `.cpp` needs both — a Makefile in this
directory and a rule in the parent, like any scheduler subdirectory.

⚠️ The top-level Makefiles track **no header dependencies**, so editing
anything here leaves every dependent object stale. Run `make clean` after
touching a policy, and always before trusting an A/B comparison between two
builds.
