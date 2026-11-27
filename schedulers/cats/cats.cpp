#include <iostream>
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "cats.hpp"
#include "schedulers/policies/edf_queue.hpp"
#include "schedulers/policies/feasibility.hpp"
#include "schedulers/policies/power_tier.hpp"
#include "schedulers/policies/urgency.hpp"


CATS_scheduler::CATS_scheduler(std::size_t channel,
                               float belief_threshold,
                               double utilization_threshold,
                               const std::vector<periodic_task_t>& periodic_tasks,
                               bool adaptive_power,
                               unsigned int fixed_tx_power,
                               bool early_drop,
                               bool urgency_check,
                               BufferPacket* buffer,
                               std::shared_ptr<unsigned int> sys_tick,
                               std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)),
    channel(channel),
    adaptive_power(adaptive_power),
    fixed_power_idx(TOP_POWER_TIER),
    early_drop(early_drop),
    urgency_check(urgency_check),
    belief_threshold(belief_threshold),
    periodic_tasks(periodic_tasks),
    horizon_H(task_hyperperiod(periodic_tasks)),
    utilization_threshold(utilization_threshold),
    max_power_idx(TOP_POWER_TIER)
{
    /* Fixed-power mode has to name a tier the predictor actually reports, or
       the probability driving its retransmission maths would belong to a
       different power than the one transmitted -- the mismatch that once made
       a 25 W scheduler decide from the 10 W curve. Resolve it once,
       loudly. */
    if (!this->adaptive_power)
    {
        const int idx = power_tier_index(fixed_tx_power);

        if (idx < 0)
            throw std::runtime_error("CATS: fixed tx_power " + std::to_string(fixed_tx_power)
                                     + "W is not one of the predictor tiers ("
                                     + power_tiers_string() + ")");

        this->fixed_power_idx = static_cast<unsigned int>(idx);

        /* No adaptive cap survives: the cap *is* the fixed tier, so p_best,
           demand, infeasibility and urgency all read that tier too. */
        this->max_power_idx = this->fixed_power_idx;
    }
}

