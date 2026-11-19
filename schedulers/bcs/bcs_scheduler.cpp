#include <cmath>
#include <numeric>
#include <utility>

#include "bcs_scheduler.hpp"
#include "schedulers/policies/channel_choice.hpp"
#include "schedulers/policies/edf_queue.hpp"

BCS_scheduler::BCS_scheduler(unsigned int tx_power, float belief_threshold,
                             std::size_t n_usable,
                             BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick,
                             std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)), tx_power(tx_power),
    belief_threshold(belief_threshold)
{
    this->usable_channels = rank_channels_by_mean(this->get_channels(), this->tx_power);

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

std::size_t BCS_scheduler::best_channel(void) const
{
    std::size_t best     = this->usable_channels.front();
    double      best_rho = this->rho_hat(best, this->tx_power);

    for (std::size_t c : this->usable_channels)
    {
        const double rho = this->rho_hat(c, this->tx_power);

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

scheduled_frame_t BCS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;

    /* One slot of belief decay, for every channel -- including the ones this
       slot will not touch, which is the whole point of keeping estimates of
       channels the radio is not on. */
    this->belief.age(this->get_channels());

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

    const std::size_t chosen = this->best_channel();
    scheduled_frame.channel = chosen;

    /* rho_hat_{c,j}(t) for the channel just chosen: read off the label last
       observed there, or off that channel's fitted stationary statistics if it
       has never been observed -- though with every belief above the threshold
       that second case cannot arise here, since an unobserved channel sits at
       belief 0. */
    const double transmission_prob = this->rho_hat(chosen, this->tx_power);

    packet_t* next = select_earliest_deadline(*this->buffer_packet);

    if (next != nullptr)
    {
        scheduled_frame.packet     = next;
        scheduled_frame.radio_mode = TX_MODE;

        uint64_t key = packet_key(next->id, next->id_count);

        /* Check if this frame is being transmitted for the first time */
        if (accumulated_prob.find(key) == accumulated_prob.end())
        {
            accumulated_prob[key] = transmission_prob;
        }
        else
        {
            /* Calculate the accumulated prob after this transmission */
            accumulated_prob[key] =
                accumulated_prob[key] + transmission_prob - accumulated_prob[key] * transmission_prob;
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
       otherwise all write to the one file. The full set keeps the plain name. */
    const std::string power = std::to_string(this->tx_power) + "W";

    return this->usable_channels.size() == this->n_channels()
           ? "BCS_" + power
           : "TCS" + std::to_string(this->usable_channels.size()) + "_" + power;
}
