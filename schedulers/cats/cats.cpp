#include <iostream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

#include "cats.hpp"


CATS_scheduler::CATS_scheduler(unsigned int frequency,
                               float belief_threshold,
                               double utilization_threshold,
                               const std::vector<periodic_task_t>& periodic_tasks,
                               BufferPacket* buffer,
                               std::shared_ptr<unsigned int> sys_tick):
    BaseScheduler(buffer, sys_tick),
    frequency(frequency),
    belief_threshold(belief_threshold),
    periodic_tasks(periodic_tasks),
    horizon_H(0U),
    utilization_threshold(utilization_threshold),
    max_power_idx(2U)
{
    this->transmission_prob[0] = this->transmission_prob[1] = this->transmission_prob[2] = 0.0;
    this->belief = 0.0;
    this->eigenvalue = BELIEF_DECAY;

    /* Horizon = LCM of all task periods. Demand within one hyperperiod is
       exact (every task completes an integer number of releases), so the
       utilization estimate is tighter than max(T_i). */
    for (const auto& t : this->periodic_tasks)
    {
        if (t.period == 0U) continue;
        this->horizon_H = (this->horizon_H == 0U) ? t.period
                                                  : std::lcm(this->horizon_H, t.period);
    }
}

double CATS_scheduler::retx_count_required(double p, double sr_req)
{
    /* P(success in k attempts) = 1 - (1-p)^k >= sr_req
       => k >= log(1 - sr_req) / log(1 - p). */
    if (sr_req <= 0.0) return 1.0;             /* still need to send the frame once */
    if (p >= 1.0)      return 1.0;
    if (p <= 0.0)      return std::numeric_limits<double>::infinity();
    if (sr_req >= 1.0) return std::numeric_limits<double>::infinity();

    const double k = std::log(1.0 - sr_req) / std::log(1.0 - p);
    return std::ceil(k);
}

double CATS_scheduler::compute_demand_slots(double p) const
{
    if (this->horizon_H == 0U) return 0.0;

    double demand = 0.0;
    for (const auto& t : this->periodic_tasks)
    {
        if (t.period == 0U) continue;
        const double k = retx_count_required(p, std::pow(t.reliability_req,
                                                         1.0 / t.frames));
        if (std::isinf(k)) return std::numeric_limits<double>::infinity();
        /* releases of task i within H: ceil(H / T_i) */
        const double releases = std::ceil(static_cast<double>(this->horizon_H) /
                                          static_cast<double>(t.period));
        demand += releases * static_cast<double>(t.frames) * k;
    }
    return demand;
}

double CATS_scheduler::compute_utilization(double p) const
{
    if (this->horizon_H == 0U) return 0.0;
    return this->compute_demand_slots(p) / static_cast<double>(this->horizon_H);
}

