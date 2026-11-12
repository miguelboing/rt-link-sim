#pragma once

#include <memory>
#include <vector>

#include "schedulers/base_scheduler.hpp"
#include "schedulers/policies/belief_tracker.hpp"
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
   The belief update (reset on an observation, decay by the believed state's
   A_ss every slot) is identical in every combination.

   The predictor hands over a model-state label, not probabilities. CATS holds
   its own copy of the channel's fitted FSMC and reads the whole row
   rho_hat_{s,.} off it, so one observation refreshes all three power tiers
   together from the same state -- which is also why a mislabelled state moves
   the power selection, the demand estimate and the urgency test at once. */
class CATS_scheduler : public BaseScheduler
{
public:
    /* Predictor tiers, low to high. Indices into transmission_prob. */
    static constexpr unsigned int N_POWERS = 3U;
    static constexpr unsigned int POWER_LEVELS[N_POWERS] = {1U, 10U, 25U};

    CATS_scheduler(std::size_t channel,
                   float belief_threshold,
                   double utilization_threshold,
                   const std::vector<periodic_task_t>& periodic_tasks,
                   bool adaptive_power,
                   unsigned int fixed_tx_power,
                   bool early_drop,
                   bool urgency_check,
                   BufferPacket* buffer,
                   std::shared_ptr<unsigned int> sys_tick);

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;

    /* --- Ablation switches ------------------------------------------------ */

    /* When false, every power decision collapses to fixed_power_idx: the
       selection loop considers only that tier and update_power_cap() leaves
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

    /* Belief in the label last heard on each channel, and the floor below
       which CATS gives up a slot to refresh it. The listening policy is CATS's,
       not the base's: the base holds the estimate, this holds what is made of
       it. Shared with CHEDF by composition -- see
       schedulers/policies/belief_tracker.hpp. Neither the state nor the
       model nor the row of decode estimates is a member any more; those are
       the estimate, and they live in the channel set. */
    BeliefTracker belief;
    float belief_threshold;
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

    /* One state label refreshes all three tiers at once, because all three
       are read off the same label by prob_at(). Overridden only to recompute
       the power cap: the base's own job is to record the observation. */
    void receive_state(std::size_t c, int state) override;

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

private:
    /* rho_hat_{c,j}(t) at tier j on the channel this scheduler transmits on.
       Derived from the label last observed there, so the three tiers always
       come from one state and one age -- which is why a mislabelled state
       moves power selection, demand and urgency together rather than one of
       them. Before the first observation it is the channel's fitted
       stationary average, so no tier ever reads as flatly impossible. */
    double prob_at(unsigned int j) const
    {
        return this->rho_hat(this->channel, POWER_LEVELS[j]);
    }

    /* Slack-aware power cap, recomputed whenever the estimates move: the
       lowest tier whose predicted U fits under the threshold. Shared by both
       prediction paths so the cap cannot depend on which one delivered the
       estimates. No-op in fixed-power mode, where the cap *is* the fixed
       tier. */
    void update_power_cap(void);
};

