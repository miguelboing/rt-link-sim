#pragma once

#include <cstddef>
#include <vector>

#include "schedulers/base_scheduler.hpp"

/* ── Channel choice: the best long-run mean ──────────────────────────────────

   Which of the available channels to commit to, decided from the fitted models
   alone and so answerable before a single slot has been observed.

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

   A free function, because the choice carries no state: it reads the models
   and answers.

   ⚠️ Ties go to the lowest index, and are reachable rather than theoretical:
   two channels built from the same curve mix have the same mean to the bit.
   Deterministic, so two schedulers using this rule always land on the same
   channel, which is what makes a comparison between them about what they do
   *after* choosing. */
inline std::size_t best_mean_channel(const std::vector<channel_estimate_t>& channels,
                                     unsigned int transmission_power)
{
    std::size_t best      = 0U;
    double      best_mean = -1.0;

    for (std::size_t c = 0; c < channels.size(); c++)
    {
        const double mean = channels[c].model.rho_bar(transmission_power);

        if (mean > best_mean)
        {
            best_mean = mean;
            best      = c;
        }
    }

    return best;
}
