#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "packet_generators/base_packet_generator.hpp"
#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"
#include "schedulers/policies/power_tier.hpp"

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
   decode estimate.

   Power is fixed unless *adaptive_power* is asked for, in which case it is
   CATS's two-part rule (`policies/power_tier.hpp`) over this scheduler's own
   channel set:

     - a *cap*, the lowest tier whose predicted utilization of the known task
       set fits under utilization_threshold. The probability a tier offers is
       the best any usable channel offers at it, because this scheduler is free
       to transmit wherever it likes each slot -- so the question "does the work
       fit at this tier" is asked of the best channel the tier can reach, not
       of one fixed channel.
     - a *per-frame choice*, the cheapest tier at or below the cap whose decode
       estimate on the chosen channel finishes the frame; if none does, the cap
       tier is spent and the frame comes back next slot.

   The cap is also the power the channel comparison and the queue costing are
   done at, since it is the most this scheduler will spend: ranking channels at
   a power it would never use would pick the channel that is best under a
   budget it does not have.

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
                  bool adaptive_power, double utilization_threshold,
                  const std::vector<periodic_task_t>& periodic_tasks,
                  BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                  std::vector<channel_estimate_t> channels);

    /* The one power every transmission goes out at while adaptive_power is
       off. Not required to be a predictor tier -- the decode estimate is read
       off the fitted curve at whatever power this is, so the scheduler still
       sizes its retransmissions from the power it transmits at. */
    unsigned int tx_power;

    /* When true, packets that can no longer make their deadline are removed.
       They are classified either way, so this governs the removal only. */
    bool early_drop;

    /* When true, the power of each transmission is chosen per frame from the
       predictor tiers, under a cap set by the task set's predicted
       utilization. When false, every transmission is tx_power and no cap
       exists. */
    bool adaptive_power;

    /* The utilization the cap policy keeps the schedule under, and the
       a-priori task set and horizon it measures that against. All three are
       read only while adaptive_power is on. */
    double utilization_threshold;
    std::vector<periodic_task_t> periodic_tasks;
    unsigned int horizon_H;

    /* The highest tier currently permitted, as an index into POWER_TIERS.
       Starts at the top, so behaviour before the first observation is the
       scheduler's most capable rather than its cheapest. */
    unsigned int max_power_idx;

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
       nothing leaves belief where it was and the channel stays due.

       The cap is re-run here and nowhere else, because this is the only thing
       that moves what it is derived from: `rho_hat` is read off the last
       label, so it changes when a label arrives and not as belief decays. */
    void receive_state(std::size_t c, int state) override
    {
        if (!BaseScheduler::observe_state(c, state)) return;

        this->belief.observed(c);
        this->update_power_cap();
    }

    std::string get_name() const override;

private:
    /* The channel most in need of a refresh: the lowest belief among those
       below the threshold, or -1 when every estimate is still trusted. */
    int stalest_channel(void) const;

    /* The channel to transmit on: highest decode estimate at `power`, ties
       broken by belief. */
    std::size_t best_channel(unsigned int power) const;

    /* The power every decision this slot is taken at: the cap while power is
       adaptive, the fixed power otherwise. One accessor, so no decision in a
       slot can read a different power than the others. */
    unsigned int ref_power(void) const
    {
        return this->adaptive_power ? POWER_TIERS[this->max_power_idx]
                                    : this->tx_power;
    }

    /* The best decode estimate any usable channel offers at tier j -- what
       this scheduler could achieve by spending that tier, since it is free to
       transmit on any of them. The input the cap policy is asked for. */
    double best_prob_at(unsigned int j) const;

    /* Re-run the slack policy. No-op while power is fixed, where there is no
       cap to move. */
    void update_power_cap(void);
};