scheduled_frame_t CATS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.channel = this->channel;

    /* Nothing transmitted unless the TX branch below picks a tier and says
       otherwise. Set here rather than left to that branch because a slot spent
       listening or idling still carries this field into the per-slot log, and
       an unset value there is whatever was on the stack. */
    scheduled_frame.transmission_power = 0U;

    /* One slot of belief decay, for every channel. CATS's policy, so CATS
       ticks it -- the base holds the estimate and makes no assumption about
       what is made of it. */
    this->belief.age(this->get_channels());

    /* Classify every buffered packet, then act on the classification. The two
       are kept separate so the early_drop switch governs *removal* only: with
       dropping off, an infeasible packet stays queued but is still classified
       infeasible, so it does not fall through into "urgent" and start
       suppressing listening. Urgency is therefore the same set of packets in
       both settings.

       Per-frame slot cost is the number of retransmissions needed to hit the
       per-frame RR target (pow(RR, 1/frames)) at the best channel probability
       we'll actually allow (prob_at(max_power_idx)) — the cap is a
       hard energy limit, so feasibility past it is moot. In fixed-power mode
       that index is the fixed tier, so the same power drives demand,
       infeasibility and urgency as is transmitted.
       Pre-prediction (p_best == 0) we fall back to 1 slot/frame so packets
       aren't all dropped before the first prediction lands. */
    const double p_best = this->prob_at(this->max_power_idx);

    const feasibility_t feasibility =
        classify_feasibility(*this->buffer_packet, *(this->system_tick), p_best);

    /* Removal is the switchable half. With early_drop off the infeasible list
       is simply discarded and those packets run to deadline expiry (or get
       retired by the reliability rule if the channel improves). */
    if (this->early_drop)
    {
        for (const auto& [id, id_count] : feasibility.infeasible)
        {
            accumulated_prob.erase(packet_key(id, id_count));
            this->buffer->drop_packet(id, id_count);
        }
    }

    /* Listen when the estimate is no longer trusted *and* the queue can spare
       the slot. Two separate questions, asked in that order: belief decides
       whether a refresh is wanted, urgency whether it is affordable.

       Belief decays at the believed state's own A_ss -- per channel and
       per state, not one tuned constant for all of them, and every channel
       ages whether the radio is using it or not. Nothing is assigned on either
       side of this test: the ageing above and receive_state() below are the
       only two things that move it. */
    if (this->belief.stale(this->channel, this->belief_threshold)
        && listening_affordable(feasibility, this->urgency_check))
    {
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet = nullptr;
    }
    else /* If it isn't, try to schedule a packet */
    {
        packet_t* next = select_earliest_deadline(*this->buffer_packet);

        if (next != nullptr)
        {
            uint64_t key     = packet_key(next->id, next->id_count);
            double req       = std::pow(next->reliability_req,
                                        1.0 / next->frames);

            /* Get or initialise accumulated probability for this packet instance */
            double acc = (accumulated_prob.find(key) != accumulated_prob.end())
                         ? accumulated_prob[key] : 0.0;

            /* max_power_idx is set by update_power_cap() on each new
               observation, following the slack policy: pick the lowest tier
               whose predicted U fits below the threshold, then cap the
               selection loop there to save energy. */
            /* Adaptive: walk tiers from the bottom and take the first that
               clears the requirement. Fixed: the loop spans the single fixed
               tier, so that is always the one chosen. */
            const unsigned int lo_idx = this->adaptive_power ? 0U : this->fixed_power_idx;
            const int chosen_idx =
                select_power_tier([this](unsigned int i) { return this->prob_at(i); },
                                  lo_idx, this->max_power_idx, acc, req);

            if (chosen_idx >= 0)
            {
                /* Accumulated probability meets requirement — remove from buffer */
                const double p_sel = this->prob_at(static_cast<unsigned int>(chosen_idx));
                accumulated_prob[key] = acc + p_sel - acc * p_sel;
                scheduled_frame.transmission_power = POWER_TIERS[chosen_idx];
                scheduled_frame.remove_from_buffer = true;
                accumulated_prob.erase(key);
            }
            else
            {
                /* No allowed tier clears the requirement yet — burn the cap
                   tier and keep retransmitting on the next slot. */
                const double p_cap = this->prob_at(this->max_power_idx);
                accumulated_prob[key] = acc + p_cap - acc * p_cap;
                scheduled_frame.transmission_power = POWER_TIERS[this->max_power_idx];
                scheduled_frame.remove_from_buffer = false;
            }

            scheduled_frame.packet     = next;
            scheduled_frame.radio_mode = TX_MODE;
        }
        else
        {
            /* Nothing queued, so spend the slot listening. The refresh needs
               no statement here: the slot is an RX slot, so the observation it
               returns moves tau_c and belief follows from that. */
            scheduled_frame.packet = nullptr;
            scheduled_frame.radio_mode = RX_MODE;
        }
    }

    return scheduled_frame;
}

void CATS_scheduler::receive_state(std::size_t c, int state)
{
    /* The predictor returns one label for the channel the slot was spent on.
       Recording it is the base's job; restoring belief in it is CATS's,
       and so is re-running the slack policy, whose tiers all read off the
       label that just moved. Nothing else is derived here, since prob_at()
       reads the whole row on demand -- which is what makes all three tiers
       share one state and one age.

       A label of -1 records nothing, so it refreshes no belief and leaves
       the cap where it was. */
    if (!BaseScheduler::observe_state(c, state)) return;

    this->belief.observed(c);
    this->update_power_cap();
}

void CATS_scheduler::update_power_cap(void)
{
    /* Fixed-power mode keeps no adaptive cap at all: max_power_idx was pinned
       to the fixed tier at construction and must not move, or the slack policy
       would creep back into demand/infeasibility/urgency. */
    if (!this->adaptive_power) return;

    /* The tiers are read off the label that just moved, on the one channel
       this scheduler transmits on. */
    this->max_power_idx =
        slack_power_cap(this->periodic_tasks, this->horizon_H,
                        this->utilization_threshold,
                        [this](unsigned int i) { return this->prob_at(i); });
}

std::string CATS_scheduler::get_name() const
{
    /* Unchanged for the published configuration, so existing logs and any
       tooling that looks for them keep working. Each ablation gets its own
       name instead, or the variants would overwrite one another's
       *_scheduled_packets.json the way two same-named runs would. */
    if (this->adaptive_power && this->early_drop && this->urgency_check)
        return "CATS Scheduler";

    std::string name = "CATS";
    name += this->adaptive_power
            ? "_adaptP"
            : ("_" + std::to_string(POWER_TIERS[this->fixed_power_idx]) + "W");
    if (!this->early_drop)    name += "_nodrop";
    if (!this->urgency_check) name += "_nourg";
    return name;
}

