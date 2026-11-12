#pragma once

#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"

/* MPEDF -- "Mean-Predictor EDF". CHEDF's accumulated-probability redundancy
   rule over an EDF queue, driven by a *static* estimate of the channel
   instead of a refreshed prediction.

   The estimate is the channel's long-run mean decode probability at this
   scheduler's transmit power,

       rho_bar_j = sum_s pi_s rho_s(SNR(P_j)),

   with pi the stationary distribution of the channel's FSMC and rho_s the
   per-state decode curve. It is computed once, before the run starts
   (main.cpp asks the channel via BasePhysicalChannel::mean_probability()),
   and never updated. Because the value never moves, the redundancy it buys is
   the same for every frame of a given task: the rule degenerates to "repeat
   each frame ceil(log(1 - theta^(1/L)) / log(1 - rho_bar)) times", regardless
   of the state the channel is actually in.

   Where it sits: EDF sends each frame once at a fixed power. CHEDF sizes the
   redundancy from a prediction it refreshes whenever its belief in it decays
   past a threshold. MPEDF sizes it from average channel quality alone. So an
   EDF/MPEDF gap is what redundancy-from-the-average buys over no redundancy,
   and an MPEDF/CHEDF gap is what *refreshed* channel information adds on top
   of knowing only the channel's long-run quality -- which is the baseline this
   scheduler exists to provide.

   It never enters RX_MODE. A static estimate learns nothing from listening, so
   unlike CHEDF it spends every slot transmitting or idle, and it has no
   listening knob at all. This is a deliberate asymmetry and it does mean MPEDF
   gets more transmit opportunities than CHEDF, so an MPEDF/CHEDF gap mixes the
   information effect with that airtime difference. Since CHEDF's listening is
   belief-driven, the size of that difference is not fixed -- it shrinks as the
   queue gets busier and CHEDF listens less.

   MPRM, the period-ordered half of this pair, is gone: the first paper settled
   EDF over RM, so the period-ordered arm of every pair was retired and this
   class is now the sole owner of the rule. (It was previously a verbatim copy
   of schedulers/mprm/, with a keep-both-in-sync hazard that no longer
   exists.) */
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

    /* Ignores predictions, and asks for none -- see the class comment. */

    std::string get_name() const override;
};
