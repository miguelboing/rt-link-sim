#pragma once

#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"

/* CHEDF -- CHARM's channel-aware retransmission/power policy over an EDF
   queue.

   Listening is belief-driven, exactly as in CHARM: belief in the last
   prediction is restored by a listen and decays on every slot after it by the
   self-transition probability A_ss of the state it believes the channel is in,
   read off its own copy of the channel's fitted FSMC, and the scheduler
   listens once it drops below belief_threshold. There is no `rx_period` -- see
   schedulers/charm/charm_scheduler.hpp for why the fixed interval was
   replaced and why no urgency guard is applied.

   Standalone by design: it stands on its own as a scheduler rather than as a
   parameterisation of CHARM. The consequence is that the retransmission rule
   below is a deliberate copy of CHARM's, not a shared one -- the CHARM/CHEDF
   comparison is only meaningful while the two differ solely in dequeue order,
   so any change to the accumulated-probability rule or the belief-driven
   listening rule must be applied to both files. */
class CHEDF_scheduler : public BaseScheduler
{
public:
    CHEDF_scheduler(unsigned int tx_power, std::size_t channel, float belief_threshold,
                    BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;

    /* Belief in the label last heard on each channel, and the floor below
       which this scheduler gives up a slot to refresh it. The listening policy
       is this scheduler's, not the base's: the base holds the estimate, this
       holds what is made of it. Shared with CHARM and CATS by composition --
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
