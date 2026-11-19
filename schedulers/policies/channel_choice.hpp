#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

#include "schedulers/base_scheduler.hpp"

/* ── Channel choice: ranking by long-run mean ────────────────────────────────

   Which of the available channels are worth using, decided from the fitted
   models alone and so answerable before a single slot has been observed.

   The ranking is each channel's long-run mean decode probability at the power
   in question,

       rho_bar_{c,j} = sum_s pi_{c,s} rho_{c,s}(SNR(P_j)),

   averaged over that channel's own stationary distribution, so a channel is
   judged on how it behaves over a run rather than on its best state. It is a
   *static* ranking: it uses no observation and gives the same answer whenever
   it is asked, which is what makes it a decision to take once at construction
   rather than per slot.

   Read off `rho_hat` with no observation recorded, which by eq. 27 is exactly
   that stationary mean. A caller that has been observing a channel will get
   the ranking of its *current beliefs* instead, which is a different question
   and wants a different function -- this one is only meaningful before
   anything has been heard, or for a scheduler that never listens.

   Free functions, because the choice carries no state: they read the models
   and answer.

   ⚠️ Ties go to the lowest index, and are reachable rather than theoretical:
   two channels built from the same curve mix have the same mean to the bit.
   Deterministic, so two schedulers using this rule always land on the same
   channels, which is what makes a comparison between them about what they do
   *after* choosing. */

/* Every channel index, best mean first, worst last. */
inline std::vector<std::size_t>
rank_channels_by_mean(const std::vector<channel_estimate_t>& channels,
                      unsigned int transmission_power)
{
    std::vector<std::size_t> order(channels.size());
    std::iota(order.begin(), order.end(), 0U);

    /* Stable, so equal means keep index order and the tie rule above holds. */
    std::stable_sort(order.begin(), order.end(),
        [&channels, transmission_power](std::size_t a, std::size_t b) {
            return channels[a].model.rho_bar(transmission_power)
                 > channels[b].model.rho_bar(transmission_power);
        });

    return order;
}

/* The single best channel: the head of the ranking. */
inline std::size_t best_mean_channel(const std::vector<channel_estimate_t>& channels,
                                     unsigned int transmission_power)
{
    const std::vector<std::size_t> order = rank_channels_by_mean(channels, transmission_power);

    return order.empty() ? 0U : order.front();
}
