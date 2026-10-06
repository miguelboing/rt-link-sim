#include <algorithm>
#include <cmath>
#include <numeric>

#include "mprm_scheduler.hpp"

MPRM_scheduler::MPRM_scheduler(unsigned int tx_power, unsigned int frequency, double mean_prob, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick): BaseScheduler(buffer, sys_tick), tx_power(tx_power), frequency(frequency), mean_prob(mean_prob) {};

scheduled_frame_t MPRM_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.frequency = this->frequency;

    /* No rx_period branch: this scheduler never listens, so every slot is a
       transmit opportunity. See the class comment. */

    /* Find the packet with the smaller period */
    auto lowest_it = std::min_element(this->buffer_packet->begin(),
                                      this->buffer_packet->end(),
                                      [](const packet_t& a, const packet_t& b) {
                                          if (!a.is_periodic) return false;
                                          if (!b.is_periodic) return true;
                                          return a.period < b.period;
                                      });

    if (lowest_it != this->buffer_packet->end() && lowest_it->is_periodic)
    {
        scheduled_frame.packet = &(*lowest_it);
        scheduled_frame.radio_mode = TX_MODE;

        uint64_t key = packet_key(lowest_it->id, lowest_it->id_count);

        /* Check if this frame is being transmitted for the first time */
        if (accumulated_prob.find(key) == accumulated_prob.end())
        {
            accumulated_prob[key] = this->mean_prob;
        }
        else
        {
            /* Calculate the accumulated prob after this transmission */
            accumulated_prob[key] =
                accumulated_prob[key] + this->mean_prob - accumulated_prob[key] * this->mean_prob;
        }

        /* Check if the prob is high enough to remove this frame from the buffer */
        if (accumulated_prob[key] >=
            std::pow(lowest_it->reliability_req, 1.0 / lowest_it->frames))
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
        /* Nothing rankable in the buffer. CHARM drops into RX_MODE here to
           refresh its prediction; a static estimate has nothing to listen
           for, so the slot idles as it would under RM. */
        scheduled_frame.packet = nullptr;
        scheduled_frame.radio_mode = IDLE;
    }

    return scheduled_frame;
}

std::string MPRM_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "MPRM_" + std::to_string(this->tx_power) + "W";
}
