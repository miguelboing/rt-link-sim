#include <iostream>
#include <algorithm>
#include <numeric>

#include "chedf_scheduler.hpp"

CHEDF_scheduler::CHEDF_scheduler(unsigned int tx_power, unsigned int frequency, float belief_threshold, BufferPacket* buffer, std::shared_ptr<unsigned int> sys_tick): BaseScheduler(buffer, sys_tick), tx_power(tx_power), frequency(frequency), belief(0.0f), belief_threshold(belief_threshold), transmission_prob(0) {};

scheduled_frame_t CHEDF_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.frequency = this->frequency;

    /* Listen as soon as the estimate is no longer trusted. Belief decays at
       the channel's own mixing rate, so this replaces the fixed rx_period
       interval with one the channel's dynamics set. No urgency guard here,
       unlike CATS: the test is belief alone. */
    if (this->belief < this->belief_threshold)
    {
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet = nullptr;
        this->belief = 1.0f;
    }
    else /* If it is not try to schedule a packet */
    {
        /* Belief drops: one more slot since the estimate was refreshed. */
        this->belief *= BELIEF_DECAY;

        /* Find the packet with the earliest deadline. This is the one line of
           policy that separates CHEDF from CHARM -- everything below is
           CHARM's retransmission rule, kept identical on purpose.

           Unlike CHARM's period comparator this needs no is_periodic guard: a
           deadline is well defined for aperiodic packets too, so every
           buffered packet is rankable and none has to be skipped. */
        auto lowest_it = std::min_element(this->buffer_packet->begin(),
                                          this->buffer_packet->end(),
                                          [](const packet_t& a, const packet_t& b) {
                                              return a.deadline < b.deadline;
                                          });

        if (lowest_it != this->buffer_packet->end())
        {
            scheduled_frame.packet = &(*lowest_it);
            scheduled_frame.radio_mode = TX_MODE;

            uint64_t key = packet_key(lowest_it->id, lowest_it->id_count);

            /* Check if this frame is being transmitted for the first time */
            if (accumulated_prob.find(key) == accumulated_prob.end())
            {
                accumulated_prob[key] = this->transmission_prob;
            }
            else
            {
                /* Calculate the accumulated prob after this transmission */
                accumulated_prob[key] =
                    accumulated_prob[key] + this->transmission_prob - accumulated_prob[key] * this->transmission_prob;
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
            /* Nothing to send, so spend the slot listening. That is a real
               refresh, so the belief this slot just decayed is restored -- as
               CATS does on the same fallback. */
            scheduled_frame.packet = nullptr;
            scheduled_frame.radio_mode = RX_MODE;
            this->belief = 1.0f;
        }
    }

    return scheduled_frame;
}

void CHEDF_scheduler::receive_prediction(const std::vector<double>& pred_probs)
{
    this->transmission_prob = pred_probs[0]; /* Only tx_power is predicted */
}

std::string CHEDF_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "CHEDF_" + std::to_string(this->tx_power) + "W";
}