scheduled_frame_t CATS_scheduler::do_schedule_frame(void)
{
    scheduled_frame_t scheduled_frame;
    scheduled_frame.frequency = this->frequency;

    /* Drop unfeasible packets and detect urgent ones in the same pass.
       Per-frame slot cost is the number of retransmissions needed to hit
       the per-frame RR target (pow(RR, 1/frames)) at the best channel
       probability we'll actually allow (transmission_prob[max_power_idx]) —
       the cap is a hard energy limit, so feasibility past it is moot.
       Pre-prediction (p_best == 0) we fall back to 1 slot/frame so packets
       aren't all dropped before the first prediction lands. */
    const double p_best = this->transmission_prob[this->max_power_idx];
    std::vector<std::pair<unsigned int, unsigned int>> to_drop;
    bool no_urgent_packet = true;
    for (auto& pkt : *this->buffer_packet)
    {
        if (pkt.deadline <= *(this->system_tick)) continue; /* already expired, check_deadlines handles it */
        const double ticks_available  = static_cast<double>(pkt.deadline - *(this->system_tick));
        const unsigned int remaining_frames = pkt.frames - pkt.frame_count;
        const double rr_per_frame = std::pow(pkt.reliability_req, 1.0 / pkt.frames);
        const double slots_per_frame = (p_best <= 0.0) ? 1.0
                                                       : retx_count_required(p_best, rr_per_frame);
        const double needed_slots = static_cast<double>(remaining_frames) * slots_per_frame;
        if (std::isinf(needed_slots) || ticks_available < needed_slots)
        {
            to_drop.emplace_back(pkt.id, pkt.id_count);
        }
        else if (ticks_available <= needed_slots)
        {
            no_urgent_packet = false;
        }
    }
    for (auto& [id, id_count] : to_drop)
    {
        accumulated_prob.erase(packet_key(id, id_count));
        this->buffer->drop_packet(id, id_count);
    }

    /* Listen only if we don't trust the channel AND no packet is on the edge of its deadline */
    if (this->belief < this->belief_threshold && no_urgent_packet)
    {
        scheduled_frame.radio_mode = RX_MODE;
        scheduled_frame.packet = nullptr;
        this->belief = 1.0;
    }
    else /* If it isn't, try to schedule a packet */
    {
        /* Belief drops */
        this->belief *= this->eigenvalue;

        /* Find the packet with the earliest deadline */
        auto lowest_it = std::min_element(this->buffer_packet->begin(),
                                          this->buffer_packet->end(),
                                          [](const packet_t& a, const packet_t& b) {
                                              return a.deadline < b.deadline;
                                          });

        if (lowest_it != this->buffer_packet->end())
        {
            const unsigned int power_levels[3] = {1, 10, 25};
            uint64_t key     = packet_key(lowest_it->id, lowest_it->id_count);
            double req       = std::pow(lowest_it->reliability_req,
                                        1.0 / lowest_it->frames);

            /* Get or initialise accumulated probability for this packet instance */
            double acc = (accumulated_prob.find(key) != accumulated_prob.end())
                         ? accumulated_prob[key] : 0.0;

            /* max_power_idx is set by receive_prediction based on the slack
               policy: pick the lowest tier whose predicted U fits below the
               threshold, then cap the selection loop there to save energy. */
            int chosen_idx = -1;
            for (unsigned int i = 0U; i <= this->max_power_idx; i++)
            {
                double acc_after = acc + transmission_prob[i] - acc * transmission_prob[i];
                if (acc_after >= req)
                {
                    chosen_idx = static_cast<int>(i);
                    break;
                }
            }

            if (chosen_idx >= 0)
            {
                /* Accumulated probability meets requirement — remove from buffer */
                accumulated_prob[key] = acc + transmission_prob[chosen_idx]
                                        - acc * transmission_prob[chosen_idx];
                scheduled_frame.transmission_power = power_levels[chosen_idx];
                scheduled_frame.remove_from_buffer = true;
                accumulated_prob.erase(key);
            }
            else
            {
                /* No allowed tier clears the requirement yet — burn the cap
                   tier and keep retransmitting on the next slot. */
                accumulated_prob[key] = acc + transmission_prob[this->max_power_idx]
                                        - acc * transmission_prob[this->max_power_idx];
                scheduled_frame.transmission_power = power_levels[this->max_power_idx];
                scheduled_frame.remove_from_buffer = false;
            }

            scheduled_frame.packet     = &(*lowest_it);
            scheduled_frame.radio_mode = TX_MODE;
        }
        else
        {
            scheduled_frame.packet = nullptr;
            scheduled_frame.radio_mode = RX_MODE;
            this->belief = 1.0;
        }
    }

    return scheduled_frame;
}

void CATS_scheduler::receive_prediction(const std::vector<double>& pred_probs)
{
    std::copy(pred_probs.begin(), pred_probs.end(), this->transmission_prob);

    /* Slack-aware power cap: walk the predictor tiers low-to-high and pick the
       first one whose predicted utilization fits below the threshold. If even
       the highest tier doesn't fit (overloaded schedule), fall back to it so
       behavior degrades gracefully. */
    unsigned int new_cap = 2U;
    for (unsigned int i = 0U; i < 3U; i++)
    {
        const double U = this->compute_utilization(this->transmission_prob[i]);
        if (U <= this->utilization_threshold)
        {
            new_cap = i;
            break;
        }
    }
    this->max_power_idx = new_cap;
}

std::string CATS_scheduler::get_name() const
{
    return "CATS Scheduler";
}

