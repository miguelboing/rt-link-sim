#include <iostream>
#include <algorithm>
#include <numeric>

#include "edf_scheduler.hpp"

EDF_scheduler::EDF_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick): BaseScheduler(buffer, sys_tick), tx_power(tx_power), channel(channel) {};

scheduled_frame_t EDF_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->channel;

    /* Find the packet with the earliest deadline */
    auto lowest_it = std::min_element(this->buffer_packet->begin(),
                                      this->buffer_packet->end(),
                                      [](const packet_t& a, const packet_t& b) {
                                          return a.deadline < b.deadline;
                                      });
    if (lowest_it != this->buffer_packet->end())
    {
        scheduled_frame.packet = &(*lowest_it);
        scheduled_frame.radio_mode = TX_MODE;
    }
    else
    {
        scheduled_frame.packet = nullptr; /* Means idle/no tranmission */
        scheduled_frame.radio_mode = IDLE;
    }


    return scheduled_frame;
}

std::string EDF_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "EDF_" + std::to_string(this->tx_power) + "W";
}

