#pragma once

#include <memory>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"
#include <unordered_map>

/* CHARM -- accumulated-probability retransmission over a period-ordered
   queue, listening on CATS's belief rule rather than on a fixed interval.

   Listening policy. CHARM as published gives up every rx_period-th slot to
   RX_MODE whatever the channel is doing. That is a fixed-interval stand-in
   for the question actually being asked -- "is the estimate I am holding
   still worth anything?" -- so it is replaced here by the belief test CATS
   uses: belief is restored to 1 by a listen and decays on every slot after
   that, and the scheduler listens as soon as it falls below
   belief_threshold. The interval that emerges is the one the channel's own
   dynamics imply, so it is no longer a parameter to tune: `rx_period` is gone
   and `belief_threshold` takes its place.

   What it decays by is the scheduler's own model. Holding a copy of the
   channel's fitted FSMC, it knows the believed state's self-transition
   probability A_ss, so belief d slots after the observation is A_ss^d -- the
   model probability the channel has not left that state, with 1 - A_ss the
   per-slot probability of leaving. That is per channel *and* per state: the
   same scheduler refreshes after 18 slots in a state the chain holds for 50
   and after 178 in one it holds for 500.

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
    CHARM_scheduler(unsigned int tx_power, std::size_t channel, float belief_threshold,
                    BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;

    /* Belief in the label last heard on each channel, and the floor below
       which this scheduler gives up a slot to refresh it. The listening policy
       is this scheduler's, not the base's: the base holds the estimate, this
       holds what is made of it. Shared with CHEDF and CATS by composition --
       see schedulers/policies/belief_tracker.hpp for why it is a held object
       rather than something the base provides. */
    BeliefTracker belief;
    float belief_threshold;
    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Record the observation (the base's job), then restore belief in it (this
       scheduler's). Only a label that was actually recorded counts as a
       refresh, so a listen that came back with nothing leaves belief where it
       was and the scheduler listens again.

       Nothing else happens here. The decode estimate is *not* cached from the
       label: rho_hat() derives it on demand, which is what lets this scheduler
       ask about a channel it is not transmitting on. */
    void receive_state(std::size_t c, int state) override
    {
        if (BaseScheduler::observe_state(c, state)) this->belief.observed(c);
    }

    std::string get_name() const override;
};
