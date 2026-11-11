#pragma once

#include <memory>

#include <optional>

#include "schedulers/base_scheduler.hpp"
#include <unordered_map>

/* CHARM -- accumulated-probability retransmission over a period-ordered
   queue, listening on CATS's belief rule rather than on a fixed interval.

   Listening policy. CHARM as published gives up every rx_period-th slot to
   RX_MODE whatever the channel is doing. That is a fixed-interval stand-in
   for the question actually being asked -- "is the estimate I am holding
   still worth anything?" -- so it is replaced here by the belief test CATS
   uses: confidence is restored to 1 by a listen and decays on every slot
   spent transmitting, and the scheduler listens as soon as it falls below
   belief_threshold. The interval that emerges is the one the channel's own
   dynamics imply, so it is no longer a parameter to tune: `rx_period` is gone
   and `belief_threshold` takes its place.

   What it decays by is the scheduler's own model. Holding a copy of the
   channel's fitted FSMC, it knows the believed state's self-transition
   probability A_ss, so confidence after d transmitting slots is A_ss^d -- the
   model probability the channel has not left that state, with 1 - A_ss the
   per-slot probability of leaving. That is per channel *and* per state: the
   same scheduler refreshes after 18 slots in a state the chain holds for 50
   and after 178 in one it holds for 500. Only a channel with no model at all
   falls back to the BELIEF_DECAY constant.

   Unlike CATS this does *not* carry the `no_urgent_packet` guard -- it
   listens purely on belief, so a listen can consume a slot a near-deadline
   packet needed. That is a deliberate simplification: it keeps the one
   difference from CATS's listening policy at zero, at the cost of CHARM
   being able to miss a deadline that periodic listening would have made.

   Everything else is unchanged: single fixed power, and a packet is retired
   once its accumulated decode probability clears its per-frame requirement.

   Keep in sync with schedulers/chedf/, which is this policy over an EDF
   queue and differs only in the min_element comparator. */
class CHARM_scheduler : public BaseScheduler
{
public:
    CHARM_scheduler(unsigned int tx_power, std::size_t channel, float belief_threshold,
                    std::optional<ChannelModel> model, BufferPacket* buffer,
                    std::shared_ptr<unsigned int> sys_tick);

    unsigned int tx_power;

    /* c: the one channel this scheduler transmits on. Single-channel for now;
       the choice becomes per-slot once the scheduler reasons over the whole
       channel set. */
    std::size_t channel;

    /* Confidence that the last prediction still describes the channel: 1
       immediately after a listen, decaying by the believed state's A_ss each
       slot spent transmitting. Starts at 0 so the first slot is always a
       listen, which is what stops the scheduler transmitting on an unset
       estimate. */
    float belief;
    float belief_threshold;

    /* The scheduler's own copy of the channel's fitted FSMC -- its estimated
       model, which supplies both the decay rate above and the decode
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
    /* CHARM transmits at a single fixed power, so it only ever needs the
       decode probability at that power. */
    std::vector<unsigned int> get_prediction_powers() const override { return {this->tx_power}; }

    std::string get_name() const override;
};
