#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "lcs_scheduler.hpp"
#include "schedulers/policies/channel_choice.hpp"
#include "schedulers/policies/edf_queue.hpp"
#include "schedulers/policies/feasibility.hpp"
#include "schedulers/policies/power_tier.hpp"

LCS_scheduler::LCS_scheduler(unsigned int tx_power, float belief_threshold,
                             bool early_drop,
                             bool adaptive_power, double utilization_threshold,
                             const std::vector<periodic_task_t>& periodic_tasks,
                             BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                             std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)), tx_power(tx_power),
    early_drop(early_drop), adaptive_power(adaptive_power),
    utilization_threshold(utilization_threshold),
    periodic_tasks(periodic_tasks),
    horizon_H(task_hyperperiod(periodic_tasks)),
    max_power_idx(TOP_POWER_TIER),
    belief_threshold(belief_threshold)
{
    /* The rungs are ordered by long-run mean at the highest power this
       scheduler could ever transmit at. With power fixed that is the fixed
       power; with power adaptive it is the top tier rather than the cap,
       because the cap moves during the run and a ladder reshuffling under the
       seat is not a ladder -- "the rung below" has to mean the same channel
       from one slot to the next. */
    const unsigned int rank_power = this->adaptive_power
                                    ? POWER_TIERS[TOP_POWER_TIER]
                                    : this->tx_power;

    this->ladder = rank_channels_by_mean(this->get_channels(), rank_power);
};

int LCS_scheduler::rung_to_refresh(void) const
{
    /* Highest rung first, so the channel a climb would move to is the one
       whose estimate gets rebuilt first. Rungs below the seat are deliberately
       not on the watch list: letting them go stale is what keeps the upkeep
       proportional to how far down the ladder the radio has walked rather than
       to how many channels exist. */
    for (std::size_t r = 0; r <= this->seat; ++r)
        if (!this->trusted(this->ladder[r])) return static_cast<int>(r);

    return -1;
}

std::size_t LCS_scheduler::best_seat(unsigned int power) const
{
    std::size_t best     = 0U;
    double      best_rho = this->rho_hat(this->ladder[0], power);

    for (std::size_t r = 1; r < this->ladder.size(); ++r)
    {
        /* A rung below the seat is only a candidate while its estimate is
           still believed. Without that test a rung the radio stopped watching
           would keep competing for the seat on a label from arbitrarily long
           ago, which is the one thing belief exists to stop. */
        if (r > this->seat && !this->trusted(this->ladder[r])) continue;

        const double rho = this->rho_hat(this->ladder[r], power);

        /* Strictly greater, so a tie keeps the higher rung: equal estimates
           mean equal information about this slot, and the better long-run mean
           is the better bet for the slots after it. */
        if (rho > best_rho)
        {
            best     = r;
            best_rho = rho;
        }
    }

    return best;
}

double LCS_scheduler::bar_to_beat(std::size_t c, unsigned int power) const
{
    const double mean = this->get_channels()[c].model.rho_bar(power);

    /* The better of the two readings of the same channel: what it averages,
       and what it was last heard doing. The second only counts while it is
       still believed -- past the threshold the label is treated as unknown,
       and an unknown channel is described by its mean.

       With the climb in best_seat() in place this max cannot currently change
       the outcome: a believed estimate higher than the seat's would already
       have taken the seat, so the only term that can exceed the seat's
       estimate here is the mean. It is written as the max anyway because that
       is the rule, and because it stays correct if the climb is ever
       restricted to the watch list. */
    if (!this->trusted(c)) return mean;

    return std::max(mean, this->rho_hat(c, power));
}

void LCS_scheduler::update_power_cap(void)
{
    /* With power fixed there is no cap: max_power_idx is never read, and
       moving it would let the slack policy back into a scheduler that was
       asked not to run it. */
    if (!this->adaptive_power) return;

    /* Asked of the seat, which is where a transmission this slot would go.
       The rungs above may well be better, but reaching one costs a listening
       slot first, so a tier that only fits because of a channel the radio is
       not on would be a budget this scheduler cannot spend yet. The seat is
       read as the slot opens, before the climb, for the same reason the queue
       is. */
    this->max_power_idx =
        slack_power_cap(this->periodic_tasks, this->horizon_H,
                        this->utilization_threshold,
                        [this](unsigned int j)
                        { return this->rho_hat(this->ladder[this->seat],
                                               POWER_TIERS[j]); });
}

