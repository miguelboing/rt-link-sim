#include <algorithm>
#include <optional>
#include <iostream>

#include "system_model/system_model.hpp"

#include "transmitter.hpp"

Transmitter::Transmitter(std::shared_ptr<std::vector<packet_t>> buffer_packet):
    buffer_packet(buffer_packet) {};

std::optional<transmitted_frame_t> Transmitter::transmit_frame(const scheduled_frame_t& scheduled_frame)
{
    transmitted_frame_t transmitted_frame;
    transmitted_frame.transmission_power = scheduled_frame.transmission_power;
    transmitted_frame.channel = scheduled_frame.channel;

    if (scheduled_frame.packet == nullptr)
    {
        transmitted_frame.packet = {0, 0, 0, 0, 0, 0};

        return transmitted_frame;
    }

    transmitted_frame.packet = *(scheduled_frame.packet);

    /* Find packet on the buffer */
    auto packet_it = std::find_if(this->buffer_packet->begin(), this->buffer_packet->end(),
        [&scheduled_frame](const packet_t& p) { return &p == scheduled_frame.packet;});

    if (packet_it != this->buffer_packet->end())
    {
        if (packet_it->frames > packet_it->frame_count) /* Check if this packet is valid */
        {
            if (scheduled_frame.remove_from_buffer) /* Remove packet from buffer */
            {
                packet_it->frame_count++;
            }
        }
        else
        {
            std::cout << "ERROR: Packet is invalid frame_count > frames" << std::endl;
            return std::nullopt;
        }
    }
    else
    {
        std::cout << "ERROR: Couldn't find the packet" << std::endl;
        return std::nullopt;
    }

    return transmitted_frame;
}

