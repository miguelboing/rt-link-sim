#pragma once

#include "schedulers/policies/feasibility.hpp"

/* ── Urgency: is this slot affordable, or is a packet depending on it? ───────

   A scheduler that wants to refresh its channel estimate has to stop
   transmitting to do it, and the slot it gives up is a slot the queue does not
   get. This policy answers the one question that stands between a stale
   estimate and an RX slot: can the queue spare it?

   It cannot, when some queued packet has *exactly* enough slots left to make
   its deadline -- the equality case `classify_feasibility()` reports as
   `any_urgent`. One slot spent elsewhere turns that packet infeasible, so a
   listen now is paid for with a deadline miss. Holding off keeps the packet,
   at the cost of transmitting on an estimate the scheduler already distrusts.

   Which way to resolve that is a *policy*, not a fact about the queue, and
   schedulers differ: one declines the slot and sends on the stale estimate,
   another listens regardless and accepts the occasional miss to keep its
   listening rule as simple as it can be. That is why this is
   a separate question from the classification, and why holding a
   `feasibility_t` implies nothing about the answer.

   Stateless, because the answer is a pure function of this slot's
   classification and a configured switch.

   Note what this deliberately does *not* decide: whether the estimate is worth
   refreshing at all. That is the staleness test, and it belongs to whatever
   listening rule the scheduler runs. A caller composes the two, and the order
   matters only for cost, not for the outcome. */

/* True when a slot may be given up for listening.

   `urgency_check` off makes this constantly true, which is the ablation arm:
   the scheduler then listens whenever its estimate is stale, whatever the
   queue is about to lose. The switch governs *this* decision and nothing else
   -- in particular it does not reach the classification, so both settings see
   the same packets classified the same way and any gap between them is this
   rule alone.

   Taking the whole `feasibility_t` rather than a bare `bool` is deliberate: a
   function of two booleans can be called with them the wrong way round and
   still compile, and the error would be silent -- an inverted urgency term does
   not crash, it just quietly answers the other policy's question. */
inline bool listening_affordable(const feasibility_t& feasibility,
                                 bool urgency_check)
{
    return !urgency_check || !feasibility.any_urgent;
}
