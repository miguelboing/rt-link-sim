#include <numeric>

#include "rate_monotonic_scheduler.hpp"
#include "schedulers/policies/rm_queue.hpp"

RM_scheduler::RM_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer,
                           std::shared_ptr<unsigned int> sys_tick, std::vector<channel_estimate_t> channels):
    BaseScheduler(buffer, sys_tick, std::move(channels)), tx_power(tx_power), channel(channel) {};

scheduled_frame_t RM_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->channel;

    packet_t* next = select_shortest_period(*this->buffer_packet);

    /* An aperiodic packet has no period to rank by, so it only ever comes back
       when nothing periodic is queued. This scheduler declines to send it and
       idles instead: ranking by period is the whole of its policy, and it has
       no rule for a packet that policy cannot order. */
    if (next != nullptr && next->is_periodic)
    {
        scheduled_frame.packet = next;
        scheduled_frame.radio_mode = TX_MODE;
    }
    else
    {
        scheduled_frame.packet = nullptr; /* Means idle/no transmission */
        scheduled_frame.radio_mode = IDLE;
    }

    return scheduled_frame;
}

std::string RM_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "Rate_M_" + std::to_string(this->tx_power) + "W";
}
