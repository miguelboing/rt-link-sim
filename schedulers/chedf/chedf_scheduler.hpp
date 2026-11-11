#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include "schedulers/base_scheduler.hpp"

/* CHEDF -- CHARM's channel-aware retransmission/power policy over an EDF
   queue.

   Listening is belief-driven, exactly as in CHARM: confidence in the last
   prediction is restored by a listen and decays on every transmitting slot by
   the self-transition probability A_ss of the state it believes the channel is
   in, read off its own copy of the channel's fitted FSMC, and the scheduler
   listens once it drops below belief_threshold. There is no `rx_period` -- see
   schedulers/charm/charm_scheduler.hpp for why the fixed interval was
   replaced and why no urgency guard is applied.

   Standalone by design: it stands on its own as a scheduler rather than as a
   parameterisation of CHARM. The consequence is that the retransmission rule
   below is a deliberate copy of CHARM's, not a shared one -- the CHARM/CHEDF
   comparison is only meaningful while the two differ solely in dequeue order,
   so any change to the accumulated-probability rule, the belief-driven
   listening rule or get_prediction_powers() must be applied to both files. */
class CHEDF_scheduler : public BaseScheduler
{
public:
    CHEDF_scheduler(unsigned int tx_power, unsigned int frequency, float belief_threshold,
                    std::optional<ChannelModel> model, BufferPacket* buffer,
                    std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;
    unsigned int frequency;

    /* Confidence that the last prediction still describes the channel: 1
       immediately after a listen, decaying by the believed state's A_ss each
       slot spent transmitting. Starts at 0 so the first slot is always a
       listen. */
    float belief;
    float belief_threshold;

    /* The scheduler's own copy of the channel's fitted FSMC -- its estimated
       model, which supplies both the belief decay rate and the decode
       probability below. Empty for a channel that has none, which is the only
       case that still takes a probability from the predictor. */
    std::optional<ChannelModel> model;

    /* s_hat: the last model state the predictor labelled, or -1 before the
       first observation. The decode estimate is read off the model at this
       state rather than handed over ready-made. */
    int believed_state;

    double transmission_prob;
    std::unordered_map<uint64_t, double> accumulated_prob;

    scheduled_frame_t do_schedule_frame(void) override;

    void receive_prediction(const std::vector<double>& pred_probs) override;

    /* The predictor hands over a state label; the decode probability comes
       from this scheduler's own response curve for that state. */
    void receive_state(int state) override;
    bool wants_state_prediction() const override { return this->model.has_value(); }
    /* CHEDF transmits at a single fixed power, so it only ever needs the
       decode probability at that power. */
    std::vector<unsigned int> get_prediction_powers() const override { return {this->tx_power}; }

    std::string get_name() const override;
};
