#include <iostream>
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>

#include "charm_scheduler.hpp"

CHARM_scheduler::CHARM_scheduler(unsigned int tx_power, unsigned int frequency, float belief_threshold,
                               std::optional<ChannelModel> model, BufferPacket* buffer,
                               std::shared_ptr<unsigned int> sys_tick):
    BaseScheduler(buffer, sys_tick), tx_power(tx_power), frequency(frequency),
    belief(0.0f), belief_threshold(belief_threshold), model(std::move(model)),
    believed_state(-1), transmission_prob(0)
{
    /* Before the first observation the state label is unavailable, so the
       estimate to stand in for it is the model's stationary average -- the
       long-run decode probability at this power. Belief starts at 0, so the
       first slot is a listen and this value is replaced before it can size a
       retransmission; it is here so the scheduler is never holding a
       probability of zero, which no channel has. */
    if (this->model.has_value())
        this->transmission_prob = this->model->rho_bar(this->tx_power);
};

scheduled_frame_t CHARM_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.transmission_power = this->tx_power;
    scheduled_frame.frequency = this->frequency;

    /* Listen as soon as the estimate is no longer trusted. Belief decays at
       the rate the believed state is expected to persist, so this replaces the
       fixed rx_period interval with one the channel's dynamics set. No urgency
       guard here, unlike CATS: the test is belief alone. */
    if (this->belief < this->belief_threshold)
    {
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet = nullptr;
        this->belief = 1.0f;
    }
    else /* If it is not try to schedule a packet */
    {
        /* Belief drops: one more slot since the estimate was refreshed. The
           factor is the believed state's own A_ss, so what decays is the
           model probability the channel has not left that state. */
        this->belief *= belief_decay(this->model, this->believed_state);

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
            /* Nothing rankable to send, so spend the slot listening. That is
               a real refresh, so the belief this slot just decayed is
               restored -- as CATS does on the same fallback. */
            scheduled_frame.packet = nullptr;
            scheduled_frame.radio_mode = RX_MODE;
            this->belief = 1.0f;
        }
    }

    return scheduled_frame;
}

void CHARM_scheduler::receive_prediction(const std::vector<double>& pred_probs)
{
    /* Only reached for a channel with no estimated model to read a state label
       in; otherwise receive_state() below is called instead. */
    this->transmission_prob = pred_probs[0]; /* Only tx_power is predicted */
}

void CHARM_scheduler::receive_state(int state)
{
    /* One observation, one label -- and the decode estimate is derived from it
       here rather than delivered ready-made. The scheduler owns the response
       curves, so this is where rho_hat_{s,j} comes from, and a wrong label
       moves every power level together. At a single fixed power that is one
       number; see CATS for the whole row.

       A label of -1 means the predictor had nothing to offer (an unknown
       frequency). Keep the previous estimate rather than inventing one: the
       slot is already spent and the belief reset is what matters. */
    if (!this->model.has_value() || state < 0) return;

    this->believed_state = state;
    this->transmission_prob = this->model->rho(static_cast<std::size_t>(state),
                                               this->tx_power);
}

std::string CHARM_scheduler::get_name() const {
    /* Power is part of the name so runs at different tx_power don't overwrite
       each other's scheduled-packet logs. */
    return "CHARM_" + std::to_string(this->tx_power) + "W";
}
