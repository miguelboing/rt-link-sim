#pragma once

#include <memory>

#include "schedulers/base_scheduler.hpp"
#include <unordered_map>

/* CHARM -- accumulated-probability retransmission over a period-ordered
   queue, listening on CATS's belief rule rather than on a fixed interval.

   Listening policy. CHARM as published gives up every rx_period-th slot to
   RX_MODE whatever the channel is doing. That is a fixed-interval stand-in
   for the question actually being asked -- "is the estimate I am holding
   still worth anything?" -- so it is replaced here by the belief test CATS
   uses: confidence decays by BELIEF_DECAY on every slot spent transmitting
   and is restored to 1 by a listen, and the scheduler listens as soon as it
   falls below belief_threshold. Since BELIEF_DECAY is the chain's mixing
   rate, the interval that emerges is the one the channel's own dynamics
   imply, and it is no longer a parameter to tune: `rx_period` is gone and
   `belief_threshold` takes its place.

   Unlike CATS this does *not* carry the `no_urgent_packet` guard -- it
   listens purely on belief, so a listen can consume a slot a near-deadline
   packet needed. That is a deliberate simplification: it keeps the one
   difference from CATS's listening policy at zero, at the cost of CHARM
   being able to miss a deadline that periodic listening would have made.

   Everything else is unchanged: single fixed power, and a packet is retired
   once its accumulated decode probability clears its per-frame requirement.

   Keep in sync with schedulers/chedf/, which is this policy over an EDF
   queue and differs only in the min_element comparator. */
class CHARM_scheduler : public BaseScheduler
{
public:
    CHARM_scheduler(unsigned int tx_power, unsigned int frequency, float belief_threshold, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;
    unsigned int frequency;

    /* Confidence that the last prediction still describes the channel: 1
       immediately after a listen, decaying by BELIEF_DECAY each slot spent
       transmitting. Starts at 0 so the first slot is always a listen, which
       is what stops the scheduler transmitting on an unset estimate. */
    float belief;
    float belief_threshold;

    double transmission_prob;
    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    void receive_prediction(const std::vector<double>& pred_probs) override;
    /* CHARM transmits at a single fixed power, so it only ever needs the
       decode probability at that power. */
    std::vector<unsigned int> get_prediction_powers() const override { return {this->tx_power}; }

    std::string get_name() const override;
};
