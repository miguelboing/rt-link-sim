#pragma once

#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"

/* CHARM -- accumulated-probability retransmission over a period-ordered queue,
   listening on the belief rule rather than on a fixed interval.

   The published algorithm, with one substitution. CHARM as published gives up
   every rx_period-th slot to RX_MODE whatever the channel is doing; that is a
   fixed-interval stand-in for the question actually being asked -- is the
   estimate still worth anything -- so it is replaced here by the belief test:
   belief is restored by a listen and decays on every slot after it, and the
   scheduler listens once it falls below belief_threshold. The interval that
   emerges is the one the channel's own dynamics imply, so `rx_period` is gone
   and `belief_threshold` takes its place.

   A frame is repeated until the probability that at least one of its copies
   was decoded clears its per-frame requirement, sized from the current decode
   estimate. Power is fixed, and the channel is chosen once from the fitted
   long-run means.

   It exists as the period-ordered control: against the same policy over a
   deadline-ordered queue, at the same power and the same listening rule, the
   only difference left is the order the queue is served in. */
class CHARM_scheduler : public BaseScheduler
{
public:
    CHARM_scheduler(unsigned int tx_power, float belief_threshold,
                    BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                    std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    /* c: the channel this scheduler commits to, chosen at construction from
       the fitted long-run means and never revisited. */
    std::size_t channel;

    /* Belief in the label last heard on each channel, and the floor below
       which this scheduler gives up a slot to refresh it. */
    BeliefTracker belief;
    float belief_threshold;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Record the observation, then restore belief in it. Only a label that was
       actually recorded counts as a refresh, so a listen that came back with
       nothing leaves belief where it was and the scheduler listens again. */
    void receive_state(std::size_t c, int state) override
    {
        if (BaseScheduler::observe_state(c, state)) this->belief.observed(c);
    }

    std::string get_name() const override;
};
