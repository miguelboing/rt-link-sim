#include <iostream>
#include <numeric>

#include "edf_scheduler.hpp"
#include "schedulers/policies/edf_queue.hpp"

EDF_scheduler::EDF_scheduler(unsigned int tx_power, std::size_t channel, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick): BaseScheduler(buffer, sys_tick), tx_power(tx_power), channel(channel) {};

scheduled_frame_t EDF_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->channel;

    packet_t* next = select_earliest_deadline(*this->buffer_packet);

    if (next != nullptr)
    {
        scheduled_frame.packet = next;
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

