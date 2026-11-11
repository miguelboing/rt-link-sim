#include <iostream>
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>

#include "charm_scheduler.hpp"

CHARM_scheduler::CHARM_scheduler(unsigned int tx_power, std::size_t channel, float belief_threshold,
                               BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick):
    BaseScheduler(buffer, sys_tick), tx_power(tx_power), channel(channel),
    belief_threshold(belief_threshold) {};

scheduled_frame_t CHARM_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.channel = this->channel;

    /* One slot of belief decay, for every channel. This scheduler's
       policy, so this scheduler ticks it -- the base holds the estimate and
       makes no assumption about what is made of it. */
    this->belief.age(this->get_channels());

    /* rho_hat_{c,j}(t): the decode estimate for this channel at this power,
       derived from the label last observed on it -- or from the channel's
       fitted stationary statistics while it has never been observed, which is
       what keeps a first transmission from being sized off a zero. */
    const double transmission_prob = this->rho_hat(this->channel, this->tx_power);

    /* Listen as soon as the estimate is no longer trusted. The decay rate is
       the believed state's own A_ss, so the interval that emerges is the one
       the channel's dynamics imply rather than a tuned clock. No urgency guard
       here, unlike CATS: the test is belief alone.

       Nothing is assigned on either side of this test: the ageing above and
       receive_state() below are the only two things that move belief. An
       RX slot that comes back with no usable label therefore buys none, and
       the scheduler listens again. */
    if (this->belief.stale(this->channel, this->belief_threshold))
    {
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet = nullptr;
    }
    else /* If it is not try to schedule a packet */
    {

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
            /* Nothing rankable to send, so spend the slot listening -- as
               CATS does on the same fallback. The refresh needs no statement
               here: the slot is an RX slot, so the observation it returns
               moves tau_c and belief follows from that. */
            scheduled_frame.packet = nullptr;
            scheduled_frame.radio_mode = RX_MODE;
        }
    }

    return scheduled_frame;
}

std::string CHARM_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "CHARM_" + std::to_string(this->tx_power) + "W";
}
