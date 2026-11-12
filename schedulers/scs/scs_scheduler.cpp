#include <cmath>
#include <iostream>
#include <numeric>

#include "scs_scheduler.hpp"
#include "schedulers/policies/edf_queue.hpp"

SCS_scheduler::SCS_scheduler(unsigned int tx_power, BufferPacket* buffer,
                             std::shared_ptr<unsigned int> sys_tick,
                             std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)), tx_power(tx_power)
{
    this->select_channel();
};

void SCS_scheduler::select_channel(void)
{
    double best = -1.0;

    for (std::size_t c = 0; c < this->n_channels(); c++)
    {
        /* With no observation recorded, rho_hat is the channel's fitted
           stationary mean -- which is the whole estimate this scheduler ever
           has, since it never listens. */
        const double mean = this->rho_hat(c, this->tx_power);

        if (mean > best)
        {
            best = mean;
            this->selected = c;
        }
    }

    /* A non-positive best mean can never accumulate to a reliability target,
       so every packet would be retransmitted until its deadline. That is a
       mis-specified set of channels rather than a policy choice, so say so
       instead of silently saturating the link. */
    if (best <= 0.0)
        std::cerr << "WARNING: no channel has a positive mean decode probability at "
                  << this->tx_power << "W - no packet can reach its reliability"
                     " target." << std::endl;
}

scheduled_frame_t SCS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->selected;

    /* The decode estimate for the chosen channel. Constant for the whole run:
       nothing observes, so nothing moves it. */
    const double mean_prob = this->rho_hat(this->selected, this->tx_power);

    /* No listening branch: every slot is a transmit opportunity or idle. */
    packet_t* next = select_earliest_deadline(*this->buffer_packet);

    if (next != nullptr)
    {
        scheduled_frame.packet = next;
        scheduled_frame.radio_mode = TX_MODE;

        uint64_t key = packet_key(next->id, next->id_count);

        /* Check if this frame is being transmitted for the first time */
        if (accumulated_prob.find(key) == accumulated_prob.end())
        {
            accumulated_prob[key] = mean_prob;
        }
        else
        {
            /* Calculate the accumulated prob after this transmission */
            accumulated_prob[key] =
                accumulated_prob[key] + mean_prob - accumulated_prob[key] * mean_prob;
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
        /* Empty buffer. A static estimate has nothing to listen for, so the
           slot idles rather than being spent on an RX slot. */
        scheduled_frame.packet = nullptr;
        scheduled_frame.radio_mode = IDLE;
    }

    return scheduled_frame;
}

std::string SCS_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "SCS_" + std::to_string(this->tx_power) + "W";
}
