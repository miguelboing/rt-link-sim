#pragma once

#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "packet_generators/base_packet_generator.hpp"
#include "schedulers/policies/feasibility.hpp"

/* ── Power tiers: how much to spend on a transmission ────────────────────────

   A scheduler that may transmit at more than one power has two decisions to
   make, and they are separate questions asked at different rates:

     1. *How high may it go at all* -- a cap, recomputed whenever the estimates
        move. It is a property of the task set: if the work fits in the slots
        available at a cheap tier, the expensive tiers buy nothing but energy.
     2. *What does this frame need* -- a per-transmission choice, the cheapest
        tier at or below the cap whose decode estimate finishes the frame.

   Both are here because more than one scheduler makes them, and because the
   tier list itself is the kind of constant that must have exactly one
   definition: a scheduler sizing retransmissions from one power while
   transmitting at another is the failure this directory exists to prevent, and
   three copies of `{1, 10, 25}` is how that happens.

   Stateless free functions. The cap is state, but it belongs to the scheduler
   that holds it -- these only compute what it should become.

   The probability at a tier is supplied by the caller as a callable taking the
   tier index, not read from a channel here. That is deliberate: on a
   single-channel scheduler it is one channel's estimate, and on a scheduler
   that re-chooses the channel it is whatever its own channel rule can deliver
   at that tier, which only that scheduler knows. */

/* The powers a scheduler may transmit at, low to high. These are the tiers the
   predictor's state label is read at -- one label, evaluated once per tier
   against the fitted response curves, so all of them share one observation and
   one age. */
inline constexpr unsigned int POWER_TIERS[]   = {1U, 10U, 25U};
inline constexpr unsigned int N_POWER_TIERS   = 3U;
inline constexpr unsigned int TOP_POWER_TIER  = N_POWER_TIERS - 1U;

/* Index of `power` in POWER_TIERS, or -1 if it is not one of them. A scheduler
   pinned to a fixed power has to name a tier: the alternative is sizing
   retransmissions from a curve it does not transmit at. */
inline int power_tier_index(unsigned int power)
{
    for (unsigned int i = 0U; i < N_POWER_TIERS; i++)
        if (POWER_TIERS[i] == power) return static_cast<int>(i);

    return -1;
}

/* The tier list as text, for the error message a bad fixed power earns. */
inline std::string power_tiers_string(void)
{
    std::string out;
    for (unsigned int i = 0U; i < N_POWER_TIERS; i++)
    {
        if (i > 0U) out += ", ";
        out += std::to_string(POWER_TIERS[i]);
    }

    return out + " W";
}

/* The horizon the task set is costed over: the LCM of the periods, i.e. the
   hyperperiod. Demand within one hyperperiod is exact, since every task
   completes a whole number of releases in it, which makes the utilization
   estimate tighter than one taken over max(T_i). Zero when there are no
   periodic tasks to cost. */
inline unsigned int task_hyperperiod(const std::vector<periodic_task_t>& tasks)
{
    unsigned int H = 0U;

    for (const auto& t : tasks)
    {
        if (t.period == 0U) continue;
        H = (H == 0U) ? t.period : std::lcm(H, t.period);
    }

    return H;
}

/* Slots the task set demands within `H` at per-attempt decode probability `p`:

       Demand = sum_i ceil(H / T_i) * C_i * retx_count_required(p, RR_i^(1/C_i))

   Infinite if any task cannot clear its requirement at `p` at all, which is
   how an unusable tier reports itself rather than as a very large number. */
inline double demand_slots(const std::vector<periodic_task_t>& tasks,
                           unsigned int H, double p)
{
    if (H == 0U) return 0.0;

    double demand = 0.0;
    for (const auto& t : tasks)
    {
        if (t.period == 0U) continue;

        const double k = retx_count_required(p, std::pow(t.reliability_req,
                                                         1.0 / t.frames));
        if (std::isinf(k)) return std::numeric_limits<double>::infinity();

        const double releases = std::ceil(static_cast<double>(H) /
                                          static_cast<double>(t.period));
        demand += releases * static_cast<double>(t.frames) * k;
    }

    return demand;
}

/* Utilization at `p`: the demand above as a fraction of the horizon. Infinite
   where the demand is, and 0 with no periodic tasks -- nothing to schedule
   fits under any threshold. */
inline double tier_utilization(const std::vector<periodic_task_t>& tasks,
                               unsigned int H, double p)
{
    if (H == 0U) return 0.0;

    return demand_slots(tasks, H, p) / static_cast<double>(H);
}

/* The slack-aware cap: the lowest tier whose predicted utilization fits under
   `threshold`.

   Walking low-to-high and stopping at the first that fits is what makes this a
   *slack* policy rather than a power minimiser -- it spends the least that
   leaves the schedule feasible, and only climbs when the channel estimates say
   the cheap tiers no longer carry the load. If even the top tier does not fit,
   the top tier is the answer: the schedule is overloaded, and refusing to
   transmit would not help it. */
template <typename ProbAtTier>
inline unsigned int slack_power_cap(const std::vector<periodic_task_t>& tasks,
                                    unsigned int H, double threshold,
                                    ProbAtTier prob_at)
{
    for (unsigned int i = 0U; i < N_POWER_TIERS; i++)
        if (tier_utilization(tasks, H, prob_at(i)) <= threshold) return i;

    return TOP_POWER_TIER;
}

/* The tier to transmit this frame at: the lowest in [lo, cap] whose decode
   estimate takes the frame's accumulated success probability to `req`, or -1
   when none of them finishes it.

   -1 is not a refusal to transmit. It says no permitted tier *completes* the
   frame this slot, which leaves the caller to spend the cap tier and come back
   next slot -- the accumulated-probability rule, with the cap as the ceiling on
   what one attempt may cost. Returning the information rather than the decision
   is what lets a scheduler keep its own retransmission bookkeeping. */
template <typename ProbAtTier>
inline int select_power_tier(ProbAtTier prob_at, unsigned int lo,
                             unsigned int cap, double acc, double req)
{
    for (unsigned int i = lo; i <= cap; i++)
    {
        const double p_i = prob_at(i);

        if (acc + p_i - acc * p_i >= req) return static_cast<int>(i);
    }

    return -1;
}
