#pragma once

#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"

/* MPEDF -- "Mean-Predictor EDF". MPRM's policy over an EDF queue: CHARM's
   accumulated-probability redundancy rule driven by a *static* long-run mean
   decode probability,

       rho_bar_j = sum_s pi_s rho_s(SNR(P_j)),

   with packets dequeued by earliest deadline rather than shortest period.
   See schedulers/mprm/mprm_scheduler.hpp for what the estimate is, where it
   comes from, and why this family exists.

   MPEDF is to MPRM what CHEDF is to CHARM, and it is a standalone class for the
   same reason: the pair is only interpretable while the two differ *solely*
   in dequeue order, so the redundancy rule below is a deliberate copy rather
   than a shared one. Any change to the accumulated-probability rule or to the
   absence of an RX slot must be applied to both files. Nothing in the build
   enforces that.

   An MPEDF/CHEDF pair isolates what refreshed channel information adds at
   equal queue discipline; an MPRM/MPEDF pair isolates queue discipline at equal
   channel knowledge. */
class MPEDF_scheduler : public BaseScheduler
{
public:
    MPEDF_scheduler(unsigned int tx_power, std::size_t channel, double mean_prob, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;

    /* Static long-run decode probability at tx_power. Fixed for the whole
       run; this is the one input that distinguishes MPEDF from CHEDF. */
    double mean_prob;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Ignores predictions, and asks for none -- see MPRM_scheduler. */

    std::string get_name() const override;
};