scheduled_frame_t LCS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;

    /* With power fixed, every slot carries the one power. With power adaptive
       nothing is committed until the transmit branch picks a tier, so the
       field starts at zero rather than at whatever was on the stack. */
    scheduled_frame.transmission_power = this->adaptive_power ? 0U : this->tx_power;

    /* One slot of belief decay, for every channel -- including the rungs this
       scheduler is not watching, since the chains they model keep mixing
       whether anything is listening or not. */
    this->belief.age(this->get_channels());

    /* Re-run the slack policy before anything reads the cap, and from the seat
       as the slot opens, so the cap and the queue are costed against the same
       rung.

       Once per slot, not once per observation as on a scheduler whose cap
       depends on the labels alone: the seat is the other input and it moves
       without any label arriving, so a cap refreshed only on an observation
       would be a cap derived from a rung this scheduler has since left. */
    this->update_power_cap();

    /* Every decision below is taken at this power, so the ladder, the queue
       costing and the transmission cannot disagree about what is being
       spent. */
    const unsigned int power = this->ref_power();

    /* Judge the queue at the decode estimate of the rung the radio is on as
       the slot opens -- the channel a transmission this slot would most likely
       go out on, and the one every packet still queued was last costed
       against. The seat may yet climb below, which makes this an estimate one
       slot behind on the slots a packet needs; that is the same lag any
       estimate-driven count carries. */
    const double p_seat = this->rho_hat(this->ladder[this->seat], power);

    const feasibility_t feasibility =
        classify_feasibility(*this->buffer_packet, *(this->system_tick), p_seat);

    /* Only the removal is switchable. The list above is built in both
       settings, so the switch governs what leaves the buffer and nothing
       about how the queue was read. */
    if (this->early_drop)
    {
        for (const auto& [id, id_count] : feasibility.infeasible)
        {
            this->accumulated_prob.erase(packet_key(id, id_count));
            this->buffer->drop_packet(id, id_count);
        }
    }

    const int due = this->rung_to_refresh();

    if (due >= 0)
    {
        /* Rebuild the watch list before using it. Nothing is assigned to
           belief here: ageing and receive_state() are the only two things that
           move it, so a listen that comes back with no usable label leaves
           this rung due next slot. */
        scheduled_frame.channel    = this->ladder[static_cast<std::size_t>(due)];
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet     = nullptr;

        return scheduled_frame;
    }

    /* Climb, or stay. Either way the seat is now the best rung the believed
       estimates support. */
    this->seat = this->best_seat(power);

    const std::size_t chosen = this->ladder[this->seat];
    const double      rho    = this->rho_hat(chosen, power);
    const double      mean   = this->get_channels()[chosen].model.rho_bar(power);

    if (rho < mean)
    {
        /* Below what this channel manages on average, so the rungs underneath
           are worth considering. Walk down from the one immediately below: the
           first rung worth leaving for that this scheduler cannot currently
           hear is the one to spend the slot listening to.

           A rung that is already fresh is skipped rather than probed. Its
           estimate took part in the seat decision above, so listening to it
           again cannot change anything this slot, and spending a slot to
           re-hear a channel that has already lost the seat is how this would
           turn into a loop. */
        for (std::size_t r = this->seat + 1U; r < this->ladder.size(); ++r)
        {
            const std::size_t candidate = this->ladder[r];

            if (rho >= this->bar_to_beat(candidate, power)) break;

            if (this->trusted(candidate)) continue;

            scheduled_frame.channel    = candidate;
            scheduled_frame.radio_mode = RX_MODE;
            scheduled_frame.packet     = nullptr;

            return scheduled_frame;
        }
    }

    scheduled_frame.channel = chosen;

    packet_t* next = select_earliest_deadline(*this->buffer_packet);

    if (next != nullptr)
    {
        scheduled_frame.packet     = next;
        scheduled_frame.radio_mode = TX_MODE;

        const uint64_t key = packet_key(next->id, next->id_count);
        const double   req = std::pow(next->reliability_req, 1.0 / next->frames);

        /* What earlier copies of this frame already bought. Absent means this
           is the first attempt, which is a zero accumulation rather than a
           special case. */
        const auto   it  = this->accumulated_prob.find(key);
        const double acc = (it != this->accumulated_prob.end()) ? it->second : 0.0;

        /* The power this frame goes out at. Fixed power is the one-tier case
           of the same rule -- the single permitted power either finishes the
           frame or is spent anyway and the frame comes back -- so both
           settings take the same path from here down and only the tier choice
           is switched. */
        unsigned int tx    = power;
        double       p_sel = rho;

        if (this->adaptive_power)
        {
            const int tier =
                select_power_tier([this, chosen](unsigned int j)
                                  { return this->rho_hat(chosen, POWER_TIERS[j]); },
                                  0U, this->max_power_idx, acc, req);

            tx    = POWER_TIERS[tier >= 0 ? static_cast<unsigned int>(tier)
                                          : this->max_power_idx];
            p_sel = this->rho_hat(chosen, tx);
        }

        const double acc_after = acc + p_sel - acc * p_sel;

        scheduled_frame.transmission_power = tx;

        /* Enough copies are now in the air to clear the per-frame
           requirement, so the frame leaves the buffer and its accumulation is
           forgotten with it. */
        if (acc_after >= req)
        {
            scheduled_frame.remove_from_buffer = true;
            this->accumulated_prob.erase(key);
        }
        else
        {
            scheduled_frame.remove_from_buffer = false;
            this->accumulated_prob[key]        = acc_after;
        }
    }
    else
    {
        /* Nothing to send. The slot would be idle, so spend it on the weakest
           estimate in the watch list even though all of them are above the
           threshold -- a free refresh that makes the next transmitting slot
           better informed. Only the watch list: topping up a rung the radio
           would not move to buys nothing it can act on. */
        std::size_t weakest = this->ladder[0];
        for (std::size_t r = 0; r <= this->seat; ++r)
            if (this->belief.of(this->ladder[r]) < this->belief.of(weakest))
                weakest = this->ladder[r];

        scheduled_frame.channel    = weakest;
        scheduled_frame.packet     = nullptr;
        scheduled_frame.radio_mode = RX_MODE;
    }

    return scheduled_frame;
}

std::string LCS_scheduler::get_name() const
{
    /* Power is part of the name so runs at different tx_power do not overwrite
       each other's scheduled-packet logs, and so is a switched-off policy, so
       an ablation arm does not overwrite the full scheduler's. An
       adaptive-power instance has no one power to name, so it is named for the
       policy instead -- the same spelling CATS uses for the same switch. */
    std::string name = "LCS_" + (this->adaptive_power
                                 ? std::string("adaptP")
                                 : (std::to_string(this->tx_power) + "W"));

    if (!this->early_drop) name += "_nodrop";

    return name;
}
