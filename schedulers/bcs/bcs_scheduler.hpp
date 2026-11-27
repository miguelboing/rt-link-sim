#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

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

   One queue policy sits on top of that, **off unless a config asks for it**,
   which is the opposite default from the other schedulers that carry it:

     - *early_drop* removes a packet whose remaining slots can no longer cover
       its reliability target, handing those slots to the packets that can
       still make it.

   It is off by default because this scheduler's plain form is the
   channel-re-selection baseline -- what it is measured against differs from it
   in *which channel each slot is spent on* and nothing else, and a queue
   policy switched on underneath would quietly become part of that gap.
   Switching it on produces a variant, and the variant says so in its name.

   Listening is unconditional: a channel whose estimate has decayed past the
   threshold gets the slot however close a queued packet is to its deadline.
   So the channel decision never reads the queue, which is what keeps a gap
   measured against this scheduler attributable to where its slots went.

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
                  std::size_t n_usable, bool early_drop,
                  BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                  std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    /* When true, packets that can no longer make their deadline are removed.
       They are classified either way, so this governs the removal only. */
    bool early_drop;

    /* Belief in the label last heard on each channel, and the floor below
       which this scheduler gives up a slot to refresh one. */
    BeliefTracker belief;
    float belief_threshold;

    /* The channels this scheduler is allowed to use, best long-run mean first.
       Fixed at construction from the fitted models, so *which* channels are in
       play is a static decision even though which one is used each slot is
       not.

       The size of this set is the knob the roster's variants differ on, and
       narrowing it is an optimisation rather than a handicap. Every channel in
       the set is an estimate to keep fresh, and every refresh is a slot spent
       probing instead of transmitting; there is only ever one queue to drain,
       so that bill grows with the size of the set while the work does not.
       Keeping only the best few hands those slots back to the queue, and what
       it gives up is the chance that a channel left out would have been the
       good one at that moment. The variants are the same class running the
       same rule over a different set, so a gap between them is that trade and
       nothing else. */
    std::vector<std::size_t> usable_channels;

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
