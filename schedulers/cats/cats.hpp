#pragma once

#include <memory>
#include <vector>

#include "schedulers/base_scheduler.hpp"
#include "packet_generators/base_packet_generator.hpp"
#include <unordered_map>

/* CATS, with its three policies individually switchable so each can be
   ablated against the full scheduler (the `cats_comparison` sweep mode).
   All three default to enabled, which is the published CATS; a config that
   sets none of them behaves exactly as before.

   The three are independent by construction:
     - adaptive_power off pins both the transmitted power *and* the power used
       for demand/infeasibility/urgency to one tier, so no adaptive cap
       survives anywhere in the maths;
     - early_drop off stops infeasible packets being removed but leaves them
       classified as infeasible, so they do not silently become "urgent" and
       suppress listening;
     - urgency_check off removes only the urgency term from the listening
       condition, leaving belief, power selection and dropping untouched.
   The belief update (reset on RX, decay by BELIEF_DECAY on TX) and the cached
   predictor estimates are identical in every combination. */
class CATS_scheduler : public BaseScheduler
{
public:
    /* Predictor tiers, low to high. Indices into transmission_prob. */
    static constexpr unsigned int N_POWERS = 3U;
    static constexpr unsigned int POWER_LEVELS[N_POWERS] = {1U, 10U, 25U};

    CATS_scheduler(unsigned int frequency,
                   float belief_threshold,
                   double utilization_threshold,
                   const std::vector<periodic_task_t>& periodic_tasks,
                   bool adaptive_power,
                   unsigned int fixed_tx_power,
                   bool early_drop,
                   bool urgency_check,
                   BufferPacket* buffer,
                   std::shared_ptr<unsigned int> sys_tick);

    unsigned int frequency;

    /* --- Ablation switches ------------------------------------------------ */

    /* When false, every power decision collapses to fixed_power_idx: the
       selection loop considers only that tier and receive_prediction() leaves
       the cap alone, so p_best is that tier's probability too. */
    bool adaptive_power;
    unsigned int fixed_power_idx;   /* index into POWER_LEVELS; unused when adaptive */

    /* When false, packets classified infeasible stay in the buffer until the
       deadline expires or the reliability rule retires them. Classification
       still happens -- it is what keeps urgency well defined. */
    bool early_drop;

    /* When false, the listening condition drops its "and no urgent packet"
       term. The empty-buffer listen is unaffected either way. */
    bool urgency_check;

    float belief;
    float belief_threshold;
    float eigenvalue;

    double transmission_prob[3];
    std::unordered_map<uint64_t, double> accumulated_prob;

    /* A-priori knowledge of the periodic task set (populated at construction).
       Used to size demand/utilization estimates against a fixed horizon. */
    std::vector<periodic_task_t> periodic_tasks;

    /* Utilization horizon — hyperperiod (LCM of periodic_tasks periods).
       Demand within one hyperperiod is exact since every task completes an
       integer number of releases. Zero when no periodic tasks exist. */
    unsigned int horizon_H;

    /* Slack-aware power-cap policy. On every channel prediction we recompute
       U at each predictor tier and cap the TX power at the lowest tier whose
       U <= utilization_threshold. Default cap is 25 W (index 2) so behavior
       before the first prediction is unchanged. */
    double utilization_threshold;
    unsigned int max_power_idx;

    scheduled_frame_t do_schedule_frame(void) override;

    void receive_prediction(const std::vector<double>& pred_probs) override;

    std::vector<unsigned int> get_prediction_powers() const override { return {1, 10, 25}; }

    std::string get_name() const override;

    /* Minimum number of transmissions per frame to achieve sr_req given a
       per-attempt success probability p. Returns +inf if p<=0<sr_req. */
    static double retx_count_required(double p, double sr_req);

    /* Demand in slots within the horizon H, given a per-frame success
       probability p. Demand_i = ceil(H/T_i) * C_i * retx_count_required(p, RR_i).
       Returns +inf if any task is infeasible at p. */
    double compute_demand_slots(double p) const;

    /* Utilization U = compute_demand_slots(p) / H. Returns +inf if infeasible,
       0.0 when there are no periodic tasks. */
    double compute_utilization(double p) const;
};

