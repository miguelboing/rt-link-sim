#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"

/* LCS -- "Ladder Channel Selection".

   The channels are put in a fixed ladder, best long-run mean at the top, and
   the radio sits on one rung at a time. It holds its seat while the channel
   under it is doing well enough, and otherwise walks down the ladder one rung
   at a time, listening before it commits to a worse channel. From any rung it
   keeps watching the rungs above, and climbs back the moment one of them is
   heard to be better.

   What makes it a ladder rather than a search over all channels: the only
   estimates it pays to keep fresh are the seat's and the ones above it. A
   scheduler that keeps every channel fresh spends a listening slot per
   channel per refresh interval whatever it is doing; this one spends that only
   on the channels it would actually move to, so sitting at the top of a
   healthy ladder costs exactly one channel's upkeep.

   The rule, in order, once per slot:

     0. *Classify the queue, and drop what is already lost.* Every queued
        packet is measured against the slots it has left at the decode
        estimate of the channel the radio is sitting on. A packet that can no
        longer be delivered in time is removed while early_drop is on, which
        hands the slots it would have taken back to the packets that can still
        make it. A packet with exactly enough slots left makes the queue
        urgent, which is what the two listening steps below have to get past.
     1. *Keep the watch list fresh, if the queue can spare the slot.* The watch
        list is the seat and every rung above it. If any of them has decayed
        below belief_threshold, the slot becomes an RX slot on the highest such
        rung -- highest rather than stalest, because a better channel is the
        one worth knowing about first and the one a climb would move to.
     2. *Take the best seat the fresh estimates support.* Among the rungs above
        the seat and any rung below it that is still fresh, whichever has the
        highest decode estimate becomes the seat, ties going to the higher
        rung. Climbing back up is this step and nothing else.
     3. *Hold the seat if it is earning it.* A seat whose estimate is at or
        above that channel's own long-run mean is doing as well as that channel
        ever does on average, so the slot is spent transmitting.
     4. *Otherwise look one rung down.* The bar to beat is the better of what
        the rung below averages and what it was last heard doing, counting the
        latter only while that estimate is still believed. A seat that clears
        the bar is kept anyway -- below its own average, but still better than
        the alternative. A seat that does not is given up, again only while the
        queue can spare the slot: it becomes an
        RX slot on the rung below, which is a listen and not yet a move. If
        that rung is already fresh there is nothing to learn by listening to
        it, so the search carries on to the next rung down, and if nothing
        below is worth leaving for the seat transmits.

   Nothing moves to a worse channel without hearing it first. Step 4 only ever
   spends a slot listening; the move itself is step 2 on a later slot, and
   happens only if the fresh estimate justifies it. So a rung that looked
   promising by its average and turns out to be bad costs one listening slot,
   not a run of transmissions on a bad channel.

   Listening is affordable rather than free. Both ways a slot is given up to
   it -- refreshing the watch list and probing the rung below -- are gated on
   the same affordability test, so a slot that some packet needs at exactly
   that moment is spent transmitting on an estimate this scheduler already
   distrusts instead of on a refresh. The empty-queue refresh at the end is not
   gated: with nothing queued there is nothing that can be urgent.

   Both of those halves are switchable, and independently. Classifying the
   queue and removing from it are separate steps, so early_drop off leaves
   hopeless packets queued *and still classified* -- turning dropping off
   cannot quietly change which packets count as urgent. urgency_check off
   removes only the affordability term, leaving the ladder, the belief rule and
   the classification exactly as they were.

   Retransmission is the accumulated-probability rule, sized from the seat's
   current decode estimate. Power is fixed. Queue order is by deadline. */
class LCS_scheduler : public BaseScheduler
{
public:
    LCS_scheduler(unsigned int tx_power, float belief_threshold,
                  bool early_drop, bool urgency_check,
                  BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                  std::vector<channel_estimate_t> channels);

    unsigned int tx_power;

    /* When false, packets that can no longer make their deadline stay in the
       buffer until it expires them. They are classified either way, so this
       governs the removal and nothing else. */
    bool early_drop;

    /* When false, a slot may be given up for listening however close a packet
       is to its deadline. */
    bool urgency_check;

    /* Belief in the label last heard on each channel, and the floor below
       which an estimate stops counting as something this scheduler knows. */
    BeliefTracker belief;
    float belief_threshold;

    /* The ladder: every channel index, best long-run mean first. Fixed at
       construction from the fitted models, so the *order* the rungs are tried
       in is static even though which rung is occupied is not. */
    std::vector<std::size_t> ladder;

    /* Which rung the radio is on, as a position in `ladder` rather than a
       channel index -- the rule is written in terms of "the rung below", so
       the position is the state worth keeping and the channel is read off it.
       Starts at the top. */
    std::size_t seat = 0U;

    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    /* Record the observation, then restore belief in it. Only a label that was
       actually recorded counts as a refresh, so a listen that came back with
       nothing leaves belief where it was and the rung stays due. */
    void receive_state(std::size_t c, int state) override
    {
        if (BaseScheduler::observe_state(c, state)) this->belief.observed(c);
    }

    std::string get_name() const override;

private:
    /* True while the last label heard on this channel is still believed, which
       is what makes its decode estimate something the rule may use. An
       estimate below the threshold is treated as not known rather than as
       pessimistic -- it is the stationary mean that stands in for an unknown
       channel, not a stale label. */
    bool trusted(std::size_t c) const
    {
        return this->belief.of(c) >= this->belief_threshold;
    }

    /* The highest rung at or above the seat whose estimate is no longer
       believed, or -1 when the whole watch list is fresh. */
    int rung_to_refresh(void) const;

    /* The best seat the believed estimates support: the rung with the highest
       decode estimate among the watch list and any fresh rung below it. */
    std::size_t best_seat(void) const;

    /* The value a rung below the seat has to beat for the seat to be given
       up: the better of its long-run mean and its last believed estimate. */
    double bar_to_beat(std::size_t c) const;
};
