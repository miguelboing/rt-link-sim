#pragma once

#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"

/* MPRM -- "Mean-Predictor RM". Rate-monotonic dequeue order with CHARM's
   accumulated-probability redundancy rule, driven by a *static* estimate of
   the channel instead of a refreshed prediction.

   The estimate is the channel's long-run mean decode probability at this
   scheduler's transmit power,

       rho_bar_j = sum_s pi_s rho_s(SNR(P_j)),

   with pi the stationary distribution of the channel's FSMC and rho_s the
   per-state decode curve. It is computed once, before the run starts
   (main.cpp asks the channel via BasePhysicalChannel::mean_probability()),
   and never updated. Because the value never moves, the redundancy it buys
   is the same for every frame of a given task: the rule degenerates to
   "repeat each frame ceil(log(1 - theta^(1/L)) / log(1 - rho_bar)) times",
   regardless of the state the channel is actually in.

   Where it sits: RM and EDF send each frame once at a fixed power. CHARM and
   CHEDF size the redundancy from a prediction refreshed every rx_period
   ticks. MPRM and MPEDF size it from average channel quality alone. So an
   RM/MPRM gap is what redundancy-from-the-average buys over no redundancy,
   and an MPRM/CHARM gap is what *refreshed* channel information adds on top
   of knowing only the channel's long-run quality -- which is the baseline
   this scheduler exists to provide.

   It never enters RX_MODE. A static estimate learns nothing from listening,
   so unlike CHARM it spends every slot transmitting or idle; it has no
   rx_period parameter. This is a deliberate asymmetry and it does mean MPRM
   gets more transmit opportunities than CHARM at the same rx_period, so an
   MPRM/CHARM gap mixes the information effect with that airtime difference.

   Keep in sync with schedulers/mpedf/, which is this policy over an EDF queue
   and differs only in the min_element comparator -- the same arrangement, and
   the same hazard, as CHARM/CHEDF. */
class MPRM_scheduler : public BaseScheduler
{
public:
    MPRM_scheduler(unsigned int tx_power, unsigned int frequency, double mean_prob, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;
    unsigned int frequency;

    /* Static long-run decode probability at tx_power. Fixed for the whole
       run; this is the one input that distinguishes MPRM from CHARM. */
    double mean_prob;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Ignores predictions, and asks for none. The empty power list is the base
       class default, which is what stops main.cpp routing a prediction here;
       the override below is explicit so the intent is not read as an
       oversight. */
    void receive_prediction(const std::vector<double>& pred_probs) override {}

    std::string get_name() const override;
};
