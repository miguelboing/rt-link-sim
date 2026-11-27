#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "lcs_scheduler.hpp"
#include "schedulers/policies/channel_choice.hpp"
#include "schedulers/policies/edf_queue.hpp"
#include "schedulers/policies/feasibility.hpp"

LCS_scheduler::LCS_scheduler(unsigned int tx_power, float belief_threshold,
                             bool early_drop,
                             BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                             std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)), tx_power(tx_power),
    early_drop(early_drop), belief_threshold(belief_threshold)
{
    this->ladder = rank_channels_by_mean(this->get_channels(), this->tx_power);
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

std::size_t LCS_scheduler::best_seat(void) const
{
    std::size_t best     = 0U;
    double      best_rho = this->rho_hat(this->ladder[0], this->tx_power);

    for (std::size_t r = 1; r < this->ladder.size(); ++r)
    {
        /* A rung below the seat is only a candidate while its estimate is
           still believed. Without that test a rung the radio stopped watching
           would keep competing for the seat on a label from arbitrarily long
           ago, which is the one thing belief exists to stop. */
        if (r > this->seat && !this->trusted(this->ladder[r])) continue;

        const double rho = this->rho_hat(this->ladder[r], this->tx_power);

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

double LCS_scheduler::bar_to_beat(std::size_t c) const
{
    const double mean = this->get_channels()[c].model.rho_bar(this->tx_power);

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

    return std::max(mean, this->rho_hat(c, this->tx_power));
}

scheduled_frame_t LCS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;

    /* One slot of belief decay, for every channel -- including the rungs this
       scheduler is not watching, since the chains they model keep mixing
       whether anything is listening or not. */
    this->belief.age(this->get_channels());

    /* Judge the queue at the decode estimate of the rung the radio is on as
       the slot opens -- the channel a transmission this slot would most likely
       go out on, and the one every packet still queued was last costed
       against. The seat may yet climb below, which makes this an estimate one
       slot behind on the slots a packet needs; that is the same lag any
       estimate-driven count carries. */
    const double p_seat = this->rho_hat(this->ladder[this->seat], this->tx_power);

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
    this->seat = this->best_seat();

    const std::size_t chosen = this->ladder[this->seat];
    const double      rho    = this->rho_hat(chosen, this->tx_power);
    const double      mean   = this->get_channels()[chosen].model.rho_bar(this->tx_power);

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

            if (rho >= this->bar_to_beat(candidate)) break;

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

        uint64_t key = packet_key(next->id, next->id_count);

        /* Check if this frame is being transmitted for the first time */
        if (accumulated_prob.find(key) == accumulated_prob.end())
        {
            accumulated_prob[key] = rho;
        }
        else
        {
            /* Calculate the accumulated prob after this transmission */
            accumulated_prob[key] =
                accumulated_prob[key] + rho - accumulated_prob[key] * rho;
        }

        /* Check if the prob is high enough to remove this frame from the buffer */
        if (accumulated_prob[key] >=
            std::pow(next->reliability_req, 1.0 / next->frames))
        {
            scheduled_frame.remove_from_buffer = true;
            accumulated_prob.erase(key);
        }
        else
        {
            scheduled_frame.remove_from_buffer = false;
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
       an ablation arm does not overwrite the full scheduler's. */
    std::string name = "LCS_" + std::to_string(this->tx_power) + "W";

    if (!this->early_drop) name += "_nodrop";

    return name;
}
