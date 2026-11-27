#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include "bcs_scheduler.hpp"
#include "schedulers/policies/channel_choice.hpp"
#include "schedulers/policies/edf_queue.hpp"
#include "schedulers/policies/feasibility.hpp"
#include "schedulers/policies/power_tier.hpp"

BCS_scheduler::BCS_scheduler(unsigned int tx_power, float belief_threshold,
                             std::size_t n_usable, bool early_drop,
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
    /* Which channels are in play is ranked by long-run mean at the highest
       power this scheduler could ever transmit at. With power fixed that is
       the fixed power; with power adaptive it is the top tier rather than the
       cap, because the cap moves during the run and a set that changed with it
       would make `n_usable` mean something different from slot to slot. */
    const unsigned int rank_power = this->adaptive_power
                                    ? POWER_TIERS[TOP_POWER_TIER]
                                    : this->tx_power;

    this->usable_channels = rank_channels_by_mean(this->get_channels(), rank_power);

    /* 0 means "work with all of them", and so does a count at or past the
       number of channels there are -- asking for the best 8 of 4 is the whole
       set, not an error. Either way the list stays in ranked order, which is
       what makes the tie rule below prefer the better channel. */
    if (n_usable > 0U && n_usable < this->usable_channels.size())
        this->usable_channels.resize(n_usable);
};

int BCS_scheduler::stalest_channel(void) const
{
    int   worst  = -1;
    float lowest = 0.0f;

    for (std::size_t c : this->usable_channels)
    {
        const float b = this->belief.of(c);
        if (b >= this->belief_threshold) continue;

        if (worst < 0 || b < lowest)
        {
            worst  = static_cast<int>(c);
            lowest = b;
        }
    }

    /* Ties go to whichever comes first, which is the best-mean one: every
       belief starts at 0, so the opening slots probe the usable channels in
       rank order, best first. */
    return worst;
}

std::size_t BCS_scheduler::best_channel(unsigned int power) const
{
    std::size_t best     = this->usable_channels.front();
    double      best_rho = this->rho_hat(best, power);

    for (std::size_t c : this->usable_channels)
    {
        const double rho = this->rho_hat(c, power);

        /* Exact equality is the right test, not an epsilon: the tie worth
           breaking is two channels believed to be in states with the same
           fitted curve, which produces the same double bit for bit. A channel
           that merely happens to be close is a different channel and the
           higher estimate should win. */
        if (rho > best_rho)
        {
            best     = c;
            best_rho = rho;
        }
        else if (rho == best_rho && this->belief.of(c) > this->belief.of(best))
        {
            best = c;
        }
    }

    return best;
}

double BCS_scheduler::best_prob_at(unsigned int j) const
{
    double best = 0.0;

    for (std::size_t c : this->usable_channels)
        best = std::max(best, this->rho_hat(c, POWER_TIERS[j]));

    return best;
}

void BCS_scheduler::update_power_cap(void)
{
    /* With power fixed there is no cap: max_power_idx is never read, and
       moving it would let the slack policy back into a scheduler that was
       asked not to run it. */
    if (!this->adaptive_power) return;

    this->max_power_idx =
        slack_power_cap(this->periodic_tasks, this->horizon_H,
                        this->utilization_threshold,
                        [this](unsigned int j) { return this->best_prob_at(j); });
}

scheduled_frame_t BCS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;

    /* With power fixed, every slot carries the one power. With power adaptive
       nothing is committed until the transmit branch picks a tier, so the
       field starts at zero rather than at whatever was on the stack. */
    scheduled_frame.transmission_power = this->adaptive_power ? 0U : this->tx_power;

    /* One slot of belief decay, for every channel -- including the ones this
       slot will not touch, which is the whole point of keeping estimates of
       channels the radio is not on. */
    this->belief.age(this->get_channels());

    /* Every decision below is taken at this power, so the channel comparison,
       the queue costing and the transmission cannot disagree about what is
       being spent. */
    const unsigned int power = this->ref_power();

    /* The channel this slot would transmit on, decided before the queue is
       read so that the estimate the queue is costed against is the one a
       transmission would actually use. It is a pure function of the beliefs
       and the estimates, so asking for it here cannot change what the rules
       below decide. */
    const std::size_t chosen            = this->best_channel(power);
    const double      transmission_prob = this->rho_hat(chosen, power);

    const feasibility_t feasibility =
        classify_feasibility(*this->buffer_packet, *(this->system_tick),
                             transmission_prob);

    /* Classify always, remove only when asked: the switch governs what leaves
       the buffer and never which packets the classification picks out. */
    if (this->early_drop)
    {
        for (const auto& [id, id_count] : feasibility.infeasible)
        {
            this->accumulated_prob.erase(packet_key(id, id_count));
            this->buffer->drop_packet(id, id_count);
        }
    }

    const int stale = this->stalest_channel();

    if (stale >= 0)
    {
        /* Refresh before transmitting. Nothing is assigned to belief here: the
           ageing above and receive_state() are the only two things that move
           it, so a listen that comes back with no usable label buys nothing
           and this channel is still the stalest next slot. */
        scheduled_frame.channel    = static_cast<std::size_t>(stale);
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet     = nullptr;

        return scheduled_frame;
    }

    /* The channel and its rho_hat_{c,j}(t) were decided above, and the label
       they are read off is always one this scheduler still believes: the
       refresh above is unconditional, so every belief is above the threshold
       by the time this line runs. A never-observed channel sits at belief 0
       and so cannot be reached here -- it was probed first. */
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
        unsigned int tx = power;

        if (this->adaptive_power)
        {
            const int tier =
                select_power_tier([this, chosen](unsigned int j)
                                  { return this->rho_hat(chosen, POWER_TIERS[j]); },
                                  0U, this->max_power_idx, acc, req);

            tx = POWER_TIERS[tier >= 0 ? static_cast<unsigned int>(tier)
                                       : this->max_power_idx];
        }

        const double p_sel     = this->rho_hat(chosen, tx);
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
        /* Nothing to send. The slot would otherwise be idle, so spend it
           topping up the weakest estimate even though it is above the
           threshold -- a free refresh that makes the next transmitting slot
           better informed. Only among the ones it works with: probing a
           channel it will never transmit on buys nothing. */
        std::size_t weakest = this->usable_channels.front();
        for (std::size_t c : this->usable_channels)
            if (this->belief.of(c) < this->belief.of(weakest)) weakest = c;

        scheduled_frame.channel    = weakest;
        scheduled_frame.packet     = nullptr;
        scheduled_frame.radio_mode = RX_MODE;
    }

    return scheduled_frame;
}

std::string BCS_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs, and so is the count of usable
       channels: the narrowed variants are this same class, and would
       otherwise all write to the one file. The full set keeps the plain name.

       A name encodes a *deviation from this scheduler's defaults*, which is
       why a switched-on queue policy is spelled out here while a scheduler
       that runs that policy by default spells out a switched-off one instead.
       Either way the plain name belongs to the plain scheduler and a variant
       cannot overwrite its log. */
    /* An adaptive-power instance has no one power to name, so it is named for
       the policy instead -- the same spelling CATS uses for the same switch. */
    const std::string power = this->adaptive_power
                              ? "adaptP"
                              : (std::to_string(this->tx_power) + "W");

    std::string name = this->usable_channels.size() == this->n_channels()
                       ? "BCS_" + power
                       : "TCS" + std::to_string(this->usable_channels.size())
                         + "_" + power;

    if (this->early_drop) name += "_drop";

    return name;
}
