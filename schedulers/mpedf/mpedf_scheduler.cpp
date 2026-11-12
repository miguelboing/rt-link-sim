#include <cmath>
#include <numeric>

#include "mpedf_scheduler.hpp"
#include "schedulers/policies/edf_queue.hpp"

MPEDF_scheduler::MPEDF_scheduler(unsigned int tx_power, std::size_t channel, double mean_prob, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick): BaseScheduler(buffer, sys_tick), tx_power(tx_power), channel(channel), mean_prob(mean_prob) {};

scheduled_frame_t MPEDF_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->channel;

    /* No listening branch at all: this scheduler never listens, so every slot is a
       transmit opportunity. See the class comment. */

    packet_t* next = select_earliest_deadline(*this->buffer_packet);

    if (next != nullptr)
    {
        scheduled_frame.packet = next;
        scheduled_frame.radio_mode = TX_MODE;

        uint64_t key = packet_key(next->id, next->id_count);

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

std::string MPEDF_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "MPEDF_" + std::to_string(this->tx_power) + "W";
}
