#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"

/* BCS -- "Belief-driven Channel Selection".

   Holds an estimate of every channel and spends each slot on whichever one the
   estimate says is most worth spending it on. Three rules, in order:

     1. *Refresh the stalest.* If any channel's belief has decayed below
        belief_threshold, the slot becomes an RX slot on the channel with the
        **lowest** belief of those. Lowest rather than first-in-order so the
        worst-known channel cannot be starved while two others take turns, and
        because a channel that has never been observed sits at belief 0 and so
        is probed before anything is transmitted at all.
     2. *Transmit on the best.* With every estimate still trusted, it sends on
        the channel whose believed state gives the highest decode probability
        at its transmit power.
     3. *Break ties on belief.* Two channels can give exactly the same decode
        estimate -- same curve, same believed state -- and then the better bet
        is the one more recently confirmed, so the higher belief wins.

   Retransmission is the published accumulated-probability rule: a frame is
   repeated until the probability that at least one of its copies was decoded
   clears its per-frame requirement, sized from the chosen channel's current
   decode estimate. Power is fixed.

   Belief is per channel and per state: it is 1 the slot after that channel was
   observed and decays by that state's A_ss on every slot since, so a channel
   the radio is not using ages exactly as fast as the chain it models mixes.
   The refresh interval is therefore set by the channels rather than by a
   tuned clock, and differs between two states of the same chain.

   What it costs: every refresh is a slot not transmitting, and with C channels
   to keep fresh the listening bill grows with C while the queue does not. The
   gain is that the slots it does transmit are spent on the best channel it
   knows of rather than on a channel fixed in advance. Those two are what a
   comparison against a scheduler that never re-chooses is measuring. */
class BCS_scheduler : public BaseScheduler
{
public:
    BCS_scheduler(unsigned int tx_power, float belief_threshold,
                  BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                  std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    /* Belief in the label last heard on each channel, and the floor below
       which this scheduler gives up a slot to refresh one. */
    BeliefTracker belief;
    float belief_threshold;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Record the observation, then restore belief in it. Only a label that was
       actually recorded counts as a refresh, so a listen that came back with
       nothing leaves belief where it was and the channel stays due. */
    void receive_state(std::size_t c, int state) override
    {
        if (BaseScheduler::observe_state(c, state)) this->belief.observed(c);
    }

    std::string get_name() const override;

private:
    /* The channel most in need of a refresh: the lowest belief among those
       below the threshold, or -1 when every estimate is still trusted. */
    int stalest_channel(void) const;

    /* The channel to transmit on: highest decode estimate at tx_power, ties
       broken by belief. */
    std::size_t best_channel(void) const;
};
